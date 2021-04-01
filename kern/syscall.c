// 系统调用实现代码
#include "inc/x86.h"
#include "inc/error.h"
#include "inc/string.h"
#include "inc/assert.h"

#include "AlvOS/env.h"
#include "kern/pmap.h"
#include "kern/trap.h"
#include "kern/syscall.h"
#include "AlvOS/console.h"
#include "kern/sched.h"

/**
 * 将字符串s打印到系统控制台，字符串长度正好是len个字符
 * 无权限访问内存就销毁环境
 */
static void
sys_cputs(const char *s, size_t len)
{
	// 调用user_mem_assert检查用户是否有权限读取内存[s, s+len]，如果没有就销毁环境
	user_mem_assert(curenv, s, len, 0);

	// 打印用户提供的字符串.
	cprintf("%.*s", len, s);
}

/**
 * 在不阻塞的情况下从系统控制台读取字符
 * 返回字符，如果没有输入等待，则返回0
 */
static int
sys_cgetc(void)
{
	return cons_getc();
}

/**
 * 返回当前环境的 envid.
 */
static envid_t
sys_getenvid(void)
{
	return curenv->env_id;
}

/**
 * 销毁envid对应的环境(也可以是当前运行的环境)
 * 
 * 成功返回0，错误返回: -E_BAD_ENV(当前环境envid不存在/调用者没有修改envid的权限)
 */
static int
sys_env_destroy(envid_t envid)
{
	int r;
	struct Env *e;

	// 当前环境envid不存在/调用者没有修改envid的权限
	if ((r = envid2env(envid, &e, 1)) < 0)
		return r;

	// 当前运行的环境
	if (e == curenv)
		cprintf("[%08x] exiting gracefully\n", curenv->env_id);
	// 非当前环境
	else
		cprintf("[%08x] destroying %08x\n", curenv->env_id, e->env_id);

	// 销毁
	env_destroy(e);
	return 0;
}

// 取消当前环境调度，并选择一个不同的环境运行.
static void
sys_yield(void)
{
	sched_yield();
}

/**
 * 创建一个子环境，寄存器集与父环境相同，状态设置为 ENV_NOT_RUNNABLE
 * 父环境返回新环境的 envid, 出错则返回负的错误代码: -E_NO_FREE_ENV, -E_NO_MEM
 * 子环境返回0.
 */
static envid_t
sys_exofork(void)
{
	/**
	 * 调用 env_alloc() 创建新环境.
	 * 调用后, 新环境除了设置状态为 ENV_NOT_RUNNABLE, 寄存器集从当前环境复制, 父id外, 其他保持不变.
	 */

	// 创建子环境
	struct Env *child;
	int result = env_alloc(&child, curenv->env_id);
	if (result < 0)
	{
		cprintf("sys_exofork(): %e \n", result);
		return result;
	}
	// 设置子环境状态为 ENV_NOT_RUNNABLE
	child->env_status = ENV_NOT_RUNNABLE;
	// 寄存器集从当前环境复制, 不再为子环境分配栈
	child->env_tf = curenv->env_tf;
	// 为子环境设置返回值 0
	child->env_tf.tf_regs.reg_rax = 0;
	// 子环境的父id
	child->env_parent_id = curenv->env_id;
	// 返回子环境的id
	return child->env_id;
}

/**
 * 环境的地址映射和寄存器状态初始化之后，修改环境状态为(ENV_RUNNABLE 或 ENV_NOT_RUNNABLE)
 * 
 * 成功返回0, 错误返回负的错误代码:
 *  -E_BAD_ENV: envid 不存在, 或调用者没有修改 envid环境的权限
 *  -E_INVAL: status 无效
 */
static int
sys_env_set_status(envid_t envid, int status)
{
	if ((status != ENV_RUNNABLE) && (status != ENV_NOT_RUNNABLE))
	{
		cprintf("sys_env_set_status(): improper status %e\n", -E_INVAL);
		return -E_INVAL;
	}
	struct Env *env;
	// 调用 envid2env() 赋值 envid 对应的环境结构指针到参数 env
	int result = envid2env(envid, &env, 1);
	if (result < 0)
	{
		cprintf("envid2env(): %e\n", result);
		return result;
	}
	// 修改环境的 status
	env->env_status = status;
	return 0;
}

/**
 * sys_env_set_pgfault_upcall() 将用户态的页错误处理函数注册到环境结构(为参数环境设置 env_pgfault_upcall)
 * 缺页中断发生时，用户栈切换到用户异常栈，并且 push UTrapframe 结构，执行 env_pgfault_upcall 指定位置的代码
 * 成功返回0, 错误返回负的错误代码:
 *  -E_BAD_ENV: envid 不存在, 或调用者没有修改 envid环境的权限
 */
static int
sys_env_set_pgfault_upcall(envid_t envid, void *func)
{
	struct Env *newenv;
	// 获取 envid 对应的环境结构，并检查操作权限
	int result = envid2env(envid, &newenv, 1);
	if (result < 0)
	{
		cprintf("sys_env_set_pgfault_upcall(): bad envid %e.\n", result);
		return result;
	}
	// 设置页错误向上调用的entry point
	newenv->env_pgfault_upcall = func;
	return 0;
}

/**
 * 分配一页物理内存，并将其以 perm 权限映射 envid 环境 va 所对应的地址空间
 * 对 pmap.c 中 page_alloc() 和 page_insert() 的封装
 * - 参数 perm 为 envid 对应环境的地址空间权限
 * - 清零页内容，防止脏数据产生异常
 * - 如果一个页已经映射到参数 va，那么该页将作为副作用取消映射
 * - 必须设置 perm -- PTE_U | PTE_P, PTE_AVAIL | PTE_W 可选，其他不可设置
 * 
 * 成功返回0，错误返回负数错误码:
 *  -E_BAD_ENV: envid 不存在，或调用者没有修改 envid环境的权限
 *  -E_INVAL: va>=UTOP，或 va 不是页对齐，或 perm 不符合
 *  -E_NO_MEM
 */
static int
sys_page_alloc(envid_t envid, void *va, int perm)
{
	// sys_page_alloc() 是对 page_alloc() 和 page_insert() 的封装
	//   但新增检查参数的正确性.
	// 如果 page_insert() 失败，会释放分配的物理页!
	struct Env *envnow;
	// 通过 envid 获取环境结构，并检查权限
	int result = envid2env(envid, &envnow, 1);
	if (result < 0)
	{
		cprintf("sys_page_alloc(): %e \n", result);
		return result;
	}
	// 分配一个空闲物理页
	struct PageInfo *pp = page_alloc(ALLOC_ZERO);
	// 以下为错误检查
	if (!pp)
	{
		cprintf("sys_page_alloc(): No memory to allocate page SYS_PAGE_ALLOC %e.\n", -E_NO_MEM);
		return -E_NO_MEM;
	}
	if ((uint64_t)va >= UTOP || PGOFF(va))
		return -E_INVAL;

	int newperm = PTE_U | PTE_P;
	if ((perm & newperm) != newperm || (perm & ~PTE_SYSCALL))
	{
		cprintf("sys_page_alloc(): permission error %e.\n", -E_INVAL);
		return -E_INVAL;
	}
	// 在内核4级页表，建立页表页的映射及权限，映射物理页到虚拟地址 va, 页表项的权限(低12位)设置为 perm|PTE_P
	if (page_insert(envnow->env_pml4e, pp, va, perm) < 0)
	{
		cprintf("sys_page_alloc(): No memory to allocate page SYS_PAGE_ALLOC %e.\n", -E_NO_MEM);
		// 内存不足则释放页结点回 page_free_list
		page_free(pp);
		return -E_NO_MEM;
	}
	// 将对应物理页清零
	memset(page2kva(pp), 0, PGSIZE);
	return 0;
}

/**
 * 将 srcenvid 地址空间中 srcva 的内存页映射到 dstenvid 地址空间中的 dstva，并设置页属性 perm
 * 从而两个环境以不同的权限访问同一个物理页(并非数据拷贝，而是共享一个页的地址，提高效率，增强系统灵活性)
 * 	-E_BAD_ENV: envid 不存在，或调用者没有修改 envid环境的权限
 * 	-E_INVAL: srcva/dstva>=UTOP，或 srcva/dstva 不是页对齐，或 perm 不符合
 * 	-E_INVAL: srcva 没有映射到 srcenvid 的地址空间
 * 	-E_INVAL: if (perm & PTE_W)，但是 srcva 是 srcenvid 的只读地址空间
 * 	-E_NO_MEM
 */
static int
sys_page_map(envid_t srcenvid, void *srcva,
			 envid_t dstenvid, void *dstva, int perm)
{
	// sys_page_alloc() 是对 page_lookup() 和 page_insert() 的封装.
	//   但新增检查参数的正确性.
	//   使用 page_lookup() 的第三个参数检查物理页的权限.
	// 通过 srcenvid/dstenvid 获取源环境和目标环境获，并检查权限
	struct Env *srcenv, *dstenv;
	int result1 = envid2env(srcenvid, &srcenv, 1);
	int result2 = envid2env(dstenvid, &dstenv, 1);
	// 检查 srcenvid/dstenvid 的正确性
	if (result1 < 0 || result2 < 0)
	{
		cprintf("sys_page_map(): envid2env error %e, %e.\n", result1, result2);
		return -E_BAD_ENV;
	}
	// 检查 srcva/dstva 的地址空间范围(<=UTOP 且 只有 PGSIZE 大小)
	if ((uintptr_t)srcva >= UTOP || (uintptr_t)dstva >= UTOP || PGOFF(srcva) || PGOFF(dstva))
	{
		cprintf("\n envid2env error %e sys_page_map\n", -E_INVAL);
		return -E_INVAL;
	}
	// 检查页请求是否正确 (srcva 是否映射到 srcenvid 的地址空间)
	struct PageInfo *map;
	pte_t *p_entry;
	// 在页式地址转换机制中查找 secenv 4级页表的线性地址 srcva 所对应的物理页
	map = page_lookup(srcenv->env_pml4e, srcva, &p_entry);
	if (!map)
	{
		cprintf("\n No page available or not mapped properly SYS_PAGE_ALLOC %e \n", -E_NO_MEM);
		return -E_NO_MEM;
	}
	// 检查权限是否符合
	int map_perm = PTE_P | PTE_U;
	if ((perm & map_perm) != map_perm || (perm & ~PTE_SYSCALL))
	{
		cprintf("sys_page_map(): permission error %e.\n", -E_INVAL);
		return -E_INVAL;
	}
	if ((perm & PTE_W) && !(*p_entry & PTE_W))
	{
		cprintf("sys_page_map(): permission error %e.\n", -E_INVAL);
		return -E_INVAL;
	}
	// 建立页表页的映射及权限，将物理页 map 映射到虚拟地址 dstva, 页表项的权限(低12位)设置为'perm|PTE_P'
	if (page_insert(dstenv->env_pml4e, map, dstva, perm) < 0)
	{
		cprintf("\n No memory to allocate page SYS_PAGE_MAP %e \n", -E_NO_MEM);
		return -E_NO_MEM;
	}
	return 0;
}

/**
 * 取消环境 envid 地址空间中线性地址 va 所对应页的映射，即删除线性地址 va 所对应的物理页
 * 成功时返回0，错误时返回负的错误码
 * 	-E_BAD_ENV: envid 不存在，或调用者没有修改 envid环境的权限
 * 	-E_INVAL: va>=UTOP，或 va 不是页对齐
 */
static int
sys_page_unmap(envid_t envid, void *va)
{
	// sys_page_unmap() 是对 page_remove() 的封装.
	struct Env *envnow;
	// 通过 envid 获取环境结构，并检查权限
	int result = envid2env(envid, &envnow, 1);
	if (result < 0)
	{
		cprintf("sys_page_unmap(): envid2env error %e.\n", result);
		return result;
	}
	if ((uint64_t)va >= UTOP || PGOFF(va))
		return -E_INVAL;
	// 删除 va 对应物理页帧映射，取消物理地址映射的虚拟地址，将映射页表中对应的项清零
	page_remove(envnow->env_pml4e, va);
	return 0;
}

/**
 * 发送一个消息到 envid 环境，当 srcva 为0时，传送64-bit，否则传送一页(可以传递更多数据，方便地设置和安排内存共享)
 * 传送一页，即将当前环境 srcva 地址处的页映射到接收环境同一地址处
 * 详细：
 * 如果srcva < UTOP，那么也发送当前映射在 srcva 的页面，这样接收者得到相同页的重复映射
 * 如果目标没有被阻塞，则发送失败，返回值为 -E_IPC_NOT_RECV，等待IPC
 * 发送失败也可能是下面列出的其他原因：
 * 否则，发送成功，目标的IPC字段更新如下:
 * - env_ipc_recving 被设置为0来阻止未来的发送;
 * - env_ipc_from 被设置为发送 envid;
 * - env_ipc_value 被设置为参数 value;
 * - env_ipc_perm 设置为'perm'如果页面被传输，否则0
 * 目标环境再次被标记为可运行的，从暂停的sys_ipc_recv系统调用返回0。(提示:sys_ipc_recv函数是否实际返回?)
 * 如果发送方想要发送一个页面，但接收方没有请求，那么就不会传输页面映射，但也不会发生错误。只有当没有错误发生时，IPC才会发生
 * 成功返回0，错误返回负数错误码：
 *  -E_BAD_ENV 如果环境envid当前不存在(不需要检查权限)
 *  -E_IPC_NOT_RECV 如果envid当前在sys_ipc_recv中没有阻塞，或者另一个环境管理首先发送
 *  -E_INVAL: 
 *    1.如果srcva < UTOP和perm不合适(参见sys_page_alloc)
 *    2.如果srcva < UTOP，但是srcva没有映射到调用者的地址空间
 *    3. if (perm & PTE_W)，但是srcva在当前环境的地址空间中是只读的
 *  -E_NO_MEM 如果envid的地址空间没有足够的内存来映射srcva
 */
static int
sys_ipc_try_send(envid_t envid, uint32_t value, void *srcva, unsigned perm)
{
	struct Env *recvr;
	int r = envid2env(envid, &recvr, 0);
	if (r < 0)
	{
		cprintf("\n Bad ENV\n");
		return r;
	}
	if (recvr->env_ipc_recving == 0)
	{
		return -E_IPC_NOT_RECV;
	}
	recvr->env_ipc_recving = 0;
	recvr->env_ipc_from = curenv->env_id;
	recvr->env_ipc_perm = 0;
	if ((srcva && (srcva < (void *)UTOP)) && ((recvr->env_ipc_dstva) && (recvr->env_ipc_dstva < (void *)UTOP)))
	{
		cprintf("\n Sending page to the receiver \n");
		if (PGOFF(srcva))
		{
			cprintf("\n Not pageAligned\n");
			return -E_INVAL;
		}
		int map_perm = PTE_U | PTE_P;
		if (((perm & map_perm) != map_perm) || (perm & ~PTE_SYSCALL))
		{
			cprintf("\nPermission error\n");
			return -E_INVAL;
		}
		pte_t *entry;
		struct PageInfo *map = page_lookup(curenv->env_pml4e, srcva, &entry);
		if (!(map) || ((perm & PTE_W) && !(*entry & PTE_W)))
		{
			cprintf("\n VA is not mapped in senders address space or Sending read only pages with write permissions not permissible\n");
			return -E_INVAL;
		}
		if (page_insert(recvr->env_pml4e, map, recvr->env_ipc_dstva, perm) < 0)
		{
			cprintf("\n No memory to map the page to target env\n");
			return -E_NO_MEM;
		}
		recvr->env_ipc_perm = perm;
	}
	recvr->env_ipc_value = value;
	recvr->env_status = ENV_RUNNABLE;
	return 0;
}

/**
 * 取消环境的执行，直到接收到消息
 * 使用struct Env的env_ipc_recving和env_ipc_dstva字段记录您想接收的数据，标记自己不可运行，然后放弃CPU
 * 如果'dstva'是< UTOP，那么你愿意接收一个页面的数据。“dstva”是应将发送页面映射到的虚拟地址
 * 这个函数只在出错时返回，但是系统调用最终会在成功时返回0
 * 错误时返回< 0。错误:
 *   -E_INVAL: 如果dstva < UTOP，但是dstva不是页面对齐的
 */
static int
sys_ipc_recv(void *dstva)
{
	curenv->env_ipc_recving = 1;
	if (dstva < (void *)UTOP)
	{
		if (PGOFF(dstva))
			return -E_INVAL;
	}
	curenv->env_ipc_dstva = dstva;
	curenv->env_status = ENV_NOT_RUNNABLE;
	curenv->env_tf.tf_regs.reg_rax = 0;
	sched_yield();
	return 0;
}

/**
 * syscall函数: 根据 syscallno 分派到对应的内核调用处理函数，并传递参数.
 * 参数:
 * syscallno: 系统调用序号(inc/syscall.h)，告诉内核要使用那个处理函数，进入寄存器eax
 * a1~a5: 传递给内核处理函数的参数，进入剩下的寄存器edx, ecx, ebx, edi, esi
 * 这些寄存器都在中断产生时被压栈了，可以通过Trapframe访问到
 */
int64_t
syscall(uint64_t syscallno, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
	// 调用对应于'syscallno'参数的函数. (0~12)
	int result = 0;
	switch (syscallno)
	{
	case SYS_cputs:
		sys_cputs((const char *)a1, (size_t)a2);
		break;

	case SYS_cgetc:
		result = sys_cgetc();
		break;

	case SYS_getenvid:
		result = sys_getenvid();
		break;

	case SYS_env_destroy:
		result = sys_env_destroy(a1);
		break;

	case SYS_page_alloc:
		result = sys_page_alloc((envid_t)a1, (void *)a2, (int)a3);
		break;

	case SYS_page_map:
		result = sys_page_map((envid_t)a1, (void *)a2, (envid_t)a3, (void *)a4, (int)a5);
		break;

	case SYS_page_unmap:
		result = sys_page_unmap((envid_t)a1, (void *)a2);
		break;

	case SYS_exofork:
		result = sys_exofork();
		break;

	case SYS_env_set_status:
		result = sys_env_set_status((envid_t)a1, (int)a2);
		break;

	// CPU 遇到页错误时，将导致页错误的线性地址保存到 CPU 中的 CR2
	case SYS_env_set_pgfault_upcall:
		result = sys_env_set_pgfault_upcall((envid_t)a1, (void *)a2);
		break;

	// 通过系统调用实现 用户环境调度
	case SYS_yield:
		sys_yield();
		break;

	case SYS_ipc_try_send:
		result = sys_ipc_try_send((envid_t)a1, (uint32_t)a2, (void *)a3, (unsigned)a4);
		break;

	case SYS_ipc_recv:
		result = sys_ipc_recv((void *)a1);
		break;

	// 未实现的系统调用
	case NSYSCALLS:
		result = -E_NO_SYS;
		break;

	default:
		result = -E_INVAL;
		break;
	}
	return result;
}
