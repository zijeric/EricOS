#include "inc/assert.h"
#include "inc/x86.h"
#include "kern/spinlock.h"
#include "kern/env.h"
#include "kern/pmap.h"
#include "kern/monitor.h"

void sched_halt(void);

/**
 * 通过系统调用 sys_yield() 进入，选择可运行(ENV_RUNNABLE)的用户环境并运行它.
 * - 不能同时在两个 CPU 上执行调度同一个环境
 *   区分环境是否正在某个 CPU 上运行的方法是：该环境的状态是否 ENV_RUNNING
 */
void sched_yield(void)
{
	/**
	 * 实现简单的轮询调度算法.
	 * CPU 在上次运行环境之后，在 envs[] 中以循环的方式搜索 ENV_RUNNABLE 环境，切换到找到的第一个环境.
	 * 如果没有可运行的环境，但是以前在 CPU 上运行的环境仍然是 ENV_RUNNING，那么选择这个环境.
	 * 不能选择当前正在另一个 CPU 上运行的环境(env_status == ENV_RUNNING)，如果没有可运行的环境，将会停止 CPU.
	 */

	// idle: 上次在当前 CPU 运行的环境
	struct Env *idle = thiscpu->cpu_env;
	int i = 0, k;

	// 获取上次运行环境的 id
	if (idle)
		i = ENVX(thiscpu->cpu_env->env_id);

	// 从上次运行环境的 id 开始，顺序循环搜索 ENV_RUNNABLE 环境
	for (k = 0; k < NENV; k++)
	{
		// 如果以前没有运行环境，则从数组的开头开始
		i = (i + 1) % NENV;
		// 若找到则切换环境
		if (envs[i].env_status == ENV_RUNNABLE)
		{
			env_run(&envs[i]);
			return;
		}
	}

	// 如果没有可运行的环境，但是上次在 CPU 上运行的环境仍然是 ENV_RUNNING，那么恢复这个环境
	// idle->env_status == ENV_RUNNING 是上次的运行环境，不能其他 CPU
	if (idle && idle->env_status == ENV_RUNNING)
	{
		env_run(idle);
	}

	// 没有可运行环境, 停止运行当前 CPU, sched_halt() 不会返回
	sched_halt();
}

// Halt this CPU when there is nothing to do. Wait until the
// timer interrupt wakes it up. This function never returns.

void sched_halt(void)
{
	int i;

	// For debugging and testing purposes, if there are no runnable
	// environments in the system, then drop into the kernel monitor.
	for (i = 0; i < NENV; i++)
	{
		if ((envs[i].env_status == ENV_RUNNABLE ||
			 envs[i].env_status == ENV_RUNNING ||
			 envs[i].env_status == ENV_DYING))
			break;
	}
	if (i == NENV)
	{
		cprintf("No runnable environments in the system!\n");
		while (1)
			monitor(NULL);
	}

	// Mark that no environment is running on this CPU
	curenv = NULL;
	lcr3(PADDR(boot_pml4e));

	// Mark that this CPU is in the HALT state, so that when
	// timer interupts come in, we know we should re-acquire the
	// big kernel lock
	xchg(&thiscpu->cpu_status, CPU_HALTED);

	// Release the big kernel lock as if we were "leaving" the kernel
	unlock_kernel();

	// Reset stack pointer, enable interrupts and then halt.
	asm volatile(
		"movq $0, %%rbp\n"
		"movq %0, %%rsp\n"
		"pushq $0\n"
		"pushq $0\n"
		"sti\n"
		"hlt\n"
		:
		: "a"(thiscpu->cpu_ts.ts_esp0));
}
