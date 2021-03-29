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
	// 为了调用trap_init_percpu时设置gdt对TSS的索引
	extern struct Segdesc gdt[];
	// 中断类型，interrupt 返回之前阻止新的中断产生，trap 则允许新中断
	// TODO......
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
	// The example code here sets up the Task State Segment (TSS) and
	// the TSS descriptor for CPU 0. But it is incorrect if we are
	// running on other CPUs because each CPU has its own kernel stack.
	// Fix the code so that it works for all CPUs.
	//
	// Hints:
	//   - The macro "thiscpu" always refers to the current CPU's
	//     struct CpuInfo;
	//   - The ID of the current CPU is given by cpunum() or
	//     thiscpu->cpu_id;
	//   - Use "thiscpu->cpu_ts" as the TSS for the current CPU,
	//     rather than the global "ts" variable;
	//   - Use gdt[(GD_TSS0 >> 3) + 2*i] for CPU i's TSS descriptor;
	//   - You mapped the per-CPU kernel stacks in mem_init_mp()
	//
	// ltr sets a 'busy' flag in the TSS selector, so if you
	// accidentally load the same TSS on more than one CPU, you'll
	// get a triple fault.  If you set up an individual CPU's TSS
	// wrong, you may not get a fault until you try to return from
	// user space on that CPU.
	//

	// 为了能陷入内核态加载内核栈，必须初始化并加载TSS.
	size_t cur_cpu_id = cpunum();

	thiscpu->cpu_ts.ts_esp0 = KSTACKTOP - cur_cpu_id * (KSTKSIZE + KSTKGAP);
	// 1.初始化 GDT 中的 TSS(kern/env.c 中配置为 NULL)
	SETTSS((struct SystemSegdesc64 *)(&gdt[(GD_TSS0 >> 3) + 2 * cur_cpu_id]), STS_T64A, (uint64_t)(&thiscpu->cpu_ts), sizeof(struct Taskstate), 0);

	// 2.加载 IDT 和 TSS，IDT 依赖于 TSS，所以先加载TSS
	// 3.加载 TSS 选择子(与其他段选择子相似，将[低三位特殊位]设置为0)
	ltr(GD_TSS0 + ((2 * cur_cpu_id << 3) & (~0x7)));

	// 4.加载 trap_init() 设置好的 IDT
	lidt(&idt_pd);
}

void print_trapframe(struct Trapframe *tf)
{
	cprintf("TRAP frame at %p from CPU %d\n", tf, cpunum());
	print_regs(&tf->tf_regs);
	cprintf("  es   0x----%04x\n", tf->tf_es);
	cprintf("  ds   0x----%04x\n", tf->tf_ds);
	cprintf("  trap 0x%08x %s\n", tf->tf_trapno, trapname(tf->tf_trapno));
	// If this trap was a page fault that just happened
	// (so %cr2 is meaningful), print the faulting linear address.
	if (tf == last_tf && tf->tf_trapno == T_PGFLT)
		cprintf("  cr2  0x%08x\n", rcr2());
	cprintf("  err  0x%08x", tf->tf_err);
	// For page faults, print decoded fault error code:
	// U/K=fault occurred in user/kernel mode
	// W/R=a write/read caused the fault
	// PR=a protection violation caused the fault (NP=page not present).
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
	if ((tf->tf_cs & 3) != 0)
	{
		cprintf("  rsp  0x%08x\n", tf->tf_rsp);
		cprintf("  ss   0x----%04x\n", tf->tf_ss);
	}
}

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

static void
trap_dispatch(struct Trapframe *tf)
{
	// 处理环境产生的异常(分发异常到对应的处理函数).
	switch (tf->tf_trapno)
	{
	// 处理页错误中断
	case T_PGFLT:
		// 内核 -> panic，用户环境 -> env_destroy
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
		// 为了方便用指令"iret Trapframe"返回用户态在trap point重新启动(即返回中断点下一个指令继续)
		// 复制Trapframe(目前位于栈上)到当前运行环境的结构体成员变量 curenv->env_tf
		curenv->env_tf = *tf;
		// Trapframe已经被暂存到结构curenv，从这里开始，可以忽略栈上的Trapframe(esp还指向它)
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

void page_fault_handler(struct Trapframe *tf)
{
	uint64_t fault_va;

	// Read processor's CR2 register to find the faulting address
	fault_va = rcr2();

	// Handle kernel-mode page faults.

	// LAB 3: Your code here.
	if (!(tf->tf_cs & 0x3))
	{
		print_trapframe(tf);
		panic("unhandled trap in kernel");
	}

	// We've already handled kernel-mode exceptions, so if we get here,
	// the page fault happened in user mode.

	// Call the environment's page fault upcall, if one exists.  Set up a
	// page fault stack frame on the user exception stack (below
	// UXSTACKTOP), then branch to curenv->env_pgfault_upcall.
	//
	// The page fault upcall might cause another page fault, in which case
	// we branch to the page fault upcall recursively, pushing another
	// page fault stack frame on top of the user exception stack.
	//
	// The trap handler needs one word of scratch space at the top of the
	// trap-time stack in order to return.  In the non-recursive case, we
	// don't have to worry about this because the top of the regular user
	// stack is free.  In the recursive case, this means we have to leave
	// an extra word between the current top of the exception stack and
	// the new stack frame because the exception stack _is_ the trap-time
	// stack.
	//
	//
	// If there's no page fault upcall, the environment didn't allocate a
	// page for its exception stack or can't write to it, or the exception
	// stack overflows, then destroy the environment that caused the fault.
	// Note that the grade script assumes you will first check for the page
	// fault upcall and print the "user fault va" message below if there is
	// none.  The remaining three checks can be combined into a single test.
	//
	// Hints:
	//   user_mem_assert() and env_run() are useful here.
	//   To change what the user environment runs, modify 'curenv->env_tf'
	//   (the 'tf' variable points at 'curenv->env_tf').

	if (curenv->env_pgfault_upcall)
	{
		struct UTrapframe *utexp;
		if (tf->tf_rsp <= UXSTACKTOP - 1 && tf->tf_rsp >= UXSTACKTOP - PGSIZE)
		{
			utexp = (struct UTrapframe *)(tf->tf_rsp - sizeof(struct UTrapframe) - 8);
		}
		else
		{
			utexp = (struct UTrapframe *)(UXSTACKTOP - sizeof(struct UTrapframe));
		}
		//storing that 64 bit thingy.(this was tough!, I'm weak with bits ;) )
		//(time frame) to be stored...but how does it get pushed into the stack...you assign it to uxstacktop
		//thats brilliant. Thank you! thank you...wait a minute...see if it overflows!
		user_mem_assert(curenv, (void *)utexp, sizeof(struct UTrapframe), PTE_W | PTE_U);
		utexp->utf_fault_va = fault_va;
		utexp->utf_err = tf->tf_err;
		utexp->utf_regs = tf->tf_regs;
		utexp->utf_rip = tf->tf_rip;
		utexp->utf_eflags = tf->tf_eflags;
		utexp->utf_rsp = tf->tf_rsp;
		//How do i run the upcall...set the rip...thats nice...thank you exercise 10 :)
		tf->tf_rip = (uint64_t)curenv->env_pgfault_upcall;
		tf->tf_rsp = (uint64_t)utexp;
		env_run(curenv);
	}
	// LAB 4: Your code here.

	// Destroy the environment that caused the fault.
	cprintf("[%08x] user fault va %08x ip %08x\n", curenv->env_id, fault_va, tf->tf_rip);
	print_trapframe(tf);
	env_destroy(curenv);
}
