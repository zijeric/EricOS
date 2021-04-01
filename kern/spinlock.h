#ifndef ALVOS_INC_SPINLOCK_H
#define ALVOS_INC_SPINLOCK_H

#include "inc/types.h"

// 注释这行会禁用自旋锁调试
#define DEBUG_SPINLOCK

// 互斥锁.
struct spinlock
{
	unsigned locked; // 标志是否已获取锁.

#ifdef DEBUG_SPINLOCK
	// For debugging:
	// 锁的名词.
	char *name;

	// 指向持有锁的 CPU.
	struct CpuInfo *cpu;

	// 锁定该锁的调用堆栈(程序计数器数组).
	uintptr_t pcs[10];
	// The call stack (an array of program counters)
	// that locked the lock.
#endif
};

void __spin_initlock(struct spinlock *lk, char *name);
void spin_lock(struct spinlock *lk);
void spin_unlock(struct spinlock *lk);

#define spin_initlock(lock) __spin_initlock(lock, #lock)

extern struct spinlock kernel_lock;

/**
 * Big Kernel Lock (BKL) 大内核锁(单个的全局锁)
 * 在这种模型，在用户态中运行的环境可以同时运行在任何可用的 CPU 上，但是只有一个环境能够运行在内核态中
 * 而任何尝试进入内核模式的其它环境都被强制等待
 */
static inline void
lock_kernel(void)
{
	spin_lock(&kernel_lock);
}

static inline void
unlock_kernel(void)
{
	spin_unlock(&kernel_lock);

	// 通常我们不需要这样做，但是qemu一次只运行一个CPU，并且有很长的时间片
	// 没有暂停，这个CPU可能会在另一个CPU有机会获取锁之前重新获取锁
	// 导致某个CPU死锁
	asm volatile("pause");
}

#endif
