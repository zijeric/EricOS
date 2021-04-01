#include "inc/mmu.h"
#include "inc/memlayout.h"
#include "kern/macro.h"

// 调用具体的页错误处理函数，通过在 trap.c push 的 UTrapframe 结构变换到调用缺页处理环境的上下文
// 首先变换到异常栈(调用缺页处理的栈，佳能保存的缺页处理环境的%rip push 进栈)，空出一个字(4-Byte)
// 

// 页错误处理的关键，用户态页错误处理函数的入口
// - 调用具体的页错误处理函数(由 lib/pgfault.c 的代码注册到环境结构的字段)，返回执行引起页错误的代码
// - 处理后直接返回不必经过内核，因为在上文 page_fault_handler() 压入栈的 UTrapframe 保存了出错前环境的状态

// 当页错误发生时，内核将我们的 %rsp 切换到用户异常栈(非嵌套页错误)，然后将一个 UTrapframe push 进栈传递给用户
// 如果这是一个嵌套错误，递归调用时，内核将在用户异常栈 %esp 之上为我们保留 8-Byte(存储 %rip)
// 然后，我们在 c 代码中调用由全局变量 _pgfault_handler 指向的页错误处理函数.
/*
	UTrapframe 在栈(UXSTACKTOP)中结构：
			.				<-- UXSTACKTOP
			.
			.				<-- trap-time(页错误前) %rsp
		(reserved 8-Byte)嵌套页错误
		trap-time rsp
		trap-time eflags
		trap-time rip
		trap-time rax    <-- start of struct PushRegs
		trap-time rbx
		trap-time rcx
		trap-time rdx
		trap-time rbp
		trap-time rdi
		trap-time rsi
		trap-time r8
		trap-time r9
		trap-time r10
		trap-time r11
		trap-time r12
		trap-time r13
		trap-time r14
		trap-time r15    <-- end of struct PushRegs
		utf_err (error code)
		utf_fault_va         <-- %rsp when handler is run
*/

.text
.globl _pgfault_upcall
_pgfault_upcall:

	// 调用C代码中的页错误处理函数.
	// 函数参数: 指向 UTrapframe 的指针

	// 通过 %rdi 传递函数参数
	movq  %rsp, %rdi
	movabs _pgfault_handler, %rax
	call *%rax

	/*
		已经从C代码的页错误处理函数返回(栈平衡)，为了返回执行页错误前的指令，push trap-time %rip 到用户异常栈上
		但是，不能直接从用户异常栈返回(jmp/ret): 必须恢复 trap-time 的环境状态
		因此，push trap-time %rip 到 trap-time 栈上!
		接下来，我们将切换到该栈并调用 ret，它将把 %rip 恢复到页错误前的值
		在用户异常栈上出现嵌套页错误的时 ...

		UTrapframe 在栈(UXSTACKTOP)中结构：
				.				<-- UXSTACKTOP
				.
				.				<-- trap-time(页错误前) %rsp
			(reserved 8-Byte)嵌套页错误
			trap-time rsp
			trap-time eflags
			trap-time rip
			trap-time rax    <-- start of struct PushRegs
			trap-time rbx
			trap-time rcx
			trap-time rdx
			trap-time rbp
			trap-time rdi
			trap-time rsi
			trap-time r8
			trap-time r9
			trap-time r10
			trap-time r11
			trap-time r12
			trap-time r13
			trap-time r14
			trap-time r15    <-- end of struct PushRegs
			utf_err (error code)
			utf_fault_va         <-- %rsp when handler is run

			%rsp 指向 utf_fault_va,
			fault_va: 8-Byte,
			utf_err: 8-Byte,
			regs: 15*8 = 120-Byte,
			eflags: 8-Byte,
			rip: 8-Byte
	*/

	addq $16, %rsp	/* 跳过用户异常栈中的 utf_fault_va 和 utf_err(不需要) */
	
	/* 用户异常栈: %rsp 指向 end of struct PushRegs */
	
	movq 120(%rsp), %r10	/* %r10 = %rip */
	movq 136(%rsp), %r11	/* %r11 = %rsp */

	subq $8, %r11			/* %r11(trap-time %rsp) - 8 */
	movq %r10, (%r11)		/* %r11(trap-time %rsp) 指向的值为 %rip */
	movq %r11, 136(%rsp)	/* 更新 trap-time %rsp 的值 */
	// 若是嵌套页错误将会覆盖原来的 trap-time rsp
	// 若是非嵌套，则 %r11 覆盖上一个指令的 %rip，并不会出错

	/*	此时的用户异常栈(嵌套)：
		
		-------------+		<-- trap-time %rsp (former)
		trap-time rip 
		-------------+		<-- %r11(trap-time %rsp - 8)
		trap-time rsp-8
		trap-time eflags
		trap-time rip

					(非嵌套)
		-------------+		<-- trap-time %rsp
		trap-time rsp-8
		trap-time eflags
		trap-time rip
	*/

	// 恢复 trap-time 寄存器集.  接下来不能再修改任何通用寄存器集 PushRegs.
	POPA_	/* 之前已经通过增加 16-Byte 跳过了 fault_va 和 utf_err，所以只需使用 POPA_ 恢复 */

	// 从栈中恢复 %eflags.  接下来不能再使用修改 %eflags 的指令，比如算术指令sub, cmp等.
	addq $8, %rsp	/* 跳过 %rip，指向 %eflags */
	popfq			/* 从栈中恢复 %eflags */

	/*	此时的用户异常栈(嵌套)：
		
		-------------+		<-- trap-time %rsp (former)
		trap-time rip 
		-------------+		<-- trap-time %rsp - 8
		trap-time rsp-8		<-- %rsp
		(blank 8-Byte)
		trap-time rip

					(非嵌套)
		-------------+		<-- trap-time %rsp
		trap-time rsp-8		<-- %rsp(trap-time %rsp-8)
		(blank 8-Byte)
		trap-time rip
	*/

	// 切换回调整后的 trap-time 栈.
	popq %rsp		/* 已经指向 %rsp，直接 pop 恢复 */

	// 返回重新执行引起页错误的环境指令.
	ret				/* popq %rip */
