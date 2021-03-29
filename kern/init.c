#include "inc/stdio.h"
#include "inc/string.h"
#include "inc/assert.h"
#include "inc/memlayout.h"

#include "kern/monitor.h"
#include "AlvOS/console.h"
#include "kern/kdebug.h"
#include "AlvOS/dwarf_api.h"

#include "kern/pmap.h"
#include "kern/kclock.h"
#include "AlvOS/env.h"
#include "kern/trap.h"
#include "kern/sched.h"
#include "kern/picirq.h"
#include "AlvOS/cpu.h"
#include "kern/spinlock.h"

/**
 * 从 kern/entry.S 进入到内核初始化代码(进入内核后所有引用地址都是虚拟地址，而链接地址=虚拟地址)
 * 
 */

uint64_t end_debug;

static void boot_aps(void);

void i386_init(void)
{
	/* __asm __volatile("int $12"); */
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

	/*
	   从 E820 获取的内存映射可知，可使用的物理内存上限是 256MB.
	   e820 MEMORY MAP
		size: 20, physical address: 0x0000000000000000, length: 0x000000000009fc00, type: reserved
		size: 20, physical address: 0x000000000009fc00, length: 0x0000000000000400, type: availiable
		size: 20, physical address: 0x00000000000f0000, length: 0x0000000000010000, type: availiable
		size: 20, physical address: 0x0000000000100000, length: 0x000000000fedf000, type: reserved
		size: 20, physical address: 0x000000000ffdf000, length: 0x0000000000021000, type: availiable
		size: 20, physical address: 0x00000000fffc0000, length: 0x0000000000040000, type: availiable
		total: 4GB(0x100000000)
		基址为0x100000的物理内存空间(拓展内存)，会是我们主要用来执行内核代码的空间
	 */
	/**
	 * BSP调用mem_init()函数，主要完成创建页目录、pages数组、envs数组、映射pages数组/envs数组/BSP内核栈等到页目录中
	 * mem_init()	// 初始化内存管理
	 * - i386_detect_memory()	// 通过汇编指令直接调用硬件查看可以使用的内存大小
	 * - boot_alloc()	// 页表映射尚未构建时的内存分配器 -> kern_pgdir, pages[](大小由探测决定), envs[](NENV)
	 * - page_init()	// 初始化 pages[] 中的每一个物理页(通过页表映射整个物理内存空间进行管理)，建立 page_free_list 链表从而使用page分配器 page_alloc(), page_free()
	 *   - boot_alloc(0)	// 获取内核后第一个空闲的物理地址空间(页对齐)
	 * - boot_map_region()	// 更新参数页目录pgdir的页表页项pte，将线性地址[va, va+size]映射到物理地址[pa, pa+size](PPN + perm)
	 *   - pgdir_walk()		// 创建二级页表(页目录映射): 根据参数线性地址va执行页式地址转换机制，返回页表页项pte的虚拟地址(物理页的基址)，只映射(创建)页目录项，无关页表页与物理页
	 *     - page_alloc()	// 通过 page_free_list 分配页(即从空闲链表取出结点)
	 * 
	 */ 
	x64_vm_init();

	env_init();
	trap_init();

	mp_init();
	lapic_init();

	pic_init();

	// Acquire the big kernel lock before waking up APs
	lock_kernel();
	// Starting non-boot CPUs
	boot_aps();

	//ENV_CREATE(user_primes, ENV_TYPE_USER);
	ENV_CREATE(user_sendpage, ENV_TYPE_USER);
	//ENV_CREATE(user_buggyhello, ENV_TYPE_USER);
	// ENV_CREATE(user_forktree, ENV_TYPE_USER);
	//ENV_CREATE(user_yield, ENV_TYPE_USER);
	//ENV_CREATE(user_yield, ENV_TYPE_USER);
	//ENV_CREATE(user_yield, ENV_TYPE_USER);
	//ENV_CREATE(user_yield, ENV_TYPE_USER);
	//ENV_CREATE(user_yield, ENV_TYPE_USER);

	// 调度并运行第一个用户环境
	sched_yield();
}

/**
 * 当boot_aps()引导 CPU 时，函数通过这个变量将应该由 mpentry.S 加载的各个内核栈指针传递到对应的 CPU
 * 每次 AP 启动都会更新值，因为每个 AP 对应不同的地址
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
	// 存储 MPENTRY_PADDR 虚拟地址
	void *code;
	struct CpuInfo *c;

	// 在 MPENTRY_PADDR 中将入口代码(kern/mpentry.S)复制到未使用的内存中
	// 其实在任何低于 640KB 的、未使用的、页对齐的物理地址上都是可以运行的，因此选择了在0x7000
	code = KADDR(MPENTRY_PADDR);
	memmove(code, mpentry_start, mpentry_end - mpentry_start);
	// 逐一引导APs
	for (c = cpus; c < cpus + ncpu; c++)
	{
		if (c == cpus + cpunum()) // 当前运行 CPU 已经开始了.
			continue;

		/// 传递参数给 mpentry.S :当前 CPU 使用对应的栈
		mpentry_kstack = percpu_kstacks[c - cpus] + KSTKSIZE;
		// 在 mpentry_start 启动 CPU
		lapic_startap(c->cpu_id, PADDR(code));
		// 等待 CPU 在 mp_main() 中完成基本设置
		while (c->cpu_status != CPU_STARTED)
			;
	}
}

/**
 * AP（cpu）从加载代码处启动，加载gdt表、手动设置初始页表、开启保护模式、分页等，然后跳转到mp_main()函数；
 * mp_main()函数直接切换到页目录，然后调用lapic_init()函数映射lapic物理地址到页目录（映射的虚拟地址递增），
 * 并允许APIC接收中断；调用env_init_percpu()函数加载当前cpu的GDT和gs/fs/es/ds/ss段描述符；
 * 调用trap_init_percpu()函数初始化当前cpu的TSS和IDT，然后使用自旋锁设置启动完成标识，
 * 并调用sched_yield()函数尝试开始执行可执行进程
 */
void mp_main(void)
{
	// 现在 EIP 处于高地址，可以安全地切换到 kern_pgdir
	lcr3(boot_cr3);
	cprintf("SMP: CPU %d starting\n", cpunum());

	lapic_init();
	env_init_percpu();
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
	cprintf("kernel panic at %s:%d: ", file, line);
	vcprintf(fmt, ap);
	cprintf("\n");
	va_end(ap);

dead:
	/* 直接陷入内核监视器 */
	while (1)
		monitor(NULL);
}
