// 并发版的素数筛
// 由 Unix 管道的发明者 Doug McIlroy 发明.
// 参考 http://swtch.com/~rsc/thread/.


#include "inc/lib.h"

unsigned primeproc(void)
{
	int i, id, p;
	int32_t procid;

	// 从左边的邻居那里取一个质数
top:
	p = ipc_recv(&procid, 0, 0);
	cprintf("CPU %d: %d ", thisproc->env_cpunum, p);

	// fork 一个右边的邻居来延续链条
	if ((id = fork()) < 0)
		panic("fork: %e", id);
	if (id == 0)
		goto top;

	// 过滤掉质数的倍数
	while (1)
	{
		i = ipc_recv(&procid, 0, 0);
		if (i % p)
			ipc_send(id, i, 0, 0);
	}
}

void umain(int argc, char **argv)
{
	int i, id;

	// fork 链中的第一个主要过程
	if ((id = fork()) < 0)
		panic("fork: %e", id);
	if (id == 0) // 子进程
		primeproc();

	// 输入2500内的整数
	for (i = 2; i<=2500; i++)
		ipc_send(id, i, 0, 0);
}
