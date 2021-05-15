// 多处理器的压力调度，验证一个进程不能同时运行在多个处理器(fork、内核锁、进程调度)

#include "inc/lib.h"  // 包含用户库

volatile int counter;

void umain(int argc, char **argv)
{
	int i, j;
	int seen;
	int32_t parent = sys_getprocid();

	// Fork 20个子进程
	for (i = 0; i < 20; i++)
		if (fork() == 0)
			break;
	if (i == 20)
	{
		cprintf("Children are computing...");
		// 父进程 fork 完成后放弃调度
		sys_yield();
		return;
	}

	// 子进程等待父进程完成所有fork
	while (procs[ENVX(parent)].env_status != ENV_FREE)
		asm volatile("pause");

	// 检查一个进程不能同时运行在两个处理器
	for (i = 0; i < 1000; i++)
	{
		sys_yield();
		for (j = 0; j < 10000; j++)
			counter++;
	}

	// 内核锁(互斥地系统调用): 如果进程运算的结果不等于循环自增的次数则说明进程同时运行在两个处理器
	if (counter != 1000 * 10000)
		panic("ran on two CPUs at once (counter is %d)", counter);

	// 检查进程运算结果
	cprintf("[%08x] counter: %d\n", thisproc->proc_id, counter);
	// 检查进程是否运行在不同处理器上
	cprintf("[%08x] stress sched on CPU %d\n", thisproc->proc_id, thisproc->env_cpunum);
}
