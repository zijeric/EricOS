#ifndef ALVOS_KERN_PMAP_H
#define ALVOS_KERN_PMAP_H
#ifndef ALVOS_KERNEL
#error "This is a AlvOS kernel header; user programs should not #include it"
#endif

#include "inc/memlayout.h"
#include "inc/assert.h"
struct Env;

// kern/entry.S 中设置的内核栈
extern char bootstacktop[], bootstack[];

// kern/pmap.c 中设置的页表数组(包含探测到的所有物理内存)
extern struct PageInfo *pages;
// kern/pmap.c 中探测到的物理内存所需要的页表数
extern size_t npages;

// kern/pmap.c 中设置的4级页表
extern pml4e_t *boot_pml4e;
// 在 boot/main.c 中加载的内核 ELF 映像的虚拟地址(+KERNBASE)
#define KELFHDR (0x10000 + KERNBASE)

/**
 * PADDR/_paddr: 将虚拟地址转化为物理地址，宏函数 PADDR 在输入的虚拟地址上减去 KERNBASE。
 */
#define PADDR(kva)                                                 \
	({                                                             \
		physaddr_t __m_kva = (physaddr_t)(kva);                    \
		if (__m_kva < KERNBASE)                                    \
			panic("PADDR called with invalid kva %08lx", __m_kva); \
		__m_kva - KERNBASE;                                        \
	})

/**
 * 宏函数 KADDR 调用了函数 _kaddr，将物理地址转化成虚拟地址，也就是在物理地址的数值上加上了 KERNBAE.
 * 
 * 内核有时候在仅知道物理地址的情况下，想要访问该物理地址，但是没有办法绕过MMU的线性地址转换机制，所以没有办法用物理地址直接访问。
 * 将虚拟地址 KERNBASE 映射到物理地址0x0处的一个原因就是希望能有一个简便的方式实现物理地址和线性地址的转换。
 * 在知道物理地址pa的情况下可以加 KERNBASE 得到对应的线性地址，可以用KADDR(pa)宏实现。
 * 在知道线性地址va的情况下减 KERNBASE 可以得到物理地址，可以用宏PADDR(va)实现。
 * 该宏采用虚拟地址 -- 指向 KERNBASE 之上的地址，在该地址上映射了机器最大 256MB 的物理内存并返回相应的物理地址。
 * 如果向其传递一个非虚拟地址，它会产生 panic 异常。
 */
#define KADDR(pa)                                                \
	({                                                           \
		physaddr_t __m_pa = (pa);                                \
		uint32_t __m_ppn = PPN(__m_pa);                          \
		if (__m_ppn >= npages)                                   \
			panic("KADDR called with invalid pa %08lx", __m_pa); \
		(void *)((uint64_t)(__m_pa + KERNBASE));                 \
	})

enum
{
	// 用于 page_alloc 函数，将返回的物理页清零.
	ALLOC_ZERO = 1 << 0,
	ALLOC_NONE,
};

void x64_vm_init();

void page_init(void);
struct PageInfo *page_alloc(int alloc_flags);
void page_free(struct PageInfo *pp);
int page_insert(pml4e_t *pml4e, struct PageInfo *pp, void *va, int perm);
void page_remove(pml4e_t *pml4e, void *va);
struct PageInfo *page_lookup(pml4e_t *pml4e, void *va, pte_t **pte_store);
void page_decref(struct PageInfo *pp);

void tlb_invalidate(pml4e_t *pml4e, void *va);

void *mmio_map_region(physaddr_t pa, size_t size);

int user_mem_check(struct Env *env, const void *va, size_t len, int perm);
void user_mem_assert(struct Env *env, const void *va, size_t len, int perm);

// 每个物理页对应一个strcut PageInfo和一个物理页号PGNUM和唯一的物理首地址
// 返回PageInfo结构pp对应物理页的物理首地址，physaddr_t(uint32_t) 自定义的物理地址类型
static inline physaddr_t
page2pa(struct PageInfo *pp)
{
	// pages: 物理页的状态描述数组首地址，(pp - pages)就是pp 在数组中的索引
	// 又因分配的所有物理内存都是页对齐，因此 索引*PGSIZE 就是对应的物理页首地址
	// (<<12 = *1000H = *PGSIZE)
	return (pp - pages) << PGSHIFT;
}

// 返回物理地址pa所在物理页对应的PageInfo结构
static inline struct PageInfo *
pa2page(physaddr_t pa)
{
	if (PPN(pa) >= npages)
		panic("pa2page called with invalid pa");
	return &pages[PPN(pa)];
}

// 返回PageInfo结构pp对应物理页的虚拟首地址：获取PageInfo结构对应的物理地址，再调用 KADDR 获取物理地址对应的虚拟地址
// 与 page2pa 类似，只不过返回的是 PageInfo 结构 pp 所对应的物理页的虚拟首地址PGNUM(20)
static inline void *
page2kva(struct PageInfo *pp)
{
	return KADDR(page2pa(pp));
}

pte_t *pgdir_walk(pde_t *pgdir, const void *va, int create);

pte_t *pml4e_walk(pml4e_t *pml4e, const void *va, int create);

pde_t *pdpe_walk(pdpe_t *pdpe, const void *va, int create);

#endif
