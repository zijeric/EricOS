// 让处理器调度其他进程

#include "inc/lib.h"

void umain(int argc, char **argv)
{
	int i;

	cprintf("Hello, I am process %08x.\n", thisproc->proc_id);
	for (i = 0; i < 5; i++)
	{
		sys_yield();
		cprintf("Back in process %08x, iteration %d.\n",
				thisproc->proc_id, i);
	}
	cprintf("All done in process %08x.\n", thisproc->proc_id);
}
