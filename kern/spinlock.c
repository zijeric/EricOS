// 实现(互斥)自旋锁的内核代码

#include "inc/types.h"
#include "inc/assert.h"
#include "inc/x86.h"
#include "inc/memlayout.h"
#include "inc/string.h"
#include "AlvOS/cpu.h"
#include "kern/spinlock.h"
#include "kern/kdebug.h"

/**
 * 大内核锁(单一的全局锁)
 */
struct spinlock kernel_lock = {
#ifdef DEBUG_SPINLOCK
	.name = "kernel_lock"
#endif
};

#ifdef DEBUG_SPINLOCK
// Record the current call stack in pcs[] by following the %ebp chain.
static void
get_caller_pcs(uint64_t pcs[])
{
	uint64_t *rbp;
	int i;

	rbp = (uint64_t *)read_rbp();
	for (i = 0; i < 10; i++)
	{
		if (rbp == 0 || rbp < (uint64_t *)ULIM)
			break;
		pcs[i] = rbp[1];		  // saved %rip
		rbp = (uint64_t *)rbp[0]; // saved %rbp
	}
	for (; i < 10; i++)
		pcs[i] = 0;
}

// Check whether this CPU is holding the lock.
static int
holding(struct spinlock *lock)
{
	return lock->locked && lock->cpu == thiscpu;
}
#endif

void __spin_initlock(struct spinlock *lk, char *name)
{
	lk->locked = 0;
#ifdef DEBUG_SPINLOCK
	lk->name = name;
	lk->cpu = 0;
#endif
}

// 获取锁
// 循环(自旋)，直到获得锁
// 长时间持有锁可能会导致其他 CPU 浪费时间来获取锁(效率低)
void spin_lock(struct spinlock *lk)
{
#ifdef DEBUG_SPINLOCK
	if (holding(lk))
		panic("CPU %d cannot acquire %s: already holding", cpunum(), lk->name);
#endif

		// xchg 是原子操作，而且是序列化的，因此调用 acquire() 之后的读取不会在它之前被重新排序
		while (xchg(&lk->locked, 1) != 0)
		asm volatile("pause");

		// 记录关于调试的获取锁信息.
#ifdef DEBUG_SPINLOCK
	lk->cpu = thiscpu;
	get_caller_pcs(lk->pcs);
#endif
}

// 释放锁.
void spin_unlock(struct spinlock *lk)
{
#ifdef DEBUG_SPINLOCK
	if (!holding(lk))
	{
		int i;
		uint32_t pcs[10];
		// Nab the acquiring EIP chain before it gets released
		memmove(pcs, lk->pcs, sizeof pcs);
		if (!lk->cpu)
			cprintf("CPU %d cannot release %s: not held by any CPU\nAcquired at:",
					cpunum(), lk->name);
		else
			cprintf("CPU %d cannot release %s: held by CPU %d\nAcquired at:",
					cpunum(), lk->name, lk->cpu->cpu_id);
		for (i = 0; i < 10 && pcs[i]; i++)
		{
			struct Ripdebuginfo info;
			if (debuginfo_rip(pcs[i], &info) >= 0)
				cprintf("  %08x %s:%d: %.*s+%x\n", pcs[i],
						info.rip_file, info.rip_line,
						info.rip_fn_namelen, info.rip_fn_name,
						pcs[i] - info.rip_fn_addr);
			else
				cprintf("  %08x\n", pcs[i]);
		}
		panic("spin_unlock");
	}

	lk->pcs[0] = 0;
	lk->cpu = 0;
#endif

	// The xchg serializes, so that reads before release are
	// not reordered after it.  The 1996 PentiumPro manual (Volume 3,
	// 7.2) says reads can be carried out speculatively and in
	// any order, which implies we need to serialize here.
	// But the 2007 Intel 64 Architecture Memory Ordering White
	// Paper says that Intel 64 and IA-32 will not move a load
	// after a store. So lock->locked = 0 would work here.
	// The xchg being asm volatile ensures gcc emits it after
	// the above assignments (and after the critical section).
	xchg(&lk->locked, 0);
}
