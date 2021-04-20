#include "inc/stdio.h"
#include "inc/string.h"
#include "inc/assert.h"
#include "inc/memlayout.h"

#include "kern/monitor.h"
#include "kern/console.h"
#include "kern/kdebug.h"
#include "kern/dwarf_api.h"

#include "kern/pmap.h"
#include "kern/kclock.h"
#include "kern/env.h"
#include "kern/trap.h"
#include "kern/sched.h"
#include "kern/picirq.h"
#include "kern/cpu.h"
#include "kern/spinlock.h"

/**
 * 从 kern/entry.S 进入到内核初始化代码(进入内核后所有引用地址都是虚拟地址，而链接地址=虚拟地址)
 */

uint64_t end_debug;

static void boot_aps(void);

void i386_init(void)
{
	// 在 kern/kernel.ld 初始化
	// GCC特性：这两个变量都是在链接（生成ELF文件时）产生的地址，GCC会在生成二进制文件的时候将这两个符号置换成地址
	// edata: .bss节在内存中开始的位置，end: 内核可执行程序在内核中结束的位置，.bss是文件在内存中的个最后一部分
	extern char edata[], end[];

	// 在执行任何其他操作之前，必须先完成 ELF 加载过程.
	// 为了确保所有静态/全局变量初始值为 0，清除程序中未初始化的全局数据(BSS)部分.
	memset(edata, 0, end - edata);

	// 初始化控制台(包括显存的初始化、键盘的初始化). 在此之后才能调用 cprintf()!
	cons_init();

	// end 在 linker(kernel.ld) 中初始化：内核 ELF 格式文件后首字节的地址
	// 因此 end 是 linker 没有 分配任何内核代码或全局变量的第一个虚拟地址/线性地址(.bss 段是已加载内核代码的最后一个段)
	extern char end[];

	// KELFHDR=(0x10000+KERNBASE): 内存中内核的ELF文件地址，因为开启了分页，需要加上内核映射基址
	// end_debug 就是 (内核 + 内核 DWARF 调试段信息) 后的首地址
	end_debug = read_section_headers(KELFHDR, (uintptr_t)end);

	/**
	 * BSP 调用mem_init()，主要创建页目录、pages数组、envs数组、映射pages数组/envs数组/BSP内核栈等到4级页表中
	 * mem_init()	// 初始化内存管理
	 * - i386_detect_memory()	// 通过 BIOS e820/调用硬件检测可以使用的内存大小
	 * - boot_alloc()	// 页表映射尚未构建时的内存分配器 -> boot_pml4e, pages[](大小由探测决定), envs[](NENV)
	 * - page_init()	// 初始化 pages[] 中的物理页(避免映射已使用的物理内存)，建立 page_free_list 链表从而使用page分配器 page_alloc(), page_free()
	 *   - boot_alloc(0)	// 获取内核后第一个空闲的物理地址空间(页对齐)
	 * - boot_map_region()	// 更新参数页目录pgdir的页表页项pte，将线性地址[va, va+size]映射到物理地址[pa, pa+size](PPN + perm)
	 *   - pml4e_walk()		// 创建4级页表(页式翻译): 根据参数线性地址 la 执行页式地址转换机制，返回页表页项pte的虚拟地址(物理页的基址)，只映射(创建)页目录项，无关物理页
	 *     - page_alloc()	// 通过 page_free_list 分配页(即从空闲链表取出结点)
	 * 
	 * 以下设置虚拟内存，主要映射了四个区域：
	 * 1.第一个是 [UPAGES, UPAGES + size of pages[])映射到页表存储的物理地址 [pages, pages + size of pages[]) 0xff000=1020KB
	 *   这里的PTSIZE代表页式内存管理所占用的空间(不包括4级页表)
	 * 
	 * 2.第二个是 [UENVS, UENVS + size of envs[])映射到envs数组存储的物理地址 [envs, envs + size of envs[]) 0x48000=120KB
	 *   这里的PTSIZE代表页式内存管理所占用的空间(不包括4级页表)
	 * 
	 * 3.第三个是 [KSTACKTOP-KSTKSIZE, KSTACKTOP) 映射到[bootstack, bootstack+64KB) 64KB
	 *   KERNBASE以下的NCPUS(8)个物理页大小用作内核栈，栈向下拓展，栈指针 RSP
	 * 
	 * 4.第四个则是映射整个内核的虚拟空间[KERNBASE, KERNBASE + (npages * PGSIZE)]到 物理地址 [0, 0xff00000)  255MB
	 *   涵盖了所有物理内存
	 */
	x64_vm_init();

	/**
	 * BSP 调用 env_init()，初始化env_free_list；同时调用 env_init_percpu() 加载当前cpu的 GDT 和 gs/fs/es/ds/ss 段描述符
	 * env_init()			// 初始化用户环境(envs[NENV], env_free_list逆序地包含所有的env)
	 * - env_init_percpu()	// 加载可区分用户态、能处理int指令(TSS)的GDT，并初始化es, ds, ss(在用户态和内核态切换使用), cs(内核代码段)等寄存器
	 */
	env_init();

	/**
	 * BSP 调用 trap_init()，初始化 IDT；同时调用 trap_init_percpu() 函数初始化 BSP 的 TSS 和 IDT
	 * trap_init()			// 设置x64的所有中断向量，使 IDT指向对应中断处理函数地址，并加载 TSS 和 IDT
	 * - SETGATE(gate, istrap, sel, off, dpl)	// 以函数指针的形式将中断处理程序写进中断描述符表
	 * - trap_init_percpu()	// 初始化并加载 Per-CPU 的 TSS 和 IDT
	 */
	trap_init();

	/**
	 * lapic_init() + mp_init() -> x86 多CPU初始化
	 * mp_init() 函数通过调用 mpconfig() 从BIOS中读取浮动指针mp，从mp中找到struct mpconf多处理器配置表，
	 * 然后根据这个结构体内的entries信息（processor table entry）对各个cpu结构体进行配置（主要是cpuid）
	 * 如果proc->flag是MPPROC_BOOT，说明这个入口对应的处理器是用于启动的处理器，把结构体数组cpus[ncpu]地址赋值给bootcpu指针
	 * 注意这里ncpu是个全局变量，那么这里实质上就是把cpus数组的第一个元素的地址给了bootcpu
	 * 如果出现任何entries匹配错误，则认为处理器的初始化失败了，不能用多核处理器进行机器的运行
	 */
	mp_init();

	/**
	 * BSP 调用lapic_init() 映射lapic物理地址到页目录(映射的虚拟地址递增)，设置时钟，允许 APIC 接收中断
	 * lapic_init()			// 
	 * - mmio_map_region()	// 
	 * - lapicw()			// 
	 * 
	 */
	lapic_init();

	// 多任务初始化函数，初始化8259A中断控制器，允许生成中断
	pic_init();

	/**
	 * BSP 调用 lock_kernel()唤醒 APs 之前，获取大内核锁
	 * lock_kernel()			// 将 大内核锁(单个的全局锁) 作为参数调用
	 * - spin_lock()			// 获取锁(xchg 原子性操作)；循环(自旋)，直到获得锁
	 */
	lock_kernel();

	/**
	 * BSP 调用boot_aps() 驱动 APs 引导
	 * boot_aps()			// 复制 CPU 启动代码(mpentry.S)到 0x7000，对 Per-CPU 分别确定内核栈地址
	 * - lapic_startap()	// 命令对应 CPU 从加载代码处开始执行给对应 AP 的 LAPIC 发送 STARTUP IPI 以及一个初始CS:IP地址
	 * AP 将在该地址上(MPENTRY_PADDR=0x7000)执行入口代码
	 * BSP 仍在 boot_aps() 等待 AP 发送 CPU_STARTED 信号(CpuInfo 的 cpu_status)，然后激活下一个 CPU
	 */
	boot_aps();

	/**
	 * kern/Makefrag 中的-b binary 选项，把对应文件链接为不解析的二进制文件
	 * obj/kern/kernel.sym 中链接器生成了一些符号(eg:_binary_obj_user_hello_start)
	 * 这种符号为普通内核代码使用一种引入嵌入式二进制文件的方法
	 * 
	 * 这个宏相当于调用env_create(_binary_obj_user_..._start, ENV_TYPE_USER)
	 * 从而指定了在之后的 env_run 中要执行的环境，user/...的 umain 环境
	 * 
	 * 创建用户环境的过程
	 * ENV_CREATE()
	 * - env_create()		// 创建新的 env 并设置好内存映射，加载程序段，初始化运行时栈
	 *   - env_alloc()		// 创建一个新的 env 并初始化结构中各个字段(eg:env_tf: cs,ds,es,ss,esp regs)
	 *     - env_setup_vm()	// 为新的环境分配并设置页目录(映射内核的BIOS与内核代码数据段)，
	 *   - load_icode()		// 根据程序文件头部加载数据段、代码段等
	 *     - region_alloc()	// 为用户环境映射一页内存作为栈空间（USTACKTOP - PGSIZE）
	 */
	// Start fs.
	ENV_CREATE(fs_fs, ENV_TYPE_FS);

#if defined(TEST)
	// Don't touch -- used by grading script!
	ENV_CREATE(TEST, ENV_TYPE_USER);
#else
	// Touch all you want.
	ENV_CREATE(user_icode, ENV_TYPE_USER);
#endif // TEST*

	// Should not be necessary - drains keyboard because interrupt has given up.
	kbd_intr();

	// Schedule and run the first user environment!
	// 在函数env_run调用env_pop_tf之后，处理器开始执行trapentry.S下的代码
	// 应该首先跳转到TRAPENTRY_NOEC(divide_handler, T_DIVIDE)处，再经过_alltraps，进入trap函数

	// 进入trap函数后，先判断是否由用户态进入内核态，若是，则必须保存环境状态
	// 也就是将刚刚得到的TrapFrame存到对应环境结构体的属性中，之后要恢复环境运行，就是从这个TrapFrame进行恢复
	// 若中断令处理器从内核态切换到内核态，则不做特殊处理(嵌套interrupt，无需切换栈)

	// 接着调用分配函数trap_dispatch()，这个函数根据中断向量，调用相应的处理函数，并返回
	// 故函数trap_dispatch返回之后，对中断的处理应当是已经完成了，该切换回触发中断的环境
	// 修改函数trap_dispatch的代码时应注意，函数后部分(内核/环境存在bug)不应该执行，否则会销毁当前环境curenv
	// 中断处理函数返回后，trap_dispatch应及时返回

	// 切换回到旧环境，调用的是env_run，根据当前环境结构体curenv中包含和运行有关的信息，恢复环境执行

	// BSP上自行创建环境，并调用sched_yield()函数尝试开始执行可执行环境
	// 只有当BSP激活所有APs并且开始调用sched_yield()运行用户程序时，其他CPU才可能开始执行用户环境
	// 调度并运行第一个用户环境
	sched_yield();
}

/**
 * 当boot_aps()引导 CPU 时，函数通过这个变量将应该由 mpentry.S 加载的各个内核栈指针传递到对应的 CPU
 * 每次 AP 启动都会更新值，因为每个 AP 对应不同的地址
 * 
 * 如果共享的内核栈，中断时，硬件会先自动将相关寄存器进栈，然后才执行锁的检查，共享内核栈可能会导致系统崩溃
 * 支持多个 CPU 的时候，只有一份内核4级页表，所有CPU都会使用这个4级页表映射CPU栈
 * 不同的 CPU 栈映射到不同的虚拟地址上
 * 注意：不同的用户环境是可以同时将用户栈物理地址映射到 UXSTACKTOP 上的
 * 因为每一个用户环境都有一份独立的4级页表，创建用户环境的时候会分配和映射一页用户栈物理页到UXSTACKTOP上
 * 多个 CPU 同时运行多个用户环境的时候，实际上都是使用各自的4级页表进行寻址和存储数据到各自的用户栈物理页上的
 */
void *mpentry_kstack;

/**
 * 驱动AP(Application Processor)启动程序的运行.
 * APs 在实模式下启动，与 boot/boot.S 的引导过程相似：boot_aps()将AP入口代码(kern/mpentry.S)拷贝到实模式下的 MPENTRY_PADDR
 * 与boot/boot.S的引导过程不同的是，AlvOS可以控制 AP 将会在哪里开始执行代码
 * 
 * 然后，boot_aps()通过向对应AP的LAPIC单元发送STARTUP处理器间中断(INI)和初始的CS:IP地址（本实验中为MPENTRY_PADDR），依次激活APs
 * AP将在 MPENTRY_PADDR 执行入口代码
 * kern/mpentry.S 在简单的设置后，将AP启动分页，使得AP进入保护模式，然后调用启动例程mp_main()
 * boot_aps()在唤醒下一个AP之前，会先等待当前AP在struct CpuInfo的cpu_status域中发送一个CPU_STARTED标记
 */
static void
boot_aps(void)
{
	// mpentry_start, mpentry_end 在 mpentry.S 定义，为了计算 mpentry.S 的字节大小
	extern unsigned char mpentry_start[], mpentry_end[];
	// 用于存储 MPENTRY_PADDR 虚拟地址
	void *code;
	struct CpuInfo *c;

	// 在 MPENTRY_PADDR 中将入口代码(kern/mpentry.S)复制到未使用的内存中
	code = KADDR(MPENTRY_PADDR);
	memmove(code, mpentry_start, mpentry_end - mpentry_start);
	// 逐一引导APs
	for (c = cpus; c < cpus + ncpu; c++)
	{
		// 遍历的 CPU 是当前运行的 CPU.
		if (c == cpus + cpunum())
			continue;

		// 传递参数给 mpentry.S: 当前 CPU 使用对应的栈地址
		mpentry_kstack = percpu_kstacks[c - cpus] + KSTKSIZE;
		// 在 mpentry_start 启动 CPU
		lapic_startap(c->cpu_id, PADDR(code));
		// 等待 CPU 在 mp_main() 中完成基本设置
		while (c->cpu_status != CPU_STARTED)
			;
	}
}

/**
 * 由 mpentry.S 调用
 * AP 从加载代码处启动，加载gdt、手动设置初始页表、开启保护模式、分页等，然后跳转到 mp_main();
 * mp_main() 直接切换到4级页表，然后调用 lapic_init() 映射 lapic 物理地址到4级页表(映射的虚拟地址递增)并允许APIC接收中断
 * 调用 env_init_percpu() 加载当前 CPU 的 GDT 和 gs/fs/es/ds/ss 段描述符;
 * 调用 trap_init_percpu() 初始化当前 CPU 的 TSS 和 IDT，然后使用自旋锁设置启动完成标识
 * 并调用 sched_yield() 尝试开始执行可执行环境
 */
void mp_main(void)
{
	// 现在 EIP 处于高地址，可以安全地切换到 kern_pgdir
	lcr3(boot_cr3);
	cprintf("SMP: CPU %d starting\n", cpunum());

	// 映射 lapic 物理地址到4级页表(映射的虚拟地址递增)并允许APIC接收中断
	lapic_init();
	// 加载当前 CPU 的 GDT 和 gs/fs/es/ds/ss 段描述符
	env_init_percpu();

	// 始化当前 CPU 的 TSS 和 IDT，然后使用自旋锁设置启动完成标识
	trap_init_percpu();
	// 传递参数到 boot_aps(): 当前 CPU 已经启动
	xchg(&thiscpu->cpu_status, CPU_STARTED);

	// 获取大内核锁，保证一次只能有一个CPU进入调度器！
	lock_kernel();
	// 初始化 AP 之后，调用 sched_yield() 在这个CPU上开始运行环境之前
	sched_yield();
	// 在 sched_yield() 中实现循环调度时会调用 env_run() 函数，进而释放内核锁
}

/*
 * 变量 panicstr 包含第一次调用 panic 的参数; 用作标志表示内核已经调用 panic
 */
const char *panicstr;

/*
 * 遇到无法解决的致命错误时调用panic.
 * 打印"panic: mesg"，然后进入内核监视器.
 */
void _panic(const char *file, int line, const char *fmt, ...)
{
	va_list ap;

	if (panicstr)
		goto dead;
	panicstr = fmt;

	// 特别确保机器处于合理的状态
	__asm __volatile("cli; cld");

	va_start(ap, fmt);
	cprintf("kernel panic on CPU %d at %s:%d: ", cpunum(), file, line);
	vcprintf(fmt, ap);
	cprintf("\n");
	va_end(ap);

dead:
	/* 直接陷入内核监视器 */
	while (1)
		monitor(NULL);
}