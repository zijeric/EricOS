// test user-level fault handler -- alloc pages to fix faults

#include "inc/lib.h"

// 用户级别的页错误处理流程如下：
// 1、用户程序通过lib/pgfault.c的set_pgfault_handler()函数注册页错误处理函数入口_pgfault_upcall（见lib/pfentry.S），并指定页错误处理函数_pgfault_handler，该函数指针将在_pgfault_upcall中被使用。第一次注册_pgfault_upcall时将申请分配并映射异常栈。
// 2、用户程序触发页错误，切换到内核模式。
// 3、内核检查页错误处理程序是否已设置、异常栈指针是否越界、是否递归触发页错误、是否已为异常栈分配物理页，然后在异常栈上存储UTrapframe，将栈指针指向异常栈，并跳转执行当前进程的env_pgfault_upcall（被设为用户级页错误处理程序入口_pgfault_upcall）。
// 4、用户模式下_pgfault_upcall调用_pgfault_upcall，执行用户级别页错误处理函数，在用户态的page_fault_handler结束后恢复现场并跳回原程序执行。
void
handler(struct UTrapframe *utf)
{
	int r;
	void *addr = (void*)utf->utf_fault_va;

	cprintf("fault %x\n", addr);
	if ((r = sys_page_alloc(0, ROUNDDOWN(addr, PGSIZE),
				PTE_P|PTE_U|PTE_W)) < 0)
		panic("allocating at %x in page fault handler: %e", addr, r);
	snprintf((char*) addr, 100, "this string was faulted in at %x", addr);
}

void
umain(int argc, char **argv)
{
	// snprintf的定义是int snprintf(char *buf, int n, const char *fmt, ...)，向buf缓存区中按照格式填充字符串。
	// 出错的原因是：当执行cprintf("%s\n", (char*)0xCafeBffe);时，触发了页错误。页错误处理函数在虚拟地址ROUNDDOWN(addr, PGSIZE)即cafeb000上分配和映射物理页，此时可访问的虚拟地址空间为[0xcafeb000,0xcafec000-1]。之后，调用snprintf向0xCafeBffe内存地址上填充字符串，由于字符串过长，导致越界访问0xcafec000，再次触发页错误。第二次页错误处理程序往0xcafec000填充字符串，返回到第一次页错误处理程序，第一次页错误处理程序往0xCafeBffe填充字符串，返回到用户程序，执行cprintf进行输出
	set_pgfault_handler(handler);
	cprintf("%s\n", (char*)0xDeadBeef);
	cprintf("%s\n", (char*)0xCafeBffe);
}
