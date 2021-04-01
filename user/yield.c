// 放弃占用 CPU, 让位给其他环境

#include "inc/lib.h"

void
umain(int argc, char **argv)
{
	int i;

	cprintf("Hello, I am environment %08x.\n", thisenv->env_id);
	// 循环放弃i(5)次调度
	for (i = 0; i < 5; i++) {
		// 放弃当前环境调度，通过调度算法选择另一个环境
		sys_yield();
		// 第i次回到当前环境
		cprintf("Back in environment %08x, iteration %d.\n",
			thisenv->env_id, i);
	}
	// 结束循环
	cprintf("All done in environment %08x.\n", thisenv->env_id);
}
