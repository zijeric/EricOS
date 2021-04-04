// 系统调用.

#include "inc/syscall.h"
#include "inc/lib.h"

/**
 * 用户的指令式系统调用，该函数将系统调用序号放入eax寄存器，五个参数依次放入edx, ecx, ebx, edi, esi，然后执行指令int 0x30，
 * 发生中断后，去IDT中查找中断处理函数，最终会走到kern/trap.c的trap_dispatch()中
 */
static inline int64_t
syscall(int num, int check, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
	int64_t ret;

	/**
	 * 通过'int 0x30'指令进行系统调用，可传递最多5个参数，处理完成后将返回值存储在eax
	 *
	 * int指令系统调用 - 硬件的操作步骤
	 * - 从IDT中获得第n个描述符，n就是int的参数
	 * - 检查%cs的域CPL<=DPL，DPL是描述符的特权级
	 * - 如果目标段选择子的DPL<CPL，就在CPU内部的寄存器保存%rsp和%ss的值
	 * - 从TSS中加载%ss和%rsp(切换用户环境栈为TSS存储的内核栈)
	 * - 将%ss, %rsp, %rflags, %cs, %rip依次压栈(保护原环境)
	 * - 清除%rflags的一些位，设置%cs和%rip为描述符中的值(用户态和内核态隔离)，进入内核态
	 * 
	 * 发生中断后，去IDT中查找中断处理函数，最终会走到kern/trap.c的trap_dispatch()中
	 * 根据中断号0x30，又会调用kern/syscall.c中的syscall()函数（注意这时候我们已经进入了内核态CPL=0）
	 * 在该函数中根据系统调用号调用对应的系统调用处理函数
	 * 
	 * C语言内联汇编
	 * asm volatile ("asm code" : output : input : changed);
	 * 通用系统调用: 在eax中传递系统调用序号，在rdx, rcx, rbx, rdi, rsi中最多传递五个参数
	 * 用T_SYSCALL中断内核
	 * volatile 告诉汇编程序不要因为我们不使用返回值就优化该指令
	 * 最后一个子句告诉汇编程序，指令可能会改变条件代码cx和任意内存memory位置
	 * 由汇编编译器做好数据保存和恢复工作(栈)
	 */

	asm volatile("int %1\n"			// code: int T_SYSCALL
				 : "=a"(ret)		// ret = eax
				 : "i"(T_SYSCALL),	// T_SYSCALL作为整数型立即数
				   "a"(num),		// eax = num 系统调用序号
				   "d"(a1),			// edx = a1
				   "c"(a2),			// ecx = a2
				   "b"(a3),			// ebx = a3
				   "D"(a4),			// edi = a4
				   "S"(a5)			// esi = a5
				 : "cc", "memory"); // 指令可能会改变条件代码cx和任意内存memory位置
									// 由汇编编译器做好数据保存和恢复工作(栈)

	if (check && ret > 0)
		panic("syscall %d returned %d (> 0)", num, ret);

	return ret;
}

void sys_cputs(const char *s, size_t len)
{
	// 调用lib/syscall.c中的syscall()
	// SYS_cputs: 系统调用序号(eax)，s: 需要输出的字符数组，len: 数组长度
	syscall(SYS_cputs, 0, (uint64_t)s, len, 0, 0, 0);
}

int sys_cgetc(void)
{
	return syscall(SYS_cgetc, 0, 0, 0, 0, 0, 0);
}

int sys_env_destroy(envid_t envid)
{
	// 销毁envid对应的环境
	return syscall(SYS_env_destroy, 1, envid, 0, 0, 0, 0);
}

envid_t
sys_getenvid(void)
{
	// 获取当前用户环境id
	return syscall(SYS_getenvid, 0, 0, 0, 0, 0, 0);
}

void sys_yield(void)
{
	syscall(SYS_yield, 0, 0, 0, 0, 0, 0);
}

int sys_page_alloc(envid_t envid, void *va, int perm)
{
	return syscall(SYS_page_alloc, 1, envid, (uint64_t)va, perm, 0, 0);
}

int sys_page_map(envid_t srcenv, void *srcva, envid_t dstenv, void *dstva, int perm)
{
	return syscall(SYS_page_map, 1, srcenv, (uint64_t)srcva, dstenv, (uint64_t)dstva, perm);
}

int sys_page_unmap(envid_t envid, void *va)
{
	return syscall(SYS_page_unmap, 1, envid, (uint64_t)va, 0, 0, 0);
}

// sys_exofork is inlined in lib.h

int sys_env_set_status(envid_t envid, int status)
{
	return syscall(SYS_env_set_status, 1, envid, status, 0, 0, 0);
}

int sys_env_set_trapframe(envid_t envid, struct Trapframe *tf)
{
	return syscall(SYS_env_set_trapframe, 1, envid, (uint64_t)tf, 0, 0, 0);
}

int sys_env_set_pgfault_upcall(envid_t envid, void *upcall)
{
	return syscall(SYS_env_set_pgfault_upcall, 1, envid, (uint64_t)upcall, 0, 0, 0);
}

int sys_ipc_try_send(envid_t envid, uint64_t value, void *srcva, int perm)
{
	return syscall(SYS_ipc_try_send, 0, envid, value, (uint64_t)srcva, perm, 0);
}

int sys_ipc_recv(void *dstva)
{
	return syscall(SYS_ipc_recv, 1, (uint64_t)dstva, 0, 0, 0, 0);
}
