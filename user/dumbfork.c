// 在两个进程之间乒乓操作一个计数器
// 只需要开始其中的一个——粗略地分成两个.

#include "inc/string.h"
#include "inc/lib.h"

int32_t dumbfork(void);

void umain(int argc, char **argv)
{
	int32_t who;
	int i;

	// fork一个子进程
	who = dumbfork();

	// print a message and yield to the other a few times
	for (i = 0; i < (who ? 10 : 20); i++)
	{
		cprintf("%d: I am the %s!\n", i, who ? "parent" : "child");
		sys_yield();
	}
}

void duppage(int32_t dstenv, void *addr)
{
	int r;

	// 这不是fork应该做的事，只是为了测试.
	if ((r = sys_page_alloc(dstenv, addr, PTE_P | PTE_U | PTE_W)) < 0)
		panic("sys_page_alloc: %e", r);
	if ((r = sys_page_map(dstenv, addr, 0, UTEMP, PTE_P | PTE_U | PTE_W)) < 0)
		panic("sys_page_map: %e", r);
	memmove(UTEMP, addr, PGSIZE);
	if ((r = sys_page_unmap(0, UTEMP)) < 0)
		panic("sys_page_unmap: %e", r);
}

int32_t
dumbfork(void)
{
	int32_t envid;
	uint8_t *addr;
	int r;
	extern unsigned char end[];

	// 分配一个新的子环境.
	// 内核会用寄存器集的一个副本来初始化它，这样子进程就会看起来也调用了sys_exofork()
	// 除了在子进程中，这个对sys_exofork()的“假”调用会返回0而不是孩子的envid.
	envid = sys_exofork();
	if (envid < 0)
		panic("S_alloc_proc: %e", envid);
	if (envid == 0)
	{
		// We're the child.
		// The copied value of the global variable 'thisproc'
		// is no longer valid (it refers to the parent!).
		// Fix it and return 0.
		thisproc = &procs[ENVX(sys_getprocid())];
		return 0;
	}

	// We're the parent.
	// Eagerly copy our entire address space into the child.
	// This is NOT what you should do in your fork implementation.
	for (addr = (uint8_t *)UTEXT; addr < end; addr += PGSIZE)
		duppage(envid, addr);

	// Also copy the stack we are currently running on.
	duppage(envid, ROUNDDOWN(&addr, PGSIZE));

	// Start the child environment running
	if ((r = sys_env_set_status(envid, ENV_RUNNABLE)) < 0)
		panic("sys_env_set_status: %e", r);

	return envid;
}
