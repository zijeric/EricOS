// 证明 IPC 缺乏公平性
// 启动这个程序的三个实例，分别为procs 1、2和3
// (user/idle is env 0).

#include "inc/lib.h"

void umain(int argc, char **argv)
{
	int32_t who, id;

	id = sys_getprocid();

	if (thisproc == &procs[1])
	{
		while (1)
		{
			ipc_recv(&who, 0, 0);
			cprintf("%x recv from %x\n", id, who);
		}
	}
	else
	{
		cprintf("%x loop sending to %x\n", id, procs[1].proc_id);
		while (1)
			ipc_send(procs[1].proc_id, 0, 0, 0);
	}
}
