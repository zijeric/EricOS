// Ping-pong 是两个共享内存进程之间的计数器.
// 只需要启动其中一个 -- 使用sfork一分为二.

#include "inc/lib.h"

uint32_t val;

void
umain(int argc, char **argv)
{
	int32_t who;
	uint32_t i;

	i = 0;
	if ((who = fork()) != 0) {
		cprintf("i am %08x; thisproc is %p\n", sys_getprocid(), thisproc);
		// 开始
		cprintf("send 0 from %x to %x\n", sys_getprocid(), who);
		ipc_send(who, 0, 0, 0);
	}

	while (1) {
		ipc_recv(&who, 0, 0);
		cprintf("%x got %d from %x (thisproc is %p %x)\n", sys_getprocid(), val, who, thisproc, thisproc->proc_id);
		if (val == 10)
			return;
		++val;
		ipc_send(who, 0, 0, 0);
		if (val == 10)
			return;
	}

}
