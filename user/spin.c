// 通过fork一个永远自旋的子进程来测试抢占调度
// 让它运行几个时间片后销毁它

#include "inc/lib.h"

void umain(int argc, char **argv)
{
	int32_t proc;

	cprintf("I am the parent.  Forking the child...\n");
	if ((proc = fork()) == 0)
	{
		cprintf("I am the child.  Spinning...\n");
		while (1)
			/* do nothing */;
	}

	cprintf("I am the parent.  Running the child...\n");
	sys_yield();
	sys_yield();
	sys_yield();
	sys_yield();
	sys_yield();
	sys_yield();
	sys_yield();
	sys_yield();

	cprintf("I am the parent.  Killing the child...\n");
	sys_env_destroy(proc);
}
