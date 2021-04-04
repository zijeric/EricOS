// 用户级页错误处理函数.
// 不是将C语言的页错误处理函数直接注册到内核，而是在 pfentry.S 中注册汇编语言封装函数.
// 由 pfentry.S 调用已注册的C函数

#include "inc/lib.h"

// 定义在  lib/pfentry.S 的汇编语言页错误入口.
extern void _pgfault_upcall(void);

// 指向当前已注册的C语言页错误处理函数.
void (*_pgfault_handler)(struct UTrapframe *utf);

/**
 * 设置用户级页错误处理函数.
 * 包含用户异常栈的初始化和页错误处理函数的设置，利用 kern/syscall.c sys_env_set_pgfault_upcall() 注册(utf字段)用户页错误处理函数
 * 第一次注册处理程序时，我们需要分配一个异常堆栈(顶部位于UXSTACKTOP的一页内存)
 * 并告诉内核在页错误发生时调用汇编语言 _pgfault_upcall 例程
 */
void set_pgfault_handler(void (*handler)(struct UTrapframe *utf))
{
	if (_pgfault_handler == 0)
	{
		// 第一次出错，创建用户异常栈
		if (sys_page_alloc(0, (void *)(UXSTACKTOP - PGSIZE), PTE_W | PTE_U | PTE_P) == 0)
			sys_env_set_pgfault_upcall(0, _pgfault_upcall);
		else
			panic("set_pgfault_handler(): no memory");
	}

	// 存储汇编语言调用的处理函数指针.
	_pgfault_handler = handler;
}
