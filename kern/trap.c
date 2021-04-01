#include "inc/mmu.h"
#include "inc/x86.h"
#include "inc/assert.h"

#include "kern/pmap.h"
#include "kern/trap.h"
#include "AlvOS/console.h"
#include "kern/monitor.h"
#include "AlvOS/env.h"
#include "kern/syscall.h"
#include "kern/sched.h"
#include "kern/kclock.h"
#include "kern/picirq.h"
#include "AlvOS/cpu.h"
#include "kern/spinlock.h"

extern uintptr_t gdtdesc_64;
static struct Taskstate ts;
extern struct Segdesc gdt[];
extern long gdt_pd;

/**
 * 为了调试，print_trapframe 可以区分打印已保存的 trapframe 和打印当前 trapframe
 * 并在当前trapframe中打印一些附加信息
 */
static struct Trapframe *last_tf;

/**
 * Interrupt descriptor table.
 * 中断描述符表必须在运行时构建(不能在boot阶段)，因为移位的函数地址不能在重定位记录中表示
 */
struct Gatedesc idt[256] = {{0}};
struct Pseudodesc idt_pd = {0, 0};

static const char *trapname(int trapno)
{
	static const char *const excnames[] = {
		"Divide error",					// 0.除法错误
		"Debug",						// 1.调试异常
		"Non-Maskable Interrupt",		// 0.不可屏蔽中断
		"Breakpoint",					// 3.断点(一个字节的INT3指令)
		"Overflow",						// 4.溢出(INTO指令)
		"BOUND Range Exceeded",			// 5.边界检验(BOUND指令)
		"Invalid Opcode",				// 6.非法操作符
		"Device Not Available",			// 7.设备不可用
		"Double Fault",					// 8.双重错误
		"Coprocessor Segment Overrun",	// 9.协处理器段溢出
		"Invalid TSS",					// 10.无效的TSS
		"Segment Not Present",			// 11.段不存在
		"Stack Fault",					// 10.栈异常
		"General Protection",			// 13.通用保护
		"Page Fault",					// 14.页错误
		"(unknown trap)",				// 15.保留
		"x87 FPU Floating-Point Error", // 16.x87FPU 浮点错误
		"Alignment Check",				// 17.界限检查
		"Machine-Check",				// 18.机器检查
		"SIMD Floating-Point Exception" // 19.SIMD 浮点错误
										// [20,31]: 保留，[32,255]: 用户可定义的中断
	};

	if (trapno < sizeof(excnames) / sizeof(excnames[0]))
		return excnames[trapno];
	if (trapno == T_SYSCALL)
		return "System call"; // 48.系统调用
	if (trapno >= IRQ_OFFSET && trapno < IRQ_OFFSET + 16)
		return "Hardware Interrupt";
	return "(unknown trap)";
}

/**
 * 在 BSP 设置x86的所有中断向量(中断向量、中断类型、中断处理函数、DPL)，并加载 TSS和IDT
 * 
 * 完整的trap控制流(传入中断向量, eg: T_SYSCALL):
 * 
 *        IDT             trapentry.S              trap.c
 * +----------------+ 
 * |   &handler1    |--> handler1:        trap (struct Trapframe *tf)
 * |                |       // push regs   {
 * |                |       call trap        // handle the exception/interrupt
 * |                |       // ...         }
 * +----------------+
 * |   &handler2    |--> handler2:
 * |                |      // push regs
 * |                |      call trap
 * |                |      // ...
 * +----------------+
 *        .
 *        .
 * +----------------+
 * |   &handlerX    |--> handlerX:
 * |                |       // push regs
 * |                |       call trap
 * |                |       // ...
 * +----------------+
 */
void trap_init(void)
{
	extern struct Segdesc gdt[];
	// 中断类型，interrupt 返回之前阻止新的中断产生，trap 则允许新中断
	uint8_t interrupt = 0;
	uint8_t trap = 1;
	// interrupt gate 的 DPL
	uint32_t kern_dpl = 0;
	uint32_t user_dpl = 3;
	// 为了在IDT中记录处理函数入口地址，从而找到 kern/trapentry.S 的对应异常的宏处理函数(_NOEC)
	// 压栈 %es,%ds 后跳转到 kern/trap.c 的 trap()
	// 声明中断异常处理函数名
	void ALV_DIVIDE();
	void ALV_DEBUG();
	void ALV_NMI();
	void ALV_BRKPT();
	void ALV_OFLOW();
	void ALV_BOUND();
	void ALV_ILLOP();
	void ALV_DEVICE();
	void ALV_DBLFLT();
	// reserved
	void ALV_TSS();
	void ALV_SEGNP();
	void ALV_STACK();
	void ALV_GPFLT();
	void ALV_PGFLT();
	// reserved
	void ALV_FPERR();
	void ALV_ALIGN();
	void ALV_MCHK();
	void ALV_SIMDERR();

	void ALV_SYSCALL();

	void ALV_IRQ0();
	void ALV_IRQ1();
	void ALV_IRQ2();
	void ALV_IRQ3();
	void ALV_IRQ4();
	void ALV_IRQ5();
	void ALV_IRQ6();
	void ALV_IRQ7();
	void ALV_IRQ8();
	void ALV_IRQ9();
	void ALV_IRQ10();
	void ALV_IRQ11();
	void ALV_IRQ12();
	void ALV_IRQ13();
	void ALV_IRQ14();
	void ALV_IRQ15();

	/**
	 * 将刚刚写好的一系列入口，以函数指针的形式，写进中断描述符表
	 * 
	 * SETGATE(gate, istrap, sel, off, dpl) 构造gate描述符存储到idt[]
	 * gate: 设置idt[T_...]istrap: 0->interrupt,1->trap/exc. 区别：interrupt 返回之前阻止新的中断产生
	 * sel: 中断都在内核中完成，段选择子为内核代码段GD_KT，off: 在内核代码段中的段内偏移为对应的函数指针，dpl: 特权级
	 * 
	 * 中断(istrap=0, dpl=3)的意义在于，允许这些中断通过int指令触发
	 * 		根据x86手册7.4，对于部分通过int指令触发的中断，只有在大于指定的权限状态下触发中断，才能进入中断入口函数
	 * 		如在用户态下触发权限为0的T_DEBUG中断，就不能进入入口，反而会触发GPFLT中断。
	 */
	SETGATE(idt[T_DIVIDE], trap, GD_KT, ALV_DIVIDE, kern_dpl);
	SETGATE(idt[T_DEBUG], trap, GD_KT, ALV_DEBUG, kern_dpl);
	SETGATE(idt[T_NMI], trap, GD_KT, ALV_NMI, kern_dpl);

	// 为了让用户也可以使用断点(breakpoint)int 3，所以 dpl 为 user_dpl
	SETGATE(idt[T_BRKPT], interrupt, GD_KT, ALV_BRKPT, user_dpl);
	SETGATE(idt[T_OFLOW], trap, GD_KT, ALV_OFLOW, kern_dpl);
	SETGATE(idt[T_BOUND], trap, GD_KT, ALV_BOUND, kern_dpl);
	SETGATE(idt[T_ILLOP], interrupt, GD_KT, ALV_ILLOP, kern_dpl);
	SETGATE(idt[T_DEVICE], trap, GD_KT, ALV_DEVICE, kern_dpl);
	SETGATE(idt[T_DBLFLT], trap, GD_KT, ALV_DBLFLT, kern_dpl);
	SETGATE(idt[T_TSS], trap, GD_KT, ALV_TSS, kern_dpl);
	SETGATE(idt[T_SEGNP], trap, GD_KT, ALV_SEGNP, kern_dpl);
	SETGATE(idt[T_STACK], trap, GD_KT, ALV_STACK, kern_dpl);
	SETGATE(idt[T_GPFLT], trap, GD_KT, ALV_GPFLT, kern_dpl);

	SETGATE(idt[T_PGFLT], interrupt, GD_KT, ALV_PGFLT, kern_dpl);
	SETGATE(idt[T_FPERR], trap, GD_KT, ALV_FPERR, kern_dpl);
	SETGATE(idt[T_ALIGN], trap, GD_KT, ALV_ALIGN, kern_dpl);
	SETGATE(idt[T_MCHK], trap, GD_KT, ALV_MCHK, kern_dpl);
	SETGATE(idt[T_SIMDERR], trap, GD_KT, ALV_SIMDERR, kern_dpl);
	
	SETGATE(idt[T_SYSCALL], interrupt, GD_KT, ALV_SYSCALL, user_dpl);

	SETGATE(idt[32], interrupt, GD_KT, ALV_IRQ0, kern_dpl);
	SETGATE(idt[33], interrupt, GD_KT, ALV_IRQ1, kern_dpl);
	SETGATE(idt[34], interrupt, GD_KT, ALV_IRQ2, kern_dpl);
	SETGATE(idt[35], interrupt, GD_KT, ALV_IRQ3, kern_dpl);
	SETGATE(idt[36], interrupt, GD_KT, ALV_IRQ4, kern_dpl);
	SETGATE(idt[37], interrupt, GD_KT, ALV_IRQ5, kern_dpl);
	SETGATE(idt[38], interrupt, GD_KT, ALV_IRQ6, kern_dpl);
	SETGATE(idt[39], interrupt, GD_KT, ALV_IRQ7, kern_dpl);
	SETGATE(idt[40], interrupt, GD_KT, ALV_IRQ8, kern_dpl);
	SETGATE(idt[41], interrupt, GD_KT, ALV_IRQ9, kern_dpl);
	SETGATE(idt[42], interrupt, GD_KT, ALV_IRQ10, kern_dpl);
	SETGATE(idt[43], interrupt, GD_KT, ALV_IRQ11, kern_dpl);
	SETGATE(idt[44], interrupt, GD_KT, ALV_IRQ12, kern_dpl);
	SETGATE(idt[45], interrupt, GD_KT, ALV_IRQ13, kern_dpl);
	SETGATE(idt[46], interrupt, GD_KT, ALV_IRQ14, kern_dpl);
	SETGATE(idt[47], interrupt, GD_KT, ALV_IRQ15, kern_dpl);

	// 将配置好的 IDT 存储到 idt_pd
	idt_pd.pd_lim = sizeof(idt) - 1;
	idt_pd.pd_base = (uint64_t)idt;

	// 为 Per-CPU 中断初始化，加载 TSS 和 IDT
	trap_init_percpu();
}

// 初始化并加载 Per-CPU 的 TSS 和 IDT
void trap_init_percpu(void)
{
	// 为 Per-CPU 设置了 Task State Segment(TSS), TSS 描述符(各自的内核栈)

	// 为了能陷入内核态加载内核栈，必须初始化并加载TSS.
	size_t cur_cpu_id = cpunum();

	thiscpu->cpu_ts.ts_esp0 = KSTACKTOP - cur_cpu_id * (KSTKSIZE + KSTKGAP);
	// 1.初始化 GDT 中的 TSS(kern/env.c 中配置为 NULL)，Per-CPU 的内核栈地址
	// 用户环境中断被触发之后，硬件会自动切换到 Per-CPU 对应的内核栈
	SETTSS((struct SystemSegdesc64 *)(&gdt[(GD_TSS0 >> 3) + 2 * cur_cpu_id]),
		   STS_T64A,
		   (uint64_t)(&thiscpu->cpu_ts),
		   sizeof(struct Taskstate),
		   0);

	// 2.加载 IDT 和 TSS，IDT 依赖于 TSS，所以先加载TSS
	// 3.加载 TSS 选择子(与其他段选择子相似，将[低三位特殊位]设置为0)
	ltr(GD_TSS0 + ((2 * cur_cpu_id << 3) & (~0x7)));
	// 中断发生时，通过该寄存器找到TSS结构，配置对应的栈指针 RSP，切换内核栈

	// 4.加载 trap_init() 设置好的 IDT
	lidt(&idt_pd);
}

/**
 * 输出刚刚发生的 trap 的详细信息
 */
void print_trapframe(struct Trapframe *tf)
{
	// CPU
	cprintf("TRAP frame at %p from CPU %d\n", tf, cpunum());
	// 通用寄存器的值
	print_regs(&tf->tf_regs);
	// 段寄存器
	cprintf("  es   0x----%04x\n", tf->tf_es);
	cprintf("  ds   0x----%04x\n", tf->tf_ds);
	// 错误码
	cprintf("  trap 0x%08x %s\n", tf->tf_trapno, trapname(tf->tf_trapno));
	// 如果 trap 是刚刚发生的页错误 ，则输出错误线性地址.
	if (tf == last_tf && tf->tf_trapno == T_PGFLT)
		cprintf("  cr2  0x%08x\n", rcr2());
	cprintf("  err  0x%08x", tf->tf_err);
	// 对于页错误，打印解码错误代码:
	// U/K = 错误发生在用户态/内核态
	// W/R = 读/写导致了故障
	// PR = 违反保护导致故障 (NP = 页不存在).
	if (tf->tf_trapno == T_PGFLT)
		cprintf(" [%s, %s, %s]\n",
				tf->tf_err & 4 ? "user" : "kernel",
				tf->tf_err & 2 ? "write" : "read",
				tf->tf_err & 1 ? "protection" : "not-present");
	else
		cprintf("\n");
	cprintf("  rip  0x%08x\n", tf->tf_rip);
	cprintf("  cs   0x----%04x\n", tf->tf_cs);
	cprintf("  flag 0x%08x\n", tf->tf_eflags);
	// 如果是用户态切换到内核态(&3)，输出 rsp, ss
	if ((tf->tf_cs & 3) != 0)
	{
		cprintf("  rsp  0x%08x\n", tf->tf_rsp);
		cprintf("  ss   0x----%04x\n", tf->tf_ss);
	}
}

/**
 * 顺序输出 PushRegs 对应的所有寄存器的值
 */
void print_regs(struct PushRegs *regs)
{
	cprintf("  r15  0x%08x\n", regs->reg_r15);
	cprintf("  r14  0x%08x\n", regs->reg_r14);
	cprintf("  r13  0x%08x\n", regs->reg_r13);
	cprintf("  r12  0x%08x\n", regs->reg_r12);
	cprintf("  r11  0x%08x\n", regs->reg_r11);
	cprintf("  r10  0x%08x\n", regs->reg_r10);
	cprintf("  r9  0x%08x\n", regs->reg_r9);
	cprintf("  r8  0x%08x\n", regs->reg_r8);
	cprintf("  rdi  0x%08x\n", regs->reg_rdi);
	cprintf("  rsi  0x%08x\n", regs->reg_rsi);
	cprintf("  rbp  0x%08x\n", regs->reg_rbp);
	cprintf("  rbx  0x%08x\n", regs->reg_rbx);
	cprintf("  rdx  0x%08x\n", regs->reg_rdx);
	cprintf("  rcx  0x%08x\n", regs->reg_rcx);
	cprintf("  rax  0x%08x\n", regs->reg_rax);
}

/**
 * 处理环境产生的异常(分发异常到对应的处理函数).
 */
static void
trap_dispatch(struct Trapframe *tf)
{
	switch (tf->tf_trapno)
	{
	// 处理页错误中断
	case T_PGFLT:
		// 内核 -> panic；用户环境 -> 写时复制(copy on write)为用户环境分配
		page_fault_handler(tf);
		break;

	// 处理虚假中断.
	// 硬件有时会因为IRQ线路上的噪声或其他原因而引起这些问题(不关心，不处理).
	case IRQ_OFFSET + IRQ_SPURIOUS:
		cprintf("Spurious interrupt on irq 7\n");
		print_trapframe(tf);
		break;

	// 处理时钟中断.
	case IRQ_OFFSET + IRQ_TIMER:
		// 必须调用 lapic_eoi() 确认中断，才能 sched_yield() 调度环境
		lapic_eoi();
		sched_yield();
		break;

	// 处理断点
	case T_BRKPT:
		// 切换到内核的监控器
		monitor(tf);
		break;

	// 处理系统调用
	case T_SYSCALL:
		// 通过当前运行环境结构体中的环境帧取参数，调用syscall
		// 传递参数时，rax存储了系统调用序号，另外5个寄存器存储参数
		// 用rax寄存器接收系统调用的返回值
		tf->tf_regs.reg_rax = syscall(tf->tf_regs.reg_rax,
									  tf->tf_regs.reg_rdx,
									  tf->tf_regs.reg_rcx,
									  tf->tf_regs.reg_rbx,
									  tf->tf_regs.reg_rdi,
									  tf->tf_regs.reg_rsi);
		break;

	default:
		// 未处理的trap: 用户环境或内核存在bug, 输出.
		print_trapframe(tf);
		// Trapframe存储的cs寄存器若指向内核代码段，内核bug -> panic
		if (tf->tf_cs == GD_KT)
		{
			panic("unhandled trap in kernel");
		}
		// 用户环境bug -> env_destroy 直接销毁
		else
		{
			env_destroy(curenv);
			return;
		}
	}
}

/**
 * 由 kern/trapentry.S调用，并且最后pushl %rsp(Trapframe基址)相当于传递了指向Trapframe的指针tf
 * esp指向栈最后一个push进入的参数，最后的指令为pushal，因此rsp: tf->tf_regs.reg_rdi
 * 
 * 可以看到，若中断是在用户态下触发，就要对tf指针指向的结构体，也就是刚刚压栈的那个结构体，进行拷贝，从而控制用户态下的不安全因素
 * 拷贝完结构体之后，调用函数trap_dispatch，将中断分发到指定的handler处理
 */
void trap(struct Trapframe *tf)
{
	// 环境可能已经设置了逆序的DF(direction flag, 方向标志位)，并且GCC的一些版本依赖于明确的DF
	// 需要设置为正序
	asm volatile("cld" ::
					 : "cc");

	// 如果其他 CPU 调用了 panic() ，则停止 CPU
	extern char *panicstr;
	if (panicstr)
		asm volatile("hlt");

	// 如果在sched_yield()中停止，则重新获取大内核锁
	if (xchg(&thiscpu->cpu_status, CPU_STARTED) == CPU_HALTED)
		lock_kernel();

	// 确保中断被禁用.
	assert(!(read_eflags() & FL_IF));

	if ((tf->tf_cs & 3) == 3)
	{
		// 确保从用户态陷入.
		// 注意，在做任何重要的内核工作之前，获取大内核锁
		lock_kernel();
		assert(curenv);
		// 如果当前环境是ENV_DYING 状态，则进行垃圾收集
		if (curenv->env_status == ENV_DYING)
		{
			env_free(curenv);
			curenv = NULL;
			sched_yield();
		}
		// 为了方便用指令 iret Trapframe 返回用户态在返回中断点下一个指令继续执行
		// 复制 Trapframe(目前位于栈上)到当前运行环境的结构体成员变量 curenv->env_tf
		curenv->env_tf = *tf;
		// Trapframe已经被暂存到结构curenv，可以忽略栈上的Trapframe(RSP还指向它)
		tf = &curenv->env_tf;
	}

	// 记录 tf 是最后一个切换环境的 Trapframe，逐层返回
	// 这样 trap_dispatch() 函数中的 print_trapframe 就可以打印一些额外的信息.
	last_tf = tf;

	// 根据发生的中断类型，调用对应的中断处理程序
	trap_dispatch(tf);

	if (curenv && curenv->env_status == ENV_RUNNING)
		// 恢复原环境
		env_run(curenv);
	else
		// 否则进行环境调度
		sched_yield();
}

/**
 * Page faults and memory protection
 * 操作系统依赖处理器的来实现内存保护。当程序试图访问无效地址或没有访问权限时，处理器在当前指令停住，引发中断进入内核。如果内核能够修复，则在刚才的指令处继续执行，否则程序将无法接着运行。系统调用也为内存保护带来了问题。大部分系统调用接口让用户程序传递一个指针参数给内核。这些指针指向的是用户缓冲区。通过这种方式，系统调用在执行时就可以解引用这些指针。但是这里有两个问题：
 * 1.在内核中的page fault要比在用户程序中的page fault更严重。如果内核在操作自己的数据结构时出现 page faults，这是一个内核的bug，而且异常处理程序会中断整个内核。但是当内核在解引用由用户程序传递来的指针时，它需要一种方法去记录此时出现的任何page faults都是由用户程序带来的。
 * 2.内核通常比用户程序有着更高的内存访问权限。用户程序很有可能要传递一个指针给系统调用，这个指针指向的内存区域是内核可以进行读写的，但是用户程序不能。此时内核必须小心的去解析这个指针，否则的话内核的重要信息很有可能被泄露。
 */
/**
 * page_fault_handler() 处理页错误，分为内核态页错误和用户环境页错误
 * 1.内核态页错误直接 panic()
 * 2.如果触发页错误的是用户环境，则
 *  (1)检查页错误处理程序是否已设置、页错误处理程序地址是否合法、异常栈指针是否越界、是否嵌套触发页错误、是否已为异常栈分配物理页
 * 	(2)用 Utrapframe 结构保存用户态页错误环境寄存器集
 *  (3)修改%rsp，切换到用户异常栈
 *  (4)压 UTrapframe 进异常栈
 *  (5)将%eip设置为 curenv->env_pgfault_upcall，并跳转回用户级页错误处理函数入口执行(用户态)
 */
void page_fault_handler(struct Trapframe *tf)
{
	uint64_t fault_va;

	// 环境引用一个不存在物理页的内存时，触发 CPU 产生页错误异常中断
	// 并把导致页错误的线性地址保存到 CPU 中的 CR2 控制寄存器
	// 为了找到页错误地址，读取 CR2 寄存器
	fault_va = rcr2();

	// 处理内核态的页错误.
	if (!(tf->tf_cs & 0x3))
	{
		// 输出异常信息然后 panic
		print_trapframe(tf);
		panic("page_fault_handler: unhandled trap in kernel");
	}

	// 页错误发生在用户态中.

	// 1.检测是否为页错误(已设置了页错误处理函数入口)
	if (curenv->env_pgfault_upcall)
	{
		// 获取异常栈中 UTrapframe 的结构
		struct UTrapframe *exp_utf;
		// 2.检测 %rsp∈[UXSTACKTOP-PGSIZE, UXSTACKTOP-1]，当前栈是否*已经*在异常栈区间
		if (tf->tf_rsp <= UXSTACKTOP - 1 && tf->tf_rsp >= UXSTACKTOP - PGSIZE)
		{
			// 嵌套页错误: 用户错误栈->内核栈->用户错误栈
			// -8: 预留 8-Byte(64-bit) 存储 %rip
			exp_utf = (struct UTrapframe *)(tf->tf_rsp - sizeof(struct UTrapframe) - 8);
		}
		else
		{
			// 非嵌套: 用户运行栈->内核栈(中断被内核捕捉)->用户错误栈(错误处理)
			// 将 exp_utf 设置为用户异常栈顶部
			exp_utf = (struct UTrapframe *)(UXSTACKTOP - sizeof(struct UTrapframe));
		}
		// 检查 curenv 的异常栈[exp_utf, exp_utf + size of UTrapframe]是否溢出，并对其具有写权限
		user_mem_assert(curenv,
						(void *)exp_utf,
						sizeof(struct UTrapframe),
						PTE_W);

		// 设置用户异常栈帧(UTrapframe)
		exp_utf->utf_fault_va = fault_va;
		exp_utf->utf_err = tf->tf_err;
		exp_utf->utf_regs = tf->tf_regs;
		exp_utf->utf_rip = tf->tf_rip;
		exp_utf->utf_eflags = tf->tf_eflags;
		exp_utf->utf_rsp = tf->tf_rsp;

		// 中断处理函数和出错的用户环境是同一环境，切换栈并改变函数入口
		// 为了执行页错误处理函数入口，并为用户态环境设置寄存器集
		// 设置当前栈帧中的 %rip 指向页错误处理函数入口
		tf->tf_rip = (uint64_t)curenv->env_pgfault_upcall;
		// 设置当前栈帧中的 %rsp 指向用户异常栈帧(UTrapframe)
		tf->tf_rsp = (uint64_t)exp_utf;
		// 切换运行环境(跳转回用户态执行错误页处理函数)
		env_run(curenv);
	}

	// 销毁导致错误的环境.
	cprintf("envid: [%08x] user fault va: %08x rip: %08x\n",
			curenv->env_id, fault_va, tf->tf_rip);
	print_trapframe(tf);
	env_destroy(curenv);
}
