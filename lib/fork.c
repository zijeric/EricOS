// 在用户态实现类Unix fork()

#include "inc/string.h"
#include "inc/lib.h"

// PTE_COW 标记COW式页表项.
// x86 页机制中在页表项里留出的几个特殊位供程序员自己定制的位，一共三位，区域为AVAIL，参考x86手册.
#define PTE_COW 0x800

/**
 * 自定义页错误处理函数.
 * 当出现页错误，主要是对标志为可写的或者COW的物理页分配新页，复制旧页的数据到新页并映射到旧页的地址空间
 */
static void
pgfault(struct UTrapframe *utf)
{
	void *addr = (void *)utf->utf_fault_va;
	uint32_t err = utf->utf_err;
	int r;

	// 检查出错的访问(1)写，(2)COW页.  若不是，调用panic().
	// pte_t entry = uvpt[VPN(addr)];
	// 分配一个新页，将其映射到临时位置(PFTEMP)，将旧页数据复制到新页，然后将新页移动到旧页的地址.
	if ((err & FEC_WR) && (uvpt[VPN(addr)] & PTE_COW))
	{
		if (sys_page_alloc(0, (void *)PFTEMP, PTE_U | PTE_P | PTE_W) == 0)
		{
			// 分配了新的物理页以后准备从原页面开始拷贝数据时，记得将地址和物理页大小对齐
			void *pg_addr = ROUNDDOWN(addr, PGSIZE);
			memmove(PFTEMP, pg_addr, PGSIZE);
			r = sys_page_map(0, (void *)PFTEMP, 0, pg_addr, PTE_U | PTE_W | PTE_P);
			if (r < 0)
			{
				panic("pgfault...something wrong with page_map");
			}
			r = sys_page_unmap(0, PFTEMP);
			if (r < 0)
			{
				panic("pgfault...something wrong with page_unmap");
			}
			return;
		}
		else
		{
			panic("pgfault...something wrong with page_alloc");
		}
	}
	else
	{
		panic("pgfault...wrong error %e", err);
	}
}

/**
 * 进行COW式的页复制.
 * 将父环境的页表空间映射到子环境，即共享数据，并都标记为COW，为了以后任意环境写数据时产生页错误，为其分配新的物理页
 * 映射: 将的虚拟页pn(地址: pn*PGSIZE)映射到相同虚拟地址的 envid 中.
 * 成功: 返回0，失败调用 panic()
 */
static int
duppage(envid_t envid, unsigned pn)
{
	int r;
	pte_t entry = uvpt[pn];
	void *addr = (void *)((uintptr_t)pn * PGSIZE);
	int perm = entry & PTE_SYSCALL;
	if (perm & PTE_SHARE)
	{
		r = sys_page_map(0, addr, envid, addr, perm);
		if (r < 0)
		{
			panic("Something went wrong on duppage %e", r);
		}
	}
	else if ((perm & PTE_COW) || (perm & PTE_W))
	{
		perm &= ~PTE_W;
		perm |= PTE_COW;
		r = sys_page_map(0, addr, envid, addr, perm);
		if (r < 0)
		{
			panic("Something went wrong on duppage %e", r);
		}
		r = sys_page_map(0, addr, 0, addr, perm);
		if (r < 0)
		{
			panic("Something went wrong on duppage %e", r);
		}
	}
	else
	{
		r = sys_page_map(0, addr, envid, addr, perm);
		if (r < 0)
		{
			panic("Something went wrong on duppage %e", r);
		}
	}
	return 0;
}

/**
 * 具有COW功能的用户级fork.
 *  1.调用 set_pgfault_handler() 对 pgfault() 错误处理函数进行注册
 *  2.调用sys_exofork()创建一个新的子环境
 *  3.映射父环境的可写或者COW的页到子环境的页表中，并都标记为COW
 *  4.为子环境分配用户异常栈
 *  5.传递用户级页错误处理函数指针给子环境
 *  6.将子环境标记为ENV_RUNNABLE并返回
 * 父环境: 返回子环境的 envid; 子环境: 返回0
 * 出错调用 panic()
 * 
 * 两个用户异常栈都不应该被标记为COW，所以必须为子环境的异常栈分配一个新页
 */
envid_t
fork(void)
{
	int r = 0;
	set_pgfault_handler(pgfault);
	envid_t childid = sys_exofork();
	if (childid < 0)
	{
		panic("\n couldn't call fork %e\n", childid);
	}
	if (childid == 0)
	{
		thisenv = &envs[ENVX(sys_getenvid())];
		// 子环境的返回值
		return 0;
	}
	r = sys_page_alloc(childid, (void *)(UXSTACKTOP - PGSIZE), PTE_P | PTE_W | PTE_U);
	if (r < 0)
		panic("\n couldn't call fork %e\n", r);

	uint64_t pml;
	uint64_t pdpe;
	uint64_t pde;
	uint64_t pte;
	uint64_t each_pde = 0;
	uint64_t each_pte = 0;
	uint64_t each_pdpe = 0;

	// 拷贝[0, UXSTACKTOP)的所有用户页，接下来为用户异常栈创建物理页
	// 因为在创建用户环境时，已经在 env_setup_vm() 中映射了内核所有页
	for (pml = 0; pml < VPML4E(UTOP); pml++)
	{
		if (uvpml4e[pml] & PTE_P)
		{

			for (pdpe = 0; pdpe < NPDPENTRIES; pdpe++, each_pdpe++)
			{
				if (uvpde[each_pdpe] & PTE_P)
				{

					for (pde = 0; pde < NPDENTRIES; pde++, each_pde++)
					{
						if (uvpd[each_pde] & PTE_P)
						{

							for (pte = 0; pte < NPTENTRIES; pte++, each_pte++)
							{
								if (uvpt[each_pte] & PTE_P)
								{

									if (each_pte != VPN(UXSTACKTOP - PGSIZE))
									{
										r = duppage(childid, (unsigned)each_pte);
										if (r < 0)
											panic("\n couldn't call fork %e\n", r);
									}
								}
							}
						}
						else
						{
							each_pte = (each_pde + 1) * NPTENTRIES;
						}
					}
				}
				else
				{
					each_pde = (each_pdpe + 1) * NPDENTRIES;
				}
			}
		}
		else
		{
			each_pdpe = (pml + 1) * NPDPENTRIES;
		}
	}

	extern void _pgfault_upcall(void);
	// 为子环境设置页错误处理函数.
	// 因为使用env_alloc()创建的env的处理函数指针都为空，但是这时已经明确的为其错误栈分配了物理页面
	// 所以可以直接使用系统调用指定错误处理的入口，_pgfault_upcall()为所有用户页错误处理函数的总入口
	r = sys_env_set_pgfault_upcall(childid, _pgfault_upcall);
	if (r < 0)
		panic("\n couldn't call fork %e\n", r);

	r = sys_env_set_status(childid, ENV_RUNNABLE);
	if (r < 0)
		panic("\n couldn't call fork %e\n", r);

	return childid;
}

