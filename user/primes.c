// 并发版的素数筛
// 由 Unix 管道的发明者 Doug McIlroy 发明.
// 参考 http://swtch.com/~rsc/thread/.
//
// 因为 AlvOS 的最大进程数 NPROCS 为 1024，创建第1023个进程就会触发异常
// 当前进程从父进程创建的管道读端读入所有的数，读到的第一个数必然是素数，输出
// fork 子进程的继续接收，子进程将处理剩余的数
// 父进程发送 除所有以第一个数为因子的和数 到子进程

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

	// 输入所有的整数
	for (i = 2; i <= 1000; i++)
		ipc_send(id, i, 0, 0);
}
