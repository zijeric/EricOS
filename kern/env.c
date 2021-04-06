#include "inc/x86.h"
#include "inc/mmu.h"
#include "inc/error.h"
#include "inc/string.h"
#include "inc/assert.h"
#include "inc/elf.h"

#include "kern/env.h"
#include "kern/pmap.h"
#include "kern/trap.h"
#include "kern/monitor.h"
#include "kern/macro.h"
#include "kern/dwarf_api.h"
#include "kern/sched.h"
#include "kern/cpu.h"
#include "kern/spinlock.h"

// 所有 Env 在内存（物理内存）中的存放是连续的，存放于 envs 处，可以通过数组的形式访问各个 Env
// envs 指向 Env 数组的指针，其操作方式跟内存管理的 pages 类似
// envs数组相当于Linux中的PCB(Process Control block)表
struct Env *envs = NULL;
// env_free_list 是空闲的环境结构链表(静态的)，相当于 page_free_list
// 简化环境的分配和释放，仅需要从该链表上添加或移除
static struct Env *env_free_list;
// (由 Env->env_link 链接所有空闲环境节点)

#define ENVGENSHIFT 12 // >= LOGNENV，支持最多"同时"执行 NENV 个用户环境

/**
 * 全局描述符表
 * 为内核态和用户态设置全局描述符表(GDT)实现分段，分段在x86上有很多用途
 * 虽然不使用分段的任何内存映射功能，但是需要它们来切换权限级别
 * 
 * 除了描述符权限级别(DPL, Descriptor Privilege Level)之外，内核段和用户段都是相同的
 * 但是要加载SS栈段寄存器，当前环境权限级别(CPL, Current Privilege Level)必须等于 DPL
 * 因此，必须为用户和内核复制代码段和数据段（权限不同），各自单独使用对应的段
 * 从而使只有内核才能访问内核栈，尽管段的基址base和段限制Limit相同
 * 
 * 特别地，在gdt[]定义中使用的SEG宏的最后一个参数指定了 DPL: 0表示内核，3表示用户
 */
struct Segdesc gdt[2 * NCPU + 5] = {
	// 0x0 - 未使用的(指向此处将总是导致错误 —— 用于捕获NULL指针)
	SEG_NULL,

	// 0x8  - 内核代码段，(GD_...>>3)转换成索引1,2,3...
	[GD_KT >> 3] = SEG64(STA_X | STA_R, 0x0, 0xffffffff, 0),

	// 0x10 - 内核数据段
	[GD_KD >> 3] = SEG64(STA_W, 0x0, 0xffffffff, 0),

	// 0x18 - 用户代码段
	[GD_UT >> 3] = SEG64(STA_X | STA_R, 0x0, 0xffffffff, 3),

	// 0x20 - 用户数据段
	[GD_UD >> 3] = SEG64(STA_W, 0x0, 0xffffffff, 3),

	// Per-CPU TSS 描述符 (从 GD_TSS0 开始计数) 在 trap_init_percpu() 中初始化.
	// 0x28 - tss, 在 trap_init_percpu() 中初始化 Per-CPU 的 TSS 描述符(从 GD_TSS0 开始)
	[GD_TSS0 >> 3] = SEG_NULL,

	// TSS 描述符的最后8字节(TSS 描述符有16个字节)
	[6] = SEG_NULL};

struct Pseudodesc gdt_pd = {
	sizeof(gdt) - 1, (unsigned long)gdt};

/**
 * 将 envid 转换为指向 Env 结构的指针.
 * 如果(checkperm==1)，envid 对应环境必须是当前环境或当前环境的子环境.(fork 必须设置为1)
 * 返回值:
 *   0 成功, -E_BAD_ENV 错误.
 *   成功, 设置 *env_store 为对应环境结构指针.
 *   错误, 设置 *env_store 为 NULL.
 */
int envid2env(envid_t envid, struct Env **env_store, bool checkperm)
{
	struct Env *e;

	// 如果 envid 是 0, 返回当前的运行的环境.
	if (envid == 0)
	{
		*env_store = curenv;
		return 0;
	}

	// 通过 envid 的索引部分查找环境结构，然后检查结构中的 env_id 字段，确保 envid 没有过时
	// (不引用在 envs[] 中使用相同索引的上一个环境).
	e = &envs[ENVX(envid)];
	if (e->env_status == ENV_FREE || e->env_id != envid)
	{
		*env_store = 0;
		return -E_BAD_ENV;
	}

	// 检查调用环境是否具有操作 envid 对应环境的权限.
	// 如果(checkperm==1)，envid 对应环境必须是当前环境或当前环境的子环境.
	if (checkperm && e != curenv && e->env_parent_id != curenv->env_id)
	{
		*env_store = 0;
		return -E_BAD_ENV;
	}

	*env_store = e;
	return 0;
}

/**
 * 函数功能：初始化NENV个Env结构体(envs数组)，将它们加入到空闲环境链表(构建env_free_list)，
 * 注意，函数将结构体插入空闲环境链表时，以逆序的方式插入(高->低)，envs[0] 在链表头部
 */
void env_init(void)
{
	struct Env *env = NULL;
	size_t i = 0;
	for (; i < NENV; i++)
	{
		envs[i].env_type = ENV_FREE;
		envs[i].env_id = 0;
		if (env)
		{
			env->env_link = &envs[i];
		}
		else
		{
			env_free_list = &envs[i];
		}
		env = &envs[i];
	}
	// Per-CPU part of the initialization
	// 为了区分用户态环境和内核态的访问权限：加载全局描述符表(GDT)，配置分段硬件为权限级别0(内核)和权限级别3(用户)使用单独的段
	env_init_percpu();
}

// 加载全局描述符 GDT，并且初始化段寄存器 gs, fs, es, ds, ss, cs
void env_init_percpu(void)
{
	// 1.加载新的gdt，为了内核的权限保护
	lgdt(&gdt_pd);

	// 2.初始化数据段寄存器GS、FS（留给用户数据段使用）、ES、DS、SS（在用户态和内核态切换使用），DPL: 段描述符权限级别
	asm volatile("movw %%ax,%%gs" ::"a"(GD_UD | DPL_USER));
	asm volatile("movw %%ax,%%fs" ::"a"(GD_UD | DPL_USER));
	asm volatile("movw %%ax,%%es" ::"a"(GD_KD));
	asm volatile("movw %%ax,%%ds" ::"a"(GD_KD));
	asm volatile("movw %%ax,%%ss" ::"a"(GD_KD));

	// 3.为了初始化内核的代码段寄存器cs，lretq重新加载cs到当前地址(lretq GD_KT,$1f 1:)
	asm volatile("pushq %%rbx \n \t movabs $1f,%%rax \n \t pushq %%rax \n\t lretq \n 1:\n" ::"b"(GD_KT)
				 : "cc", "memory");
	// 4.初始化LDT表为0，并未使用
	lldt(0);
}

//
// Initialize the kernel virtual memory layout for environment e.
// Allocate a page map level 4, set e->env_pml4e accordingly,
// and initialize the kernel portion of the new environment's address space.
// Do NOT (yet) map anything into the user portion
// of the environment's virtual address space.
//
// Returns 0 on success, < 0 on error.  Errors include:
//	-E_NO_MEM if page directory or table could not be allocated.
//
static int
env_setup_vm(struct Env *e)
{
	int r;
	int i;
	struct PageInfo *pp = NULL;

	// 分配环境4级页表
	if (!(pp = page_alloc(0)))
		return -E_NO_MEM;

	/**
	 * 由于每个用户环境共享内核空间
	 * 所以对于用户环境而言，在 UTOP 以上的虚拟地址空间对于所有环境来说都是相同的
	 * 因此在初始化 env_pml4e 的时候，只需要复制内核的部分进用户环境的地址空间就实现了页式映射的共享
	 * 不需要映射页表页的原因是，用户环境可以和内核共用这些页表页
	 * 
	 * 初始化新环境虚拟地址的内核部分：把 UTOP 以上的内容从 boot_pml4e(用户只读) 中复制到 env_pml4e 中
	 */
	// 让 e->env_pml4e 指向新分配的环境4级页表
	e->env_pml4e = (pml4e_t *)page2kva(pp);
	// env_cr3 保存 pa(新分配的环境4级页表)
	e->env_cr3 = page2pa(pp);
	pp->pp_ref++;
	size_t pml4e_map = 0;
	for (; pml4e_map < NPMLENTRIES; pml4e_map++)
	{
		// 初始化新环境虚拟地址的内核部分：把 UTOP 以上的内容从 boot_pml4e 中复制到 env_pml4e 中
		if (pml4e_map >= PML4(UTOP))
		{
			e->env_pml4e[pml4e_map] = boot_pml4e[pml4e_map] | (PTE_P & ~(PTE_W | PTE_U));
		}
	}

	// 将 UVPT虚拟地址 对应在4级页表中的表项 设置成4级页表自己的物理地址
	// 当用户环境访问4级页表，只要把对应的虚拟地址设置成 UVPT 即可
	// 配置新环境4级页表的 UVPT 映射 env 自己的页表为只读，无法被用户篡改
	// 权限: 内核 R-, 用户 R-
	e->env_pml4e[PML4(UVPT)] = e->env_cr3 | PTE_P | PTE_U;

	return 0;
}

/**
 * 向 env_free_list(Unix 进程表)申请空闲的环境，对新申请的环境的struct Env(Unix 进程描述符)进行初始化，主要是对段寄存器的初始化
 * 成功后，新环境存储在 *newenv_store
 * 成功时返回0，失败时返回<0
 * 
 * 内核要开始的第一个用户环境不是通过中断等方法来进入到内核的，而是由内核直接载入的
 * 对环境的 Env 进行初始化，是为了模仿int x指令的作用，模拟第一个环境是通过中断进入了内核，在内核处理完了相应的操作之后，才返回用户态的
 */
int env_alloc(struct Env **newenv_store, envid_t parent_id)
{
	int32_t generation;
	int r;
	struct Env *e;

	if (!(e = env_free_list))
		return -E_NO_FREE_ENV;

	// 为新的环境分配并设置4级页表(映射BIOS与内核代码数据段).
	if ((r = env_setup_vm(e)) < 0)
		return r;

	// 为新环境生成env_id.
	generation = (e->env_id + (1 << ENVGENSHIFT)) & ~(NENV - 1);
	// 生成的env_id必须为正数
	if (generation <= 0)
		generation = 1 << ENVGENSHIFT;
	e->env_id = generation | (e - envs);

	// 设置基础的状态变量：父id、环境类型、环境状态(就绪态)、运行次数
	e->env_parent_id = parent_id;
	e->env_type = ENV_TYPE_USER;
	e->env_status = ENV_RUNNABLE;
	e->env_runs = 0;

	// 清除所有已保存的寄存器状态，防止当前环境的寄存器值泄漏到新环境中(所有环境所使用寄存器相同)
	memset(&e->env_tf, 0, sizeof(e->env_tf));

	// 为当前准备运行的环境env的env_tf(段寄存器)设置适当的初始值
	// GD_UD 是 GDT中的用户数据段选择子，GD_UT 是用户代码段选择子
	// 每个段寄存器的低两位包含当前访问者权限级别(RPL); 0:内核，3:用户
	// 当切换特权级别时，硬件会进行各种检查，包括RPL和存储在段描述符本身中的描述符特权级别(DPL)
	// tf_rsp: 初始化为 USTACKTOP，表示当前用户栈为NULL
	// tf_cs: 初始化为用户段选择子，用户可访问
	// tf_rip: 这里 rip 的值就是在 load_icode() 里设置的用户程序入口地址
	e->env_tf.tf_ds = GD_UD | DPL_USER;
	e->env_tf.tf_es = GD_UD | DPL_USER;
	e->env_tf.tf_ss = GD_UD | DPL_USER;
	e->env_tf.tf_rsp = USTACKTOP;
	e->env_tf.tf_cs = GD_UT | DPL_USER;
	// 在 load_icode() 中设置 e->env_tf.tf_rip 指向程序 ELF 文件的 entry point.

	// 置位FL_IF，自动开启外部中断响应.
	e->env_tf.tf_eflags |= FL_IF;
	// 页错误处理函数地址
	e->env_pgfault_upcall = 0;

	// 清除IPC接收标志.
	e->env_ipc_recving = 0;

	// 存储分配的环境
	env_free_list = e->env_link;
	*newenv_store = e;

	cprintf("[%08x] new env %08x\n", curenv ? curenv->env_id : 0, e->env_id);
	return 0;
}

/**
 * 为用户环境 e 分配和映射物理内存，用于存储环境运行所需资源(调用page_insert())
 * 参数：
 * e:Env指针, va:虚拟地址, len:分配和映射的空间大小
 * 作用：在 e 指向的用户虚拟地址空间中分配[va, va+len)一段区域，为后续写入数据作准备
 * - 不需要对分配的空间初始化
 * - 分配的页用户和内核具有写权限
 * - 需要对起始地址 va 和长度 len 进行页对齐
 * - 只在 load_icode() 调用
 * 
 * 实现：
 * 类似pmap.c中的boot_map_region()，却不一样
 * boot_map_region() 操作的是内核的虚拟地址空间，boot_pml4e提供的是静态映射(内核共享)，且不涉及物理页的分配
 * 而region_alloc() 则是要为实际的物理页帧分配(page_insert())映射到当前用户的虚拟地址空间(page_alloc())中(页表页项)
 */
static void
region_alloc(struct Env *e, void *va, size_t len)
{
	// 开始区间和结束区间页对齐
	size_t start = ROUNDDOWN((uint64_t)va, PGSIZE);
	size_t end = ROUNDUP((uint64_t)(va + len), PGSIZE);
	for (; start < end; start += PGSIZE)
	{
		// 分配一个物理页帧
		struct PageInfo *pp = page_alloc(0);
		if (!pp)
			panic("No memory available for allocation to environment.");
		if (page_insert(e->env_pml4e, pp, (void *)start, PTE_U | PTE_P | PTE_W) < 0)
			panic("Cannot allocate any memory.");
	}
}

/**
 * 先前调用 env_setup_vm() 新建4级页表，并从内核的4级页表映射到页表页项
 * 函数功能：解析程序的 ELF 映像，加载其内容到新环境的用户地址空间中
 * - 像bootloader，从ELF文件加载用户环境的初始代码区、栈和处理器标识位
 * - 这个函数仅在内核初始化期间、第一个用户态环境运行前被调用
 * - 将 ELF 镜像中所有可加载的段载入到用户地址空间中，并设置 e->env_tf.tf_rip 为 ELF 的 entry point，以便环境执行程序指令
 * - 清零 .bss 段
 * - 映射环境的初始栈
 * 
 * 由于还没有实现文件系统，所以用户环境实际的存放的位置实际上是在内存中的，文件载入内存，实际上是内存之间的数据的复制而已
 * 所以这个函数的作用就是将嵌入在内核中的用户环境复制到user.ld指定的用户虚拟地址空间(0x800020)
 * 参数binary指针，指向用户程序在内核中的开始位置
 */
void load_icode(struct Env *e, uint8_t *binary)
{
	/**
	 * 注意:
	 * 1.对于用户程序ELF文件的每个程序头ph中的ph->p_memsz和ph->p_filesz，
	 * 前者是该程序头应在内存中占用的空间大小，而后者是实际该程序头占用的文件大小.
	 * 区别就是ELF文件中BSS节中那些没有被初始化的静态变量，这些变量不会被分配文件储存空间，
	 * 但是在实际载入后，需要在内存中给与相应的空间，并且全部初始化为0
	 * 所以具体来讲，就是每个程序段ph，总共占用p_memsz的内存，
	 * 前面p_filesz的空间从binary的对应内存复制过来，后面剩下的空间全部清0
	 * 
	 * 2.ph->p_va 指向该程序段应该被存储到用户环境Env的虚拟空间地址. 
	 * 可是，在进入 load_icode() 时，是内核态进入的，所以虚拟地址空间还是内核的空间
	 * 要如何对用户的虚拟空间进行操作呢？
	 *  lcr3 (e->env_cr3);
	 * 这个语句在进入每个程序头进行具体设置时，将页表切换到用户虚拟地址空间
	 * 方便的通过 memset() 和 memmove() 函数对 va 进行相应的操作了
	 * 其中 e->env_cr3 的值是在前面的 env_setup_vm() 设置好的
	 * ELF 载入完毕以后，就不需要对用户空间进行操作了，所以在最后切回到内核虚拟地址空间来
	 * 
	 * 3.要配置程序的入口地址，对应的操作: e->env_tf.tf_rip = ELFHDR->e_entry;
	 */
	// binary 指向第一个用户环境起始的虚拟地址(内核va之后首个)，因此用(Elf*)指向该虚拟地址为解析 ELF 头部作准备
	struct Elf *env_elf = (struct Elf *)binary;
	// ph: ELF头部的程序头，eph: ELF 头部所有程序之后的结尾处
	struct Proghdr *ph, *eph;
	// 判断是否为有效的 ELF文件
	if (env_elf->e_magic != ELF_MAGIC)
		panic("load_icode: The binary is not a valid ELF!\n");

	// 加载程序段：
	// ph 指向ELF头部的程序头的起始地址(env_elf+env_elf->e_phoff)
	ph = (struct Proghdr *)((uint8_t *)env_elf + env_elf->e_phoff);
	// eph(end of ph): ELF头部所有程序之后的结尾处
	eph = ph + env_elf->e_phnum;
	// 在内核对用户的虚拟空间进行操作
	// A:用户虚拟地址空间的4级页表e->env_cr3
	lcr3(e->env_cr3);

	for (; ph < eph; ph++)
	{
		// 只加载 LOAD 类型的程序段到内存
		if (ph->p_type == ELF_PROG_LOAD)
		{
			// AlvOS 分配用户空间不是连续的，而是根据ph->p_va作为每次的开始地址，以p_memsz为长度进行页分配
			// 首先对当前用户环境在虚拟地址处分配空闲的物理页
			region_alloc(e, (void *)ph->p_va, ph->p_memsz);
			// 将当前ELF程序段 ph 的数据复制到p_va虚拟地址空间，大小p_filesz
			memmove((void *)ph->p_va, (void *)(binary + ph->p_offset), ph->p_filesz);
			// ELF文件中BSS段中那些没有被初始化的静态变量，全部初始化为0，即p_memsz-p_filesz的部分(.bss)
			// 又因为函数 region_alloc 分配的物理页已经标志ALLOC_ZERO，无需再memset清零
			memset((void *)(ph->p_va + ph->p_filesz), 0, ph->p_memsz - ph->p_filesz);
		}
	}
	// 恢复cr3寄存器为内核的4级页表
	lcr3(boot_cr3);

	// 这样才能根据设置好的cs与新的偏移量eip找到用户程序需要执行的代码
	e->env_tf.tf_rip = env_elf->e_entry;
	// 再在虚拟地址(USTACKTOP-PGSIZE)为环境映射一个 PGSIZE 大小的初始栈
	region_alloc(e, (void *)(USTACKTOP - PGSIZE), PGSIZE);

	// 在许多系统上，内核初始化分配一个栈页，然后如果程序发生的故障是去访问这个栈页下面的页，那么内核会自动分配这些页，并让程序继续运行
	// 通过这种方式，内核只分配程序所需要的内存栈，但是程序可以运行在一个任意大小的栈的假像中
	e->elf = binary;
}

/**
 * 为参数binary创建用户环境虚拟地址空间，并且将其载入到相应的虚拟地址上
 * 参数：
 * binary: 用户环境所在地址(va), type: 用户环境类型，一般为 ENV_TYPE_USER
 * 函数功能：
 * 1.使用 env_alloc() 分配一个新的env
 * 2.设置好env_type
 * 3.这个函数只在内核初始化期间，即运行第一个用户态环境之前被调用 新 env->parent_id 设置为0
 * 4.并调用 load_icode() 将分配的新 env 加载到 binary
 */
void env_create(uint8_t *binary, enum EnvType type)
{
	struct Env *e;
	// 1.分配一个新的 env 环境，即创建用户环境的地址空间4级页表
	uint32_t r = env_alloc(&e, 0);
	// 处理分配环境的错误，分别是内存不足、环境分配已满(>1024)
	if (r < 0)
	{
		panic("env_create: %e", r);
	}
	// 2.设置 env_type, env_parent_id
	e->env_type = type;
	// 4.将用户环境运行所需要的代码加载到用户环境的地址空间(参数binary)
	load_icode(e, binary);

	// x86处理器使用了EFLAGS的IOPL位来决定是否允许保护模式代码使用device I/O指令
	// 根据 ENV_TYPE_FS 向文件系统进程给出相关的I/O权限
	if (type == ENV_TYPE_FS)
	{
		e->env_tf.tf_eflags |= FL_IOPL_MASK;
	}
}

/**
 * 释放环境e及其所有内存.
 */
void env_free(struct Env *e)
{
	pte_t *pt;
	uint64_t pdeno, pteno;
	physaddr_t pa;

	// If freeing the current environment, switch to kern_pgdir
	// before freeing the page directory, just in case the page
	// gets reused.
	if (e == curenv)
		lcr3(boot_cr3);

	// Note the environment's demise.
	// cprintf("[%08x] free env %08x\n", curenv ? curenv->env_id : 0, e->env_id);

	// Flush all mapped pages in the user portion of the address space
	pdpe_t *env_pdpe = KADDR(PTE_ADDR(e->env_pml4e[0]));
	int pdeno_limit;
	uint64_t pdpe_index;
	// using 3 instead of NPDPENTRIES as we have only first three indices
	// set for 4GB of address space.
	for (pdpe_index = 0; pdpe_index <= 3; pdpe_index++)
	{
		if (!(env_pdpe[pdpe_index] & PTE_P))
			continue;
		pde_t *env_pgdir = KADDR(PTE_ADDR(env_pdpe[pdpe_index]));
		pdeno_limit = pdpe_index == 3 ? PDX(UTOP) : PDX(0xFFFFFFFF);
		static_assert(UTOP % PTSIZE == 0);
		for (pdeno = 0; pdeno < pdeno_limit; pdeno++)
		{

			// only look at mapped page tables
			if (!(env_pgdir[pdeno] & PTE_P))
				continue;
			// find the pa and va of the page table
			pa = PTE_ADDR(env_pgdir[pdeno]);
			pt = (pte_t *)KADDR(pa);

			// unmap all PTEs in this page table
			for (pteno = 0; pteno < PTX(~0); pteno++)
			{
				if (pt[pteno] & PTE_P)
				{
					page_remove(e->env_pml4e, PGADDR((uint64_t)0, pdpe_index, pdeno, pteno, 0));
				}
			}

			// free the page table itself
			env_pgdir[pdeno] = 0;
			page_decref(pa2page(pa));
		}
		// free the page directory
		pa = PTE_ADDR(env_pdpe[pdpe_index]);
		env_pdpe[pdpe_index] = 0;
		page_decref(pa2page(pa));
	}
	// free the page directory pointer
	page_decref(pa2page(PTE_ADDR(e->env_pml4e[0])));
	// free the page map level 4 (PML4)
	e->env_pml4e[0] = 0;
	pa = e->env_cr3;
	e->env_pml4e = 0;
	e->env_cr3 = 0;
	page_decref(pa2page(pa));

	// return the environment to the free list
	e->env_status = ENV_FREE;
	e->env_link = env_free_list;
	env_free_list = e;
}

//
// Frees environment e.
// If e was the current env, then runs a new environment (and does not return
// to the caller).
//
void env_destroy(struct Env *e)
{
	// If e is currently running on other CPUs, we change its state to
	// ENV_DYING. A zombie environment will be freed the next time
	// it traps to the kernel.
	if (e->env_status == ENV_RUNNING && curenv != e)
	{
		e->env_status = ENV_DYING;
		return;
	}

	env_free(e);

	if (curenv == e)
	{
		curenv = NULL;
		sched_yield();
	}
}

/**
 * 使用 iret 指令恢复在 Trapframe 中寄存器值，将退出内核并开始执行原(新)环境代码，此函数不会返回.
 * PushRegs 结构的寄存器顺序对应于(逆序)指令 POPA
 * POPA 弹出栈的时候栈指针递增，所以将 RSP 设置为 Trapframe 所在内存的首地址，依次恢复寄存器
 *
 * 实现: 
 * 1.将 %rsp 指向 tf 地址处，也就是将栈顶指向 Trapframe 结构开始处，Trapframe 结构开始处正是一个 PushRegs 结构
 *   POPA 将 PushRegs 结构中保存的寄存器值恢复到寄存器中
 * 2.接着按顺序弹出 %es, %ds
 * 3.最后执行 iret 指令(中断返回指令)：
 *   从 Trapframe 结构中依次恢复 tf_rip, tf_cs, tf_rflags, tf_rsp, tf_ss 到相应寄存器
 */
void env_pop_tf(struct Trapframe *tf)
{
	// 为了便于用户空间调试，记录正在运行的CPU
	curenv->env_cpunum = cpunum();

	__asm __volatile(
					 /* 占位符 %0 由"g"(tf)定义，代表参数tf，即Trapframe的指针地址 */
					 /* 指令代表esp指向参数(Trapframe*)tf开始位置 */
					 "movq %0,%%rsp\n"
					 /* 将存储 struct PushRegs 的寄存器数值放回寄存器 */
					 POPA
					 /* 恢复 %es, %ds 段寄存器 */
					 "movw (%%rsp),%%es\n"
					 "movw 8(%%rsp),%%ds\n"
					 "addq $16,%%rsp\n"
					 /* 跳过 tf_trapno, tf_errcode */
					 "\taddq $16,%%rsp\n"
					 /* iret之后发生权限级的改变(即由内核态切换到用户态)，所以iret会依次弹出5个寄存器
					 (rip、cs、rflags、rsp、ss) */
					 "\tiretq"
					 /* 这些寄存器在env_alloc(), load_icode()中都已赋值，iret后，rip就指向了程序的入口地址，
					 cs也由内核代码段转向了用户代码段，rsp也由内核栈转到了用户栈 */
					 :
					 /* g 是通用传递约束(寄存器、内存、立即数)，此处表示使用内存 */
					 : "g"(tf)
					 : "memory");
	// 错误处理
	panic("iret failed");
}

/**
 * 上下文切换：curenv -> Env e.
 * 函数功能：上下文切换/运行初始化完毕的用户环境
 * 所以在加载新的用户环境的 cr3寄存器之前(重定位cs与rip)，必须将设置好的es、ds和rsp入栈，保护原环境(调用函数env_pop_tf(), 完成上下文切换)
 * 加载完毕之后恢复这些寄存器，然后用户环境就在新的代码段开始执行用户的环境了
 * 注意：如果这是第一次调用 env_run()，curenv 为 NULL
 */
void env_run(struct Env *e)
{
	/**
	 * 步骤1: 如果这是一个上下文切换(一个新的环境正在运行):
	 * 	1.如果当前环境是ENV_RUNNING，则将当前环境(如果有)设置回ENV_RUNNABLE(想想它还可以处于什么状态 ENV_NOT_RUNNABLE)，
	 * 	2.将 curenv 设置为新环境，
	 * 	3.将其状态设置为ENV_RUNNING，
	 * 	4.更新其 env_runs 计数器，
	 * 	5.使用lcr3()切换到对应的4级页表地址空间
	 * 
	 * 步骤2:使用env_pop_tf()恢复环境的寄存器，并在环境中返回用户态。
	 * 
	 * 注意，这个函数从e->env_tf加载新环境的状态，确保您已经将e->env_tf的相关部分设置为合理的值
	 */

		// 1.如果当前运行的环境(curenv)是正在运行(ENV_RUNNING)，上下文切换，更新状态为等待运行(ENV_RUNNABLE)
	if (curenv && curenv->env_status == ENV_RUNNING)
	{
		curenv->env_status = ENV_RUNNABLE;
	}
	// 2~4.设置curenv为新环境，并更新状态和运行次数
	curenv = e;
	curenv->env_status = ENV_RUNNING;
	curenv->env_runs++;
	// env_run() 修改了参数e的成员状态之后就会调用lcr3切换到进程的4级页表，这时候内存管理单元MMU所使用的寻址上下文立即发生了变化。在地址切换前后，为什么参数e仍能够被引用？
	// 内核地址空间被映射到4级页表，所有环境4级页表的内核部分是相同的，通过内核地址空间访问e.
	// 5.使用lcr3()切换到e对应的4级页表(地址空间)
	lcr3(curenv->env_cr3);
	// 用户态 -> 内核态，加大内核锁
	unlock_kernel();
	// 调用env_pop_tf切换(恢复)回用户态
	env_pop_tf(&e->env_tf);
}
