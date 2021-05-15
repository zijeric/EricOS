// 乒乓是两个进程之间的计数器.
// 只需要开始其中的一个---- 用fork分成两个.

#include "inc/lib.h"

void
umain(int argc, char **argv)
{
	envid_t who;

	if ((who = fork()) != 0) {
		// Parent 让球滚起来
		// "send 0 from 父进程ID to 子进程ID"
		cprintf("send 0 from %x to %x\n", sys_getprocid(), who);
		// 仅发送值0到子进程，发现子进程尚未准备接收信息，让出CPU
		ipc_send(who, 0, 0, 0);
	}

	while (1) {
		// Child & Parent
		// 置接收状态、目标线性地址、阻塞态、RAX返回值(成功)、让出CPU
		// 调度、设置from发送进程ID(、设置perm_store)、返回发送进程发送的值
		uint32_t i = ipc_recv(&who, 0, 0);
		cprintf("%x got %d from %x\n", sys_getprocid(), i, who);
		if (i == 20)
			return;
		i++;
		// 仅发送值i到父进程，发现父进程尚未准备接收信息，让出CPU
		ipc_send(who, i, 0, 0);
		if (i == 20)
			return;
	}

}

