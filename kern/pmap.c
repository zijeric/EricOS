/**
 * 物理内存分配器：boot_alloc(), page_init(), page_alloc() 和 page_free().
 * x64_vm_init(): 初始化内核虚拟内存
 * void pointer，任何类型的指针都可以直接赋值给它，无需进行强制类型转换
 * 指针的类型用于每取多少字节将其视为对应类型的值 (char:1, int:2)
 */
#include "inc/x86.h"
#include "inc/mmu.h"
#include "inc/error.h"
#include "inc/string.h"
#include "inc/assert.h"

#include "kern/pmap.h"
#include "kern/kclock.h"
#include "kern/multiboot.h"
#include "kern/env.h"
#include "kern/cpu.h"

// boot 阶段的页表映射(5PGSIZE): - 1 pml4(包含1项)，2 pdpt(包含4项)，2 pde(包含2048个项)
// extern uint64_t pml4phys;
// #define BOOT_PAGE_TABLE_START ((uint64_t)KADDR((uint64_t)&pml4phys))
// #define BOOT_PAGE_TABLE_END ((uint64_t)KADDR((uint64_t)(&pml4phys) + 5 * PGSIZE))

// 通过 i386_detect_memory() 对 npages, npages_basemem 赋值
size_t npages;				  // 物理内存量(以页为单位)
static size_t npages_basemem; // 基本内存量(以页为单位)

// 通过 x86_vm_init() 对 boot_pml4e, boot_cr3, pages, page_free_lists 赋值
// 由内核初始化的4级页表
pml4e_t *boot_pml4e;
// 存储到CR3寄存器的 pml4 物理地址
physaddr_t boot_cr3;
// 所有 PageInfo 在物理内存连续存放于 pages 处，可以通过数组的形式访问各个 PageInfo，
// 而 pages 紧接于 boot_pml4e 页表之上，kernel 向上的高地址部分连续分布着 pages 数组
// 物理页状态(PageInfo)数组，数组中第 i 个成员代表内存中第 i 个 page
// 因此，物理地址和数组索引很方便相换算(<<PGSHIFT)
struct PageInfo *pages;					// 物理页状态(PageInfo)数组
static struct PageInfo *page_free_list; // 空闲物理页链表

// --------------------------------------------------------------
// 检测机器的物理内存设置.
// --------------------------------------------------------------

static int
nvram_read(int r)
{
	return mc146818_read(r) | (mc146818_read(r + 1) << 8);
}

/**
 * 读取 bootloader 传递给内核的 multiboot 信息
 * type: 可用内存，保留内存(包括设备的 IO 映射、为 BIOS 保留的空间或物理损坏的内存等)
 * e820 MEMORY MAP
	size: 20, physical address: 0x0000000000000000, length: 0x000000000009fc00, type: reserved
	size: 20, physical address: 0x000000000009fc00, length: 0x0000000000000400, type: availiable
	size: 20, physical address: 0x00000000000f0000, length: 0x0000000000010000, type: availiable
	size: 20, physical address: 0x0000000000100000, length: 0x000000000fedf000, type: reserved
	size: 20, physical address: 0x000000000ffdf000, length: 0x0000000000021000, type: availiable
	size: 20, physical address: 0x00000000fffc0000, length: 0x0000000000040000, type: availiable
	total: 4GB(0x100000000)
 */
static void
multiboot_read(multiboot_info_t *mbinfo, size_t *basemem, size_t *extmem)
{
	int i;

	memory_map_t *mmap_base = (memory_map_t *)(uintptr_t)mbinfo->mmap_addr;
	memory_map_t *mmap_list[mbinfo->mmap_length / (sizeof(memory_map_t))];

	cprintf("\ne820 MEMORY MAP\n");
	for (i = 0; i < (mbinfo->mmap_length / (sizeof(memory_map_t))); i++)
	{
		memory_map_t *mmap = &mmap_base[i];

		uint64_t addr = APPEND_HILO(mmap->base_addr_high, mmap->base_addr_low);
		uint64_t len = APPEND_HILO(mmap->length_high, mmap->length_low);

		// type: 1 可用内存，type: 2 保留内存(包括设备的 IO 映射、为 BIOS 保留的空间或物理损坏的内存等)
		cprintf("size: %d, physical address: 0x%016x, length: 0x%016x, type: %s\n", mmap->size,
				addr, len, (mmap->type > 1 ? "availiable" : "reserved"));

		if (mmap->type > 5 || mmap->type < 1)
			mmap->type = MB_TYPE_RESERVED;

		// Insert into the sorted list
		int j = 0;
		for (; j < i; j++)
		{
			memory_map_t *this = mmap_list[j];
			uint64_t this_addr = APPEND_HILO(this->base_addr_high, this->base_addr_low);
			if (this_addr > addr)
			{
				int last = i + 1;
				while (last != j)
				{
					*(mmap_list + last) = *(mmap_list + last - 1);
					last--;
				}
				break;
			}
		}
		mmap_list[j] = mmap;
	}
	cprintf("\n");

	// Sanitize the list
	for (i = 1; i < (mbinfo->mmap_length / (sizeof(memory_map_t))); i++)
	{
		memory_map_t *prev = mmap_list[i - 1];
		memory_map_t *this = mmap_list[i];

		uint64_t this_addr = APPEND_HILO(this->base_addr_high, this->base_addr_low);
		uint64_t prev_addr = APPEND_HILO(prev->base_addr_high, prev->base_addr_low);
		uint64_t prev_length = APPEND_HILO(prev->length_high, prev->length_low);
		uint64_t this_length = APPEND_HILO(this->length_high, this->length_low);

		// Merge adjacent regions with same type
		if (prev_addr + prev_length == this_addr && prev->type == this->type)
		{
			this->length_low = (uint32_t)prev_length + this_length;
			this->length_high = (uint32_t)((prev_length + this_length) >> 32);
			this->base_addr_low = prev->base_addr_low;
			this->base_addr_high = prev->base_addr_high;
			mmap_list[i - 1] = NULL;
		}
		else if (prev_addr + prev_length > this_addr)
		{
			//Overlapping regions
			uint32_t type = restrictive_type(prev->type, this->type);
			prev->type = type;
			this->type = type;
		}
	}

	for (i = 0; i < (mbinfo->mmap_length / (sizeof(memory_map_t))); i++)
	{
		memory_map_t *mmap = mmap_list[i];
		if (mmap)
		{
			if (mmap->type == MB_TYPE_USABLE || mmap->type == MB_TYPE_ACPI_RECLM)
			{
				if (mmap->base_addr_low < 0x100000 && mmap->base_addr_high == 0)
					*basemem += APPEND_HILO(mmap->length_high, mmap->length_low);
				else
					*extmem += APPEND_HILO(mmap->length_high, mmap->length_low);
			}
		}
	}
}

/**
 * 检测可以使用的内存大小
 * 1.通过 boot 阶段使用 BIOS 中 multiboot 信息(64-bit)
 * 2.通过汇编指令直接调用硬件(32-bit)
 * (更新全局变量 npages:总内存所需物理页的数目 & npages_basemem:0x000A0000，基本内存所需物理页数目，BIOS 之前)
 */
static void
i386_detect_memory(void)
{
	size_t npages_extmem;
	size_t basemem = 0;
	size_t extmem = 0;

	// 检查 bootloader 是否向内核传递了 multiboot 信息(64-bit)
	extern char multiboot_info[];
	uintptr_t *mbp = (uintptr_t *)multiboot_info;
	multiboot_info_t *mbinfo = (multiboot_info_t *)*mbp;

	// 如果传递了 multiboot 信息，则通过 multiboot 读取基本内存和拓展内存(64-bit)
	if (mbinfo && (mbinfo->flags & MB_FLAG_MMAP))
	{
		multiboot_read(mbinfo, &basemem, &extmem);
	}
	else
	{
		// 否则通过硬件检测(32-bit)
		basemem = (nvram_read(NVRAM_BASELO) * 1024);
		extmem = (nvram_read(NVRAM_EXTLO) * 1024);
	}

	assert(basemem);

	npages_basemem = basemem / PGSIZE;
	npages_extmem = extmem / PGSIZE;

	// 计算基本内存和扩展内存中可用的物理页数.
	if (npages_extmem)
		npages = (EXTPHYSMEM / PGSIZE) + npages_extmem;
	else
		npages = npages_basemem;

	if (nvram_read(NVRAM_EXTLO) == 0xffff)
	{
		// 拓展内存 > 16M(0x1000000) in blocks of 64k
		size_t pextmem = nvram_read(NVRAM_EXTGT16LO) * (64 * 1024);
		npages_extmem = ((16 * 1024 * 1024) + pextmem - (1 * 1024 * 1024)) / PGSIZE;
	}

	// 计算基本内存和扩展内存中可用的物理页数.
	if (npages_extmem)
		// 如果存在超过1MB的拓展内存(>16-bit)
		npages = (EXTPHYSMEM / PGSIZE) + npages_extmem;
	else
		// (16-bit)
		npages = npages_basemem;

	cprintf("Physical memory: %uM available, base = %uK, extended = %uM\n",
			npages * PGSIZE / (1024 * 1024),
			npages_basemem * PGSIZE / 1024,
			npages_extmem * PGSIZE / 1024 / 1024);

	// AlvOS 的物理内存硬件是固定的，只支持 256MB 的物理内存
	if (npages > ((255 * 1024 * 1024) / PGSIZE))
	{
		npages = (255 * 1024 * 1024) / PGSIZE;
		cprintf("Using only %uM of the available memory, npages = %d.\n", npages * PGSIZE / 1024 / 1024, npages);
	}
}

// --------------------------------------------------------------
// 在 UTOP 之上设置内存映射(虚拟地址映射到物理地址).
// --------------------------------------------------------------

static void mem_init_mp(void);
static void boot_map_region(pml4e_t *pml4e, uintptr_t va, size_t size, physaddr_t pa, int perm);
static void check_page_free_list(bool only_low_memory);
static void check_page_alloc(void);
static void check_boot_pml4e(pml4e_t *pml4e);
static physaddr_t check_va2pa(pde_t *pgdir, uintptr_t va);
static void page_check(void);

/**
 * 为了分配 pml4, pages[], envs[] 物理内存空间，需要一个临时的简单物理内存分配器
 * boot_alloc()只在 AlvOS 初始化其虚拟内存系统时使用，page_alloc()才是真正的页分配器
 * 
 * 如果 n > 0，则分配足够容纳'n'个字节的连续物理内存页(不初始化该内存)，返回虚拟地址
 * 如果 n == 0，则不分配内存就返回下一个空闲页的地址
 * 
 * 如果内存不足，boot_alloc 会崩溃而调用panic()
 * 这也是为什么 boot_alloc 要检查是否分配了超过 va(0xfefd000) 的原因 boot_alloc 函数只用来分配：
 *  1.boot_pml4e: PGSIZE 4KB
 * (64-bit CPU 抽象成4级页表的原因就是为了通过一个顶级页表(CR3)以及分页硬件(MMU)能访问所有的对应于任一物理内存的物理页帧)
 *  2.pages[65280]: 255*4KB=1020KB (0xff000)
 *  3.envs[NENV]: 72*4KB=288KB (0x48000)
 */
static void *
boot_alloc(uint32_t n)
{
	// 下一个空闲内存(地址空间)首字节的虚拟地址
	static char *nextfree;
	// 用 char *result 返回 nextfree 所指向的地址
	char *result;

	// end_debug 在 kern/init.c 中初始化 (内核 + 内核 DWARF 调试段信息) 后的首地址
	// 很巧妙的利用了局部静态变量 nextfree 没有显式的赋值初始化的时候，会默认初始化为0，并且只初始化一次
	// 如果这是第一次，将初始化 nextfree
	if (!nextfree)
	{
		extern uintptr_t end_debug;
		nextfree = ROUNDUP((char *)end_debug, PGSIZE);
	}
	// 从 end_debug 开始都是没有分配的虚拟地址空间，分配n字节的空间，更新 nextfree 并且保持对齐

	// 分配足够大的内存块以容纳'n'个字节，然后更新 nextfree
	// 确保 nextfree 与 PGSIZE 的倍数保持对齐.
	result = nextfree;
	if (n > 0)
	{
		uint32_t newSize = ROUNDUP(n, PGSIZE);
		cprintf("boot_alloc: \n newSize: %p \n end of newSize: %p \n end of usable Mem: %p \n AlvOS bootstrap va(0x100000): %p \n AlvOS kernel va(0x200000): %p \n mapping base pa(KERNBASE): %p \n",
				newSize, nextfree + newSize, KADDR(0xfefd000), KADDR(0x200000), PADDR(KERNBASE));
		// 处理 PC 内存不足的情况：KADDR(0xfefd000) -- 受限于256MB，e820映射给出的内核可用地址空间的最后一个地址.
		//last address of the usable address space that the e820 map gives.
		if (nextfree + newSize > (char *)KADDR(0xfefd000))
			panic("No memory available in boot_alloc");
		nextfree = nextfree + newSize;
		cprintf("boot_alloc:\n next_free: %p\n");
	}
	return result;
}

/**
 * boot_pml4e 是4级页表基址的线性(虚拟)地址
 * 此函数只设置整个地址空间中的内核部分(即 addresses >= UTOP)
 * 由 kern/env.c 设置地址空间的用户部分
 * [UTOP, ULIM], 用户: R/- (不可写), [ULIM, ...], 用户: -/- (不可读写).
 * 
 * mem_init()	// 初始化内存管理(4级页表映射)
 * - i386_detect_memory()	// 通过汇编指令直接调用硬件查看可以使用的内存大小
 * - boot_alloc()	// 页表映射尚未构建时的内存分配器 -> boot_pml4e, pages[npages], envs[NENV]
 * - page_init()	// 初始化 pages[] 中的每一个物理页(通过页表映射整个物理内存空间进行管理)，建立 page_free_list 链表从而使用page分配器 page_alloc(), page_free()
 *   - boot_alloc(0)	// 获取内核后第一个空闲的物理地址空间(页对齐)
 * - boot_map_region()	// 更新参数4级页表映射流程pml4->pdpe->pde的表项pte，将线性地址[va, va+size]映射到物理地址[pa, pa+size](PPN + perm)
 *   - pml4e_walk()		// 创建4级页表映射: 根据参数线性地址va执行页式地址转换机制，返回页表页项pte的虚拟地址(物理页的基址)，只映射(创建)4级页表项，无关页表页与物理页帧
 *     - page_alloc()	// 通过 page_free_list 分配页(即从空闲链表取出结点)
 * 
 * boot_map_region()主要映射了四个区域：
 * 1.第一个是将虚拟地址空间[UPAGES, UPAGES+PTSIZE)映射到页表存储的物理地址空间 [pages, pages+PTSIZE)
 *   这里的PTSIZE代表页式内存管理所占用的空间(不包括4级页表)
 * 
 * 2.第一个是将虚拟地址空间[UENVS, UENVS+PTSIZE)映射到envs数组存储的物理地址空间 [envs, envs+PTSIZE)
 *   这里的PTSIZE代表用户环境管理所占用的空间
 * 
 * 3.第三个是将虚拟地址空间[KSTACKTOP-KSTKSIZE, KSTACKTOP) 映射到物理地址空间[bootstack,bootstack+32KB)
 *   KERNBASE以下的8个物理页大小用作内核栈，栈向下拓展，栈底EBP 栈顶ESP
 * 
 * 4.第四个则是映射整个内核的虚拟地址空间[KERNBASE, 2^32-KERNBASE)到物理地址空间[0,256M)
 *   涵盖了所有物理内存
 */
void x64_vm_init(void)
{
	pml4e_t *pml4e;
	struct Env *env;

	// 1.通过 multiboot/硬件 查看可以使用的内存大小 (底层 kern/kclock.c)
	// 赋值全局变量 npages(总内存量所需的页表个数) 和 npages_basemem(基础内存量所需页表个数)，用于计算 PageInfo 个数
	// base memory: [0x1000, 0xA0000), BIOS: [0xA0000, 0x100000), extmem: [0x100000, 0x10000000)
	i386_detect_memory();

	//////////////////////////////////////////////////////////////////////
	// 2.为了替换 kern/entry.S 中临时的 entry_pgdir 创建内核的4级页表(一个页)，并设置权限.
	cprintf("x64_vm_init: allocate memory for pml4e.\n");
	pml4e = boot_alloc(PGSIZE);
	memset(pml4e, 0, PGSIZE);
	// 更新全局4级页表变量 boot_pml4e
	boot_pml4e = pml4e;
	// boot_cr3 存储第4级页表页的基址
	boot_cr3 = PADDR(pml4e);

	//////////////////////////////////////////////////////////////////////
	// 分配 通过4级页表管理内核所有地址空间 所需要的空间
	// 分配 npages 个 PageInfo 结构体的数组并将其存储在数组'pages'中
	// 内核使用 pages 数组来跟踪物理页：
	// pages 数组的每一项是一个 PageInfo 结构，对应一个物理页的信息
	// npages 是管理内存所需要的物理页数，调用 memset 将每个PageInfo结构体的所有字段初始化为 0(present-bit)
	cprintf("x64_vm_init: allocate memory for pages[%d].\n", npages);
	pages = (struct PageInfo *)boot_alloc(sizeof(struct PageInfo) * npages);
	// memset(pages, 0, pages_size); 根据 ELF 格式分析可知，未初始化变量在 .bss 段会被置零

	//////////////////////////////////////////////////////////////////////
	// 分配 管理环境内存 所需要的空间
	// 给 NENV 个 Env 结构体在内存中分配空间，envs 存储该数组的首地址
	// (struct Env *) envs 是指向所有环境链表的指针，其操作方式跟内存管理的 pages 类似
	cprintf("x64_vm_init: allocate memory for envs[%d].\n", NENV);
	envs = (struct Env *)boot_alloc(sizeof(struct Env) * NENV);
	memset(envs, 0, NENV * sizeof(struct Env));
	// size_t end_envs = PPN(PADDR(0x80045a4000));
	// cprintf("end_envs: %p\n", end_envs);

	//////////////////////////////////////////////////////////////////////
	// Now that we've allocated the initial kernel data structures, we set
	// up the list of free physical pages. Once we've done so, all further
	// memory management will go through the page_* functions. In
	// particular, we can now map memory using boot_map_region or page_insert
	// 3. 初始化 pages[] 中的每一项(通过页表映射整个物理内存空间进行管理)
	// 建立 page_free_list 链表从而使用 page分配器 page_alloc(), page_free() 管理物理页
	page_init();

	//////////////////////////////////////////////////////////////////////
	// 现在 pages[] 存储了所有物理页的信息，page_free_list 链表记录所有空闲的物理页
	// 可以用 page_alloc() 和 page_free() 进行分配和回收
	// 并使用 boot_map_region(), page_insert() 进行页表映射，page_remove() 取消映射
	//////////////////////////////////////////////////////////////////////

	/**
	 * 以下设置虚拟内存，主要映射了四个区域：
	 * 1.第一个是 [UPAGES, UPAGES + size of pages[])映射到页表存储的物理地址 [pages, pages + size of pages[]) 0xff000=1020KB
	 *   这里的PTSIZE代表页式内存管理所占用的空间(不包括4级页表)
	 * 
	 * 2.第二个是 [UENVS, UENVS + size of envs[])映射到envs数组存储的物理地址 [envs, envs + size of envs[]) 0x48000=120KB
	 *   这里的PTSIZE代表页式内存管理所占用的空间(不包括4级页表)
	 * 
	 * 3.第三个是 [KSTACKTOP-KSTKSIZE, KSTACKTOP) 映射到[bootstack, bootstack+64KB) 64KB
	 *   KERNBASE以下的NCPUS(8)个物理页大小用作内核栈，栈向下拓展，栈指针 RSP
	 * 
	 * 4.第四个则是映射整个内核的虚拟空间[KERNBASE, KERNBASE + (npages * PGSIZE)]到 物理地址 [0, 0xff00000)  255MB
	 *   涵盖了所有物理内存
	 */
	//////////////////////////////////////////////////////////////////////
	// [UPAGES, sizeof(pages)] => [pages, sizeof(pages)]
	// 映射页式内存管理所占用的空间：将虚拟地址的 UPAGES 映射到物理地址pages数组开始的位置
	// pages 将在 地址空间 UPAGES 中映射内存(权限：用户只读)，以便于所有页表页和物理页帧能够从这个数组中读取
	// 权限: 内核 R-，用户 R-
	size_t pg_size = ROUNDUP(npages * (sizeof(struct PageInfo)), PGSIZE);
	boot_map_region(boot_pml4e, UPAGES, pg_size, PADDR(pages), PTE_U | PTE_P);
	cprintf("pg_size: %p\n", pg_size);

	//////////////////////////////////////////////////////////////////////
	// [UENVS, sizeof(envs)] => [envs, sizeof(envs)]
	// 将 UENV 所指向的虚拟地址开始 的空间(权限：用户只读)映射到 envs 数组的首地址，所以物理页帧权限被标记为PTE_U
	// 与 pages 数组一样，envs 也将在 地址空间UENVS 中映射用户只读的内存，以便于用户环境能够从这个数组中读取
	// 权限: 内核 R-，用户 R-
	size_t env_size = ROUNDUP(NENV * (sizeof(struct Env)), PGSIZE);
	boot_map_region(boot_pml4e, UENVS, env_size, PADDR(envs), PTE_U | PTE_P);
	cprintf("env_size: %p\n", env_size);
	// 注意，pages和envs本身作为内核代码的数组，拥有自己的虚拟地址，且内核可对其进行读写
	// boot_map_region函数将两个数组分别映射到了UPAGES和UENVS起 4M空间的虚拟地址，这相当于另外的映射镜像，
	// 4级页表项权限被设为用户/内核只可读，因此通过UPAGES和UENVS的虚拟地址去访问pages和envs的话，只能读不能写

	//////////////////////////////////////////////////////////////////////
	// [KSTACKTOP, KSTKSIZE) => [bootstack, KSTKSIZE), KSTKSIZE=16*PGSIZE
	// 使用 bootstack 所指的物理内存作为内核堆栈。内核堆栈从虚拟地址 KSTACKTOP 向下扩展
	// 设置从[KSTACKTOP-PTSIZE, KSTACKTOP]整个范围都是内核堆栈，但是把它分成两部分:
	// [KSTACKTOP-KSTKSIZE, KSTACKTOP) ---- 由物理内存支持，可以被映射
	// [KSTACKTOP-PTSIZE, KSTACKTOP-KSTKSIZE) ---- 没有物理内存支持，不可映射
	// 因此，如果内核栈溢出将会触发 panic，而不是覆盖内存，类似规定被称为"守护页"
	// 权限: 内核 RW，用户 NONE
	boot_map_region(boot_pml4e, (KSTACKTOP - KSTKSIZE), KSTKSIZE, PADDR(bootstack), PTE_P | PTE_W);
	cprintf("bootstack: %p\n", PADDR(bootstack));
	// 仅映射[KSTACKTOP-KSTKSIZE, KSTACKTOP)，即基址:KSTACKTOP-KSTKSIZE, 拓展偏移:KSTKSIZE

	//////////////////////////////////////////////////////////////////////
	// [KERNBASE, KERNBASE + (npages * PGSIZE)] => [0, 0x10000000)   <256MB
	// 在 KERNBASE及以上地址 映射内核的所有物理内存 (npages * PGSIZE)
	// 事实上内核所占的物理内存不一定有 256MB 那么大
	// 权限: 内核 RW，用户 NONE
	boot_map_region(boot_pml4e, KERNBASE, npages * PGSIZE, (physaddr_t)0x0, PTE_P | PTE_W);
	cprintf("kern_size: %p\n", npages * PGSIZE);

	// 初始化内存映射中与 SMP 相关的部分
	mem_init_mp();

	check_boot_pml4e(boot_pml4e);

	//////////////////////////////////////////////////////////////////////
	// 权限: 内核 RW, 用户 -

	// pml4_virt: boot[0, 128MB], kernel[KERNBASE, KB+128MB]
	// 将 cr3 寄存器存储的 pml4virt 临时4级页表切换到新创建的完整 boot_cr3 4级页表
	// eip 指令指针现在位于[KERNBASE, KERNBASE+4MB]内（在执行内核代码），为了4级页表切换时 CPU 执行内核不产生冲突
	// boot_cr3 在KERNBASE以上区间的映射包含了 pml4virt，且管理了pages, envs, kern_stack
	// MMU 分页硬件在进行页式地址转换时会自动地从 CR3 中取得4级页表地址
	lcr3(boot_cr3);
	cprintf("boot_cr3: %p\n",boot_cr3);

	// pdpe_t *pdpe = KADDR(PTE_ADDR(pml4e[1]));
	// pde_t *pgdir = KADDR(PTE_ADDR(pdpe[0]));

	check_page_free_list(1);
	check_page_alloc();
	page_check();

	check_page_free_list(0);
}

/**
 * 为了支持 SMP(Symmetrical Multi-Processing 对称多处理器)，修改kern_pgdir中的映射
 *   - 映射区域 [KSTACKTOP-PTSIZE, KSTACKTOP) 中的 per-CPU 的内核栈
 * 给每个栈分配KSTKSIZE大小，中间留出KSTKGAP作为保护，使得一个栈溢出一定不会影响相邻的栈：
 * 
 *    KERNBASE, ---->  +------------------------------+ 0xf0000000      --+
 *    KSTACKTOP        |     CPU0's Kernel Stack      | RW/--  KSTKSIZE   |
 *                     | - - - - - - - - - - - - - - -|                   |
 *                     |      Invalid Memory (*)      | --/--  KSTKGAP    |
 *                     +------------------------------+                   |
 *                     |     CPU1's Kernel Stack      | RW/--  KSTKSIZE   |
 *                     | - - - - - - - - - - - - - - -|                 PTSIZE
 *                     |      Invalid Memory (*)      | --/--  KSTKGAP    |
 *                     +------------------------------+                   |
 *                     :              .               :                   |
 *                     :              .               :                   |
 *    MMIOLIM ------>  +------------------------------+ 0xefc00000      --+
 *
 * 对于 CPUi，使用 percpu_kstacks[i] 引用的物理内存作为其内核栈
 * CPUi 的内核栈从虚拟地址 kstacktop_i = KSTACKTOP - i * (KSTKSIZE + KSTKGAP) 向下增长，分为两部分，
 * 如同在 pmap.c 中的 mem_init() 设置的单一栈:
 *     * [kstacktop_i - KSTKSIZE, kstacktop_i)
 *          -- 实际映射在物理内存
 *     * [kstacktop_i - (KSTKSIZE + KSTKGAP), kstacktop_i - KSTKSIZE)
 *          -- 作为保护页，不映射到物理内存; 因此，如果内核的栈溢出，就会出错，而不会覆盖另一个 CPU 的栈
 */
static void
mem_init_mp(void)
{
	// ncpus: nth CPU
	size_t ncpus = 0;
	for (; ncpus < NCPU; ncpus++)
	{
		// per-CPU 的内核栈基址
		size_t kstacktop_ncpus = KSTACKTOP - ncpus * (KSTKSIZE + KSTKGAP);

		// 参照pic/虚拟内存映射.png，将内核栈映射到 KSTACKTOP 下的 CPUi kernel stack，注意它们之间的保护页
		// kstacktop_i - KSTKSIZE: 从基址开始往上映射
		boot_map_region(boot_pml4e,
						kstacktop_ncpus - KSTKSIZE,
						KSTKSIZE, PADDR(percpu_kstacks[ncpus]),
						PTE_P | PTE_W);
	}
}

// --------------------------------------------------------------
// 跟踪物理页
// 初始化之前分配的 pages[]，pages[] 在每个物理页上都有一个'struct PageInfo'项
// 物理页被引用次数，并且构建一个PageInfo链表，保存空闲的物理页，表头是全局变量page_free_list
// --------------------------------------------------------------

//
// Initialize page structure and memory free list.
// After this is done, NEVER use boot_alloc again.  ONLY use the page
// allocator functions below to allocate and deallocate physical
// memory via the page_free_list.
//
void page_init(void)
{
	// 总结:
	//  1.[0, PGSIZE): 存放实模式的中断向量表IDT以及BIOS的相关载入程序
	//  2.[MPENTRY_PADDR, MPENTRY_PADDR+PGSIZE]: 存访 APs 的 bootstrap, size:0x1000
	//  3.[IOPHYSMEM, EXTPHYSMEM): 存放 I/O 所需要的空间，比如VGA的一部分显存直接映射这个地址
	//    [EXTPHYSMEM, end): 存放操作系统内核 kernel
	//    [end, end_debug]: 存放内核各段的 DWARF 调试信息
	//    [PADDR(boot pml4), PADDR(boot pml4) + PGSIZE): 存放 pml4
	//    [PADDR(pages[65280]), boot freemem): 存放 pages[], size:0xff000
	//    [PADDR(envs[NENV]), boot freemem]: 存放 envs[], size:0x48000
	// 	但是除了1、2项之外，后面的区域实际上是一段连续内存[IOPHYSMEM, boot freemem)，
	//  所以实现时，用排除法(不在以上3种情况)，加入空闲页链表里
	size_t i;
	struct PageInfo *last = NULL;
	for (i = 0; i < npages; i++)
	{
		pages[i].pp_ref = 0;
		pages[i].pp_link = NULL;
		// 1.[0, PGSIZE): 存放实模式的中断向量表IDT以及BIOS的相关载入程序.
		if (i > 1 && last)
			last->pp_link = &pages[i];
		else
			page_free_list = &pages[i];

		// 3.[IOPHYSMEM, EXTPHYSMEM): 存放 I/O 所需要的空间，比如VGA的一部分显存直接映射这个地址.
		if (page2pa(&pages[i]) >= PADDR(KERNBASE + IOPHYSMEM) && page2pa(&pages[i]) <= PADDR(boot_alloc(0)))
		{
			pages[i].pp_link = NULL;
			pages[i].pp_ref += 1;
		}
		// 2.[MPENTRY_PADDR, MPENTRY_PADDR+PGSIZE]: 存访 APs 的 bootstrap, size:0x1000
		else if (page2pa(&pages[i]) == MPENTRY_PADDR)
		{
			pages[i].pp_link = NULL;
			pages[i].pp_ref += 1;
		}
		// 用排除法(不在以上3种情况)，加入空闲页链表里
		else
			last = &pages[i];
	}
}

/**
 * page_alloc() 函数将从 pages 数组空间中由后往前分配，通过使用 page_free_list 指针和 pp_link 成员返回链表第一个 PageInfo 结构地址
 * 当传入参数标识非 0 时，分配的空间将被清零。虽然一开始 pages 数组空间被清零，但此刻分配的空间可能是之前被使用后回收的，不一定为空
 * 
 * 具体实现：
 * (alloc_flags & ALLOC_ZERO)==true，则通过 page2kva() 和 memset() 用'\0'填充整个返回的物理页
 * 不增加物理页的引用次数，如果确实需要增加引用次数，调用者必须显式地调用 page_insert()
 * 确保将分配物理页的 pp_link 字段设置为 NULL，这样 page_free() 检查就可以双重保证
 * (pp->pp_ref == 0 和 pp->pp_link == NULL)
 * 如果空闲内存不足，返回 NULL
 * 
 * 需要注意的是，不需要增加 PageInfo 的 pp_ref 字段
 * 为什么 page_alloc() 不需要，在 pdgir_walk() 与 page_insert() 中却都要？
 * 每一次page被映射到一个虚拟地址va的时候需要增加 pp_ref，而如果取消映射就得减少
 * page_alloc() 只是分配了物理页，并没有与虚拟地址建立映射，故不需要改pp_ref值
 */
struct PageInfo *
page_alloc(int alloc_flags)
{
	// 获取空闲页链表的第一个物理页结点 phypage，准备从链表取出
	struct PageInfo *phypage = page_free_list;
	// 存在空闲的内存，更新空闲页链表
	if (phypage)
	{
		// 与普通链表取出结点的步骤相同
		// 将 page_init() 组织的空闲页链表 page_free_list 的第一个页结点取出，将头指针指向下一个页结点
		// 空闲页链表page_free_list指向*准备取出的页结点*的下一个页结点
		// 判断是否为最后一个页结点
		if (phypage->pp_link == phypage)
			page_free_list = NULL;
		else
			page_free_list = phypage->pp_link;
		// 为了能在 page_free() 双重错误检查，将*准备取出的页结点*的 pp_link 设置为 NULL
		phypage->pp_link = NULL;

		// alloc_flags 和 ALLOC_ZERO 进行与运算，非0:需要将返回的页清零
		// 一定记得 memset() 参数使用的是虚拟地址
		// 获取*准备取出的页结点*对应的虚拟地址，
		// 调用 memset 函数将*准备取出的页结点*对应物理页页表的虚拟地址 PGSIZE 字节清零，确保所有的 PTE_P 都是'\0'
		if (alloc_flags & ALLOC_ZERO)
			memset(page2kva(phypage), '\0', PGSIZE);
		if (alloc_flags == 0)
			memset(page2kva(phypage), 0, PGSIZE);
		return phypage;
	}
	// 空闲内存不足，返回 NULL
	return NULL;
}

/**
 * 从空闲链表头添加函数参数PageInfo结点，page_free_list 相当于栈，后进先出
 * 将一个物理页返回到空闲链表(只有当 pp->pp_ref 等于0时才应该调用page_free)
 */
void page_free(struct PageInfo *pp)
{
	if (pp)
	{
		// 只有当 pp->pp_ref 等于0 且 pp->pp_link 等于NULL时，才应该调用page_free
		if (pp->pp_link || pp->pp_ref)
			panic("page_free: Not able to free page either being used by something else or is already free.\n");

		struct PageInfo *last = page_free_list;
		// 判断当前空闲页链表是否为空
		if (last)
		{
			// 非空，插入 pp 结点
			pp->pp_link = page_free_list;
			page_free_list = pp;
		}
		else
		{
			// page_free_list 指向 pp
			page_free_list = pp;
			// 为了最后一个结点就不会指向 NULL，pp 指向自身
			pp->pp_link = pp;
		}
	}
}

/**
 * 减少物理页PageInfo结构上的引用计数，如果引用次数为0，就释放结构对应的物理页
 */
void page_decref(struct PageInfo *pp)
{
	// 减少物理页上的引用计数
	if (--pp->pp_ref == 0)
		// 如果引用次数为0，就释放它
		page_free(pp);
}

/**
 * 根据参数 pml4 pointer，pml4e_walk() 翻译4级页表映射，返回一个指向虚拟地址 va 的页表项(PTE)的指针
 * 
 * 相关的4级页表指针页(PDPE)可能不存在
 * 如果不存在，并且 create == false，那么 pml4e_walk() 返回 NULL
 * 否则，pml4e_walk() 使用 page_alloc() 分配一个新的PDPE页
 *  - 如果分配失败，pml4e_walk() 返回 NULL
 *  - 否则，增加新物理页的引用次数并将其清空，根据返回参数 pdpe_t pointer 调用pdpe_walk()
 * pdpe_walk() 将获取4级页表指针 pde pointer，进而获取页表项 PTE
 * 如果 pdpe_walk() 返回 NULL
 *  - 为 pdpe pointer 分配的页面(如果是新分配的)应该被释放
 */
pte_t *
pml4e_walk(pml4e_t *pml4e, const void *va, int create)
{
	// 内核 pml4e 唯一，通过 pml4e 可以索引到整个4级页表表
	if (pml4e)
	{
		// pml4e[PML4(va)]: 索引到虚拟地址 va 对应的 pml4e 的表项
		// pml4e_t *pml4_entry: pdpe表项的物理地址
		pml4e_t *pml4_entry = &pml4e[PML4(va)];
		pdpe_t *pdp_entry = NULL;
		// 对应的4级页表指针页(PDPE)可能不存在
		if (*pml4_entry == 0)
		{
			// 存在，使用 page_alloc() 分配一个新的 PDPE 页
			if (create)
			{
				struct PageInfo *pp = page_alloc(0);
				// 如果分配失败，返回 NULL
				if (!pp)
					return NULL;
				// 增加新物理页的引用次数并将其清空，根据返回参数 pdpe_t pointer 调用 pdpe_walk()
				pp->pp_ref++;
				// pdp_entry 指向 PDPE 页的虚拟地址
				pdp_entry = (pdpe_t *)page2kva(pp);
				memset(pdp_entry, 0, PGSIZE);

				// 修改 pml4e表中 PDPE 页项的权限
				// pml4_entry 的值为 PDPE 页项的物理地址
				*pml4_entry = (pml4e_t)PADDR(pdp_entry);
				*pml4_entry |= PTE_USER;

				// pdpe_walk() 将获取页表项指针 pt_entry(pte)
				pte_t *pt_entry = pdpe_walk(pdp_entry, va, create);
				// pdpe_walk() 返回 NULL，为 pt_entry 分配的物理页需要被释放
				if (!pt_entry)
				{
					page_decref(pp);
					*pml4_entry = 0;
					return NULL;
				}
				// 返回翻译的页表页项指针 pte pointer
				return pt_entry;
			}
			// 不存在，并且 create == false，返回 NULL
			else
			{
				return NULL;
			}
		}
		// 4级页表指针页(PDPE)存在，调用 pdpe_walk() 获取对应的页表页项并返回
		else
		{
			pdp_entry = (pdpe_t *)KADDR(PTE_ADDR(*pml4_entry));
			pte_t *pt_entry = pdpe_walk(pdp_entry, va, create);
			return pt_entry;
		}
	}
	else
		panic("'page_free_list' is a null pointer!");
}

/**
 * 根据参数 pdpe(pdpe页项)，pdpe_walk() 返回指向页表页项(pte)的指针
 * 
 * 函数的编程逻辑与 pml4e_walk() 类似
 */
pte_t *
pdpe_walk(pdpe_t *pdpe, const void *va, int create)
{
	// pdpe[PDPE(va)]: 索引到虚拟地址 va 对应的 pml4e 的表项
	// pdpe_t *pdp_entry: 4级页表页项的物理地址
	pdpe_t *pdp_entry = &pdpe[PDPE(va)];
	pde_t *pd_entry = NULL;
	// 对应的4级页表指针页(PDPE)可能不存在
	if (*pdp_entry == 0)
	{
		// 存在，使用 page_alloc() 分配一个新的4级页表页
		if (create)
		{
			struct PageInfo *pp = page_alloc(0);
			// 如果分配失败，返回 NULL
			if (!pp)
				return NULL;
			// 增加新物理页的引用次数并将其清空，根据返回参数 pde_t pointer 调用 pgdir_walk()
			pp->pp_ref++;
			pd_entry = (pde_t *)page2kva(pp);
			memset(pd_entry, 0, PGSIZE);

			// 修改 pdpe 表中4级页表页项的权限
			// pdp_entry 的值为4级页表页项的物理地址
			*pdp_entry = (pdpe_t)PADDR(pd_entry);
			*pdp_entry |= PTE_USER;

			// pgdir_walk() 将获取页表项指针 pt_entry(pte)
			pte_t *pt_entry = pgdir_walk(pd_entry, va, create);
			if (!pt_entry)
			{
				page_decref(pp);
				*pdp_entry = 0;
				return NULL;
			}
			// 返回翻译的页表页项指针 pte pointer
			return pt_entry;
		}
		// 不存在，并且 create == false，返回 NULL
		else
		{
			return NULL;
		}
	}
	// 4级页表页(PDE)存在，调用 pgdir_walk() 获取对应的页表页项并返回
	else
	{
		pd_entry = (pde_t *)KADDR(PTE_ADDR(*pdp_entry));
		pte_t *pt_entry = pgdir_walk(pd_entry, va, create);
		return pt_entry;
	}
}

/**
 * 根据参数 pgdir(4级页表页项)，pgdir_walk() 返回指向页表页项(pte)的指针
 * 编程逻辑与 pml4e_walk() 和 pdpe_walk() 相同.
 */
pte_t *
pgdir_walk(pde_t *pgdir, const void *va, int create)
{
	// pgdir[PDX(va)]: 索引到虚拟地址 va 对应的4级页表的表项
	// pde_t *pd_entry: 4级页表表项的物理地址
	pde_t *pd_entry = &pgdir[PDX(va)];
	pte_t *pt_entry = NULL;
	// 对应的4级页表页(PDE)可能不存在
	if (*pd_entry == 0)
	{
		// 存在，使用 page_alloc() 分配一个新的 PDE 页
		if (create)
		{
			struct PageInfo *pp = page_alloc(0);
			// 如果分配失败，返回 NULL
			if (!pp)
				return NULL;
			// 增加新物理页的引用次数并将其清空，根据返回参数 pdpe_t pointer 调用 pdpe_walk()
			pp->pp_ref++;
			memset(page2kva(pp), 0, PGSIZE);
			// 修改4级页表表中 PDE 页项的权限
			// pd_entry 的值为 PDE 页项的物理地址
			*pd_entry = page2pa(pp);
			*pd_entry |= PTE_USER;
		}
		// 不存在，并且 create == false，返回 NULL
		else
			return NULL;
	}
	// 4级页表页(PDE)存在，获取对应的4级页表项并返回
	pt_entry = (pte_t *)KADDR(PTE_ADDR(*pd_entry));
	pt_entry = &pt_entry[PTX(va)];
	return pt_entry;
}

/**
 * 将虚拟地址空间 [va, va+size) 映射到位于 pml4e 页表中的物理地址空间 [pa, pa+size)
 * 大小是 PGSIZE 的倍数，对表项使用权限位 perm|PTE_P
 * boot_map_region() 仅用于设置 UTOP 之上的"内核静态"页映射，因此*不*应该更改映射页上的 pp_ref 字段
 */
static void
boot_map_region(pml4e_t *pml4e, uintptr_t la, size_t size, physaddr_t pa, int perm)
{
	size_t i;
	pte_t *pt_entry;
	// 此循环已实现 ROUNDUP(size, PGSIZE) 的效果
	for (i = 0; i < size; i += PGSIZE)
	{
		// 获取线性地址 (la+i) 对应的页表项 PTE 的地址，无则分配清零的页表页
		pt_entry = pml4e_walk(pml4e, (void *)(la + i), ALLOC_ZERO);
		if (pt_entry)
		{
			// 为了设置参数 la 对应的页表项 PTE，并授予的权限，清空权限位(低12-bit)
			*pt_entry = PTE_ADDR(pa + i);
			// (pa+i) 是页对齐的(4096=2^12)，所以低12位为空，与 PTE_P 和 perm 或运算设置权限
			*pt_entry |= perm | PTE_P;
		}
		// pte 为空，说明内存越界了，映射区间出错
		else
		{
			panic("boot_map_region(): out of memory\n");
		}
	}
}

/**
 * 实现页式内存管理最重要的一个函数，建立页表页的映射及权限
 * 将物理页'pp'映射到虚拟地址'va', 页表项的权限(低12位)设置为'perm|PTE_P'.
 * 实现：
 * - 如果已经有一个映射到'va'的物理页帧，则该物理页帧会被 page_remove() 移除
 * - 根据需要分配一个页表并插入到'pml4e through pdpe through pgdir'中
 * - 如果插入成功，则 pp->pp_ref 会递增
 * - 如果页曾出现在'va'中，则必须调用 tlb_invalidate() 使 TLB 无效，防止页表不对应
 * 分类讨论:
 * 1. pml4e 上没有映射虚拟地址 va 对应的页表项PPN(物理页)，那么这时直接修改相应的二级页表表项即可
 * 2. 如果已经挂载了物理页，且物理页和当前分配的物理页不一样，那么就调用 page_remove(dir,va) 卸下原来的物理页，再挂载新分配的物理页
 * 3. 如果已经挂载了物理页，而且已挂载物理页和当前分配的物理页是同样的，这种情况非常普遍，
 * 就是当内核要修改一个物理页的访问权限时，它会将同一个物理页重新插入一次，传入不同的perm参数，即完成了权限修改。
 */
int page_insert(pml4e_t *pml4e, struct PageInfo *pp, void *va, int perm)
{
	// 通过4级页式地址转换机制 pml4e_walk()，获取虚拟地址 va 对应的页表项 PTE 地址，
	// 如果 va 对应的页表还没有分配，则分配一个空的物理页作为页表
	pte_t *page_entry = pml4e_walk(pml4e, va, ALLOC_ZERO);
	// 分配页表项成功
	if (page_entry)
	{
		// 提前增加pp_ref引用次数，避免 pp 在插入page_free_list之前被释放的极端情况
		pp->pp_ref += 1;

		// 当前虚拟地址va已经被映射过，需要先释放
		// 无论是否和当前分配的物理页帧相同，最终都要修改perm权限设置，所以统一插入新的页表项
		if (*page_entry & PTE_P)
		{
			// 调用 page_remove 中删除页表中对应的页表项(取消映射)
			page_remove(pml4e, va);
		}
		*page_entry = page2pa(pp);
		*page_entry |= perm | PTE_P;
		return 0;
	}
	// 分配页表项失败
	return -E_NO_MEM;
}

/** 
 * 在页式地址转换机制中查找线性地址va所对应的物理页
 * - 如果找到，返回该物理页对应的 PageInfo 结构地址，并将对应的页表项的地址放到 pte_store
 * - 如果还没有物理页被映射到va，那就返回NULL（包括4级页表表项的PTE_P=0 / 页表表项的PTE_P=0两种情况）
 * page_lookup 参数
 * pgdir: 4级页表地址, va: 虚拟地址,
 * pte_store: (pte_t*)指针的地址，方便修改指针与swap同理，接收二级页表项地址
 * 应用于 page_remove，将pte设置为0，令二级页表该项无法索引到物理页帧(物理地址)
 * 
 * 利用了页式地址转换机制，所以可以调用函数 pml4e_walk() 获取页表项虚拟地址(pte*)，
 * 用 pte_store 指向页表项的虚拟地址，然后返回所找到的物理页帧PageInfo结构的虚拟地址
 */
struct PageInfo *
page_lookup(pml4e_t *pml4e, void *va, pte_t **pte_store)
{
	// 获取给定虚拟地址 va 对应页表项的虚拟地址，将create置为0，如果对应的页表不存在，不再新分配
	pte_t *pt_entry = pml4e_walk(pml4e, va, 0);
	if (pt_entry)
	{
		// 	将 pte_store 指向页表项的虚拟地址 pte
		if (pte_store)
			*pte_store = pt_entry;
		// 由 pt 接收给定虚拟地址 va 对应页表项的PPN索引(12-bit 清除权限设置)
		physaddr_t pt = PTE_ADDR(*pt_entry);
		// 返回物理页帧结构地址(虚拟地址)
		return pa2page(pt);
	}
	// 对应的页表项不存在/无效，返回 NULL
	return NULL;
}

/**
 * page_remove 参数
 * pgdir: 4级页表地址, va:虚拟地址
 * 从 Page Table 中删除一个 page frame 映射(页表项)
 * 实际是取消物理地址映射的虚拟地址，将映射页表中对应的项清零即可。
 * 如果没有对应的虚拟地址就什么也不做。
 * 
 * 具体做法如下：
 * 1.找到va虚拟地址对应的物理页帧PageInfo结构的虚拟地址（调用page_lookup）
 * 2.减少物理页帧PageInfo结构的引用数 / 物理页帧被释放（调用page_decref）
 * 3.虚拟地址 va 对应的页表项 PTE 应该被设置为0（如果存在 PTE）
 * 4.失效化 TLB 缓存，重新加载 TLB 的4级页表，否则数据不对应（调用tlb_invalidate）
 */
void page_remove(pml4e_t *pml4e, void *va)
{
	pte_t *pt_entry = NULL;
	// pp 获取线性地址 va 对应的物理页帧PageInfo的地址，pt_entry 指向页表项的虚拟地址
	struct PageInfo *pp = page_lookup(pml4e, va, &pt_entry);
	// 只有当 va 映射到物理页，才需要取消映射，否则什么也不做
	if (pp)
	{
		// 将pp->pp_ref减1，如果pp->pp_ref为0，需要释放该PageInfo结构（将其放入page_free_list链表中）
		page_decref(pp);
		// 将页表项 PTE 对应的 PPN 设为0，令二级页表该项无法索引到物理页帧
		*pt_entry = 0;
		// 失效化 TLB 缓存，重新加载 TLB 的 pml4，否则数据不对应
		tlb_invalidate(pml4e, va);
	}
}

/**
 * 使 TLB 项无效，仅当正在修改的页表是CPU当前处理的页表时才使用
 */
void tlb_invalidate(pml4e_t *pml4e, void *va)
{
	// 当正在修改当前pml4e地址空间时才刷新TLB项.
	assert(pml4e != NULL);
	if (!curenv || curenv->env_pml4e == pml4e)
		invlpg(va);
}

/**
 * 可能多次调用 mmio_map_region()，每一次根据 pa 和 size，会将[pa,pa+size)映射到[base,base+size)，返回保留区域的基址
 * 若映射地址超出 MMIOLIM(0x8003c00000) 则越界；基址大小*不*一定是 PGSIZE 的倍数
 * 
 * MMIO: Memory-mapped I/O(内存映射 I/O)
 * 在 MMIO 部分物理内存固定到了某些 I/O 设备的寄存器，所以可以使用与访问内存相同的 load/store 指令来访问设备寄存器
 */
void *
mmio_map_region(physaddr_t pa, size_t size)
{
	/**
	 * base 代表下一个区域基址(从哪里开始)
	 * 最初，base 为 MMIO 区域的基址
	 * 因为是静态变量，所以它的值会在调用 mmio_map_region() 后保留(就像boot_alloc()中的nextfree一样，保持更新)
	 */
	static uintptr_t base = MMIOBASE;

	// 确保参数 newsize 向上取整为 PGSIZE 的倍数
	size_t newsize = ROUNDUP(size, PGSIZE);
	// 判断准备保留的区域是否会溢出 MMIOLIM 虚拟地址空间(如果是，则直接调用panic)
	if (base + newsize >= MMIOLIM)
	{
		panic("mmio_map_region: out of range\n");
	}
	// 参考: IA32卷3A的10.5节 (section 10.5 of IA32 volume 3A)
	// 因为[MMIOBASE, MMIOLIM]是设备内存，而不是常规DRAM，所以必须禁用CPU对该内存的缓存(缓存设备信息不安全)
	// 幸运的是，页表硬件为控制位提供了 PTE_PCD(禁用缓存), PTE_PWT(直写) 创建映射
	int perm = PTE_PCD | PTE_PWT | PTE_W | PTE_P;
	boot_map_region(boot_pml4e, base, newsize, pa, perm);
	uintptr_t len = base;
	// 更新 base 基址
	base = base + newsize;
	// 返回预留区域的基址
	return (void *)len;
}

// user_mem_check_addr 静态变量一般设置为第一个错误的虚拟地址
static uintptr_t user_mem_check_addr;

/**
 * 内存保护
 * 作用: 检测用户环境是否有权限访问线性地址区域[va, va+len)
 * 在 kern/syscall.c 中的系统调用函数中调用，检查内存访问权限
 * 
 * 检查内存[va, va+len]是否允许一个环境以权限'perm|PTE_P'访问
 * 如果:
 * (1)该虚拟地址在 ULIM 下面
 * (2)有权限访问该页表页项(物理页)权限
 * 用户环境才可以访问该虚拟地址
 * 
 * 如果有错误，将 user_mem_check_addr 变量设置为第一个错误的虚拟地址
 * 如果用户程序可以访问该范围的地址，则返回0，否则返回-E_FAULT
 */
int user_mem_check(struct Env *env, const void *va, size_t len, int perm)
{

	// 无法限制参数va, len页对齐，且需要存储第一个访问出错的地址，va 所在的物理页需要单独处理一下，不能直接对齐
	uintptr_t start = ROUNDDOWN((uintptr_t)va, PGSIZE);
	uintptr_t end = ROUNDUP((uintptr_t)va + len, PGSIZE);
	// 指向环境需要访问的页表页项(物理页)
	pte_t *page;
	// va对应的物理页存在才能访问
	perm |= PTE_P;
	while (start < end)
	{
		struct PageInfo *p = page_lookup(env->env_pml4e, (void *)start, &page);
		// 物理页不存在 / 访问物理页的权限不足 / 当前虚拟地址 >= ULIM
		if (!p || (*page & perm) != perm || start >= ULIM)
		{
			// 非法访问
			// 将 user_mem_check_addr 变量设置为第一个错误的虚拟地址
			user_mem_check_addr = (uintptr_t)va;
			user_mem_check_addr = (user_mem_check_addr > start) ? user_mem_check_addr : start;
			return -E_FAULT;
		}
		start += PGSIZE;
	}
	return 0;
}

/**
 * 检查是否允许环境 env 以 perm|PTE_U|PTE_P 权限访问内存范围[va, va+len]
 * 如果可以，那么函数简单返回
 * 如果不能，则 env 被销毁，如果env是当前环境，则此函数不返回
 */
void user_mem_assert(struct Env *env, const void *va, size_t len, int perm)
{
	if (user_mem_check(env, va, len, perm | PTE_U) < 0)
	{
		cprintf("[%08x] user_mem_check assertion failure for va %08x\n",
				env->env_id, user_mem_check_addr);
		// 不返回
		env_destroy(env);
	}
}

// --------------------------------------------------------------
// Checking functions.
// --------------------------------------------------------------

//
// Check that the pages on the page_free_list are reasonable.
//

static void
check_page_free_list(bool only_low_memory)
{
	struct PageInfo *pp;
	unsigned pdx_limit = only_low_memory ? 1 : NPDENTRIES;
	uint64_t nfree_basemem = 0, nfree_extmem = 0;
	char *first_free_page;

	if (!page_free_list)
		panic("'page_free_list' is a null pointer!");

	// 这段代码的作用就是调整page_free_list链表的顺序，将代表低地址的PageInfo结构放到链表的表头处，这样的话，每次分配物理地址时都是从低地址开始。
	if (only_low_memory)
	{
		// Move pages with lower addresses first in the free
		// list, since entry_pgdir does not map all pages.
		struct PageInfo *pp1, *pp2;
		struct PageInfo **tp[2] = {&pp1, &pp2};
		// 执行该for循环后，pp1指向（0~4M）中地址最大的那个页的PageInfo结构。
		// pp2指向所有页中地址最大的那个PageInfo结构
		for (pp = page_free_list; pp; pp = pp->pp_link)
		{
			int pagetype = PDX(page2pa(pp)) >= pdx_limit;
			*tp[pagetype] = pp;
			tp[pagetype] = &pp->pp_link;
		}
		// 执行for循环后，pp1指向（0~4M）中地址最大的那个页的PageInfo结构
		// pp2指向所有页中地址最大的那个PageInfo结构
		*tp[1] = 0;
		*tp[0] = pp2;
		page_free_list = pp1;
	}

	// if there's a page that shouldn't be on the free list,
	// try to make sure it eventually causes trouble.
	for (pp = page_free_list; pp; pp = pp->pp_link)
		if (PDX(page2pa(pp)) < pdx_limit)
			memset(page2kva(pp), 0x97, 128);

	first_free_page = (char *)boot_alloc(0);
	for (pp = page_free_list; pp; pp = pp->pp_link)
	{
		// check that we didn't corrupt the free list itself
		assert(pp >= pages);
		assert(pp < pages + npages);
		assert(((char *)pp - (char *)pages) % sizeof(*pp) == 0);

		// check a few pages that shouldn't be on the free list
		assert(page2pa(pp) != 0);
		assert(page2pa(pp) != IOPHYSMEM);
		assert(page2pa(pp) != EXTPHYSMEM - PGSIZE);
		assert(page2pa(pp) != EXTPHYSMEM);
		assert(page2pa(pp) < EXTPHYSMEM || (char *)page2kva(pp) >= first_free_page);
		// (new test for lab 4)
		assert(page2pa(pp) != MPENTRY_PADDR);

		if (page2pa(pp) < EXTPHYSMEM)
			++nfree_basemem;
		else
			++nfree_extmem;
	}

	assert(nfree_extmem > 0);
}

//
// Check the physical page allocator (page_alloc(), page_free(),
// and page_init()).
//
static void
check_page_alloc(void)
{
	struct PageInfo *pp, *pp0, *pp1, *pp2;
	int nfree;
	struct PageInfo *fl;
	char *c;
	int i;

	// if there's a page that shouldn't be on
	// the free list, try to make sure it
	// eventually causes trouble.
	for (pp0 = page_free_list, nfree = 0; pp0; pp0 = pp0->pp_link)
	{
		memset(page2kva(pp0), 0x97, PGSIZE);
	}

	for (pp0 = page_free_list, nfree = 0; pp0; pp0 = pp0->pp_link)
	{
		// check that we didn't corrupt the free list itself
		assert(pp0 >= pages);
		assert(pp0 < pages + npages);

		// check a few pages that shouldn't be on the free list
		assert(page2pa(pp0) != 0);
		assert(page2pa(pp0) != IOPHYSMEM);
		assert(page2pa(pp0) != EXTPHYSMEM - PGSIZE);
		assert(page2pa(pp0) != EXTPHYSMEM);
	}
	// should be able to allocate three pages
	pp0 = pp1 = pp2 = 0;
	assert((pp0 = page_alloc(0)));
	assert((pp1 = page_alloc(0)));
	assert((pp2 = page_alloc(0)));
	assert(pp0);
	assert(pp1 && pp1 != pp0);
	assert(pp2 && pp2 != pp1 && pp2 != pp0);
	assert(page2pa(pp0) < npages * PGSIZE);
	assert(page2pa(pp1) < npages * PGSIZE);
	assert(page2pa(pp2) < npages * PGSIZE);

	// temporarily steal the rest of the free pages
	fl = page_free_list;
	page_free_list = 0;

	// should be no free memory
	assert(!page_alloc(0));

	// free and re-allocate?
	page_free(pp0);
	page_free(pp1);
	page_free(pp2);
	pp0 = pp1 = pp2 = 0;
	assert((pp0 = page_alloc(0)));
	assert((pp1 = page_alloc(0)));
	assert((pp2 = page_alloc(0)));
	assert(pp0);
	assert(pp1 && pp1 != pp0);
	assert(pp2 && pp2 != pp1 && pp2 != pp0);
	assert(!page_alloc(0));

	// test flags
	memset(page2kva(pp0), 1, PGSIZE);
	page_free(pp0);
	assert((pp = page_alloc(ALLOC_ZERO)));
	assert(pp && pp0 == pp);
	c = page2kva(pp);
	for (i = 0; i < PGSIZE; i++)
		assert(c[i] == 0);

	// give free list back
	page_free_list = fl;

	// free the pages we took
	page_free(pp0);
	page_free(pp1);
	page_free(pp2);

	cprintf("check_page_alloc() succeeded!\n");
}

//
// Checks that the kernel part of virtual address space
// has been setup roughly correctly (by x64_vm_init()).
//
// This function doesn't test every corner case,
// but it is a pretty good sanity check.
//

static void
check_boot_pml4e(pml4e_t *pml4e)
{
	uint64_t i, n;

	pml4e = boot_pml4e;

	// check pages array
	n = ROUNDUP(npages * sizeof(struct PageInfo), PGSIZE);
	for (i = 0; i < n; i += PGSIZE)
	{
		//cprintf("%x %x %x\n",i,check_va2pa(pml4e, UPAGES + i), PADDR(pages) + i);
		assert(check_va2pa(pml4e, UPAGES + i) == PADDR(pages) + i);
	}

	// check envs array (new test for lab 3)
	n = ROUNDUP(NENV * sizeof(struct Env), PGSIZE);
	for (i = 0; i < n; i += PGSIZE)
		assert(check_va2pa(pml4e, UENVS + i) == PADDR(envs) + i);

	// check phys mem
	for (i = 0; i < npages * PGSIZE; i += PGSIZE)
		assert(check_va2pa(pml4e, KERNBASE + i) == i);

	// check kernel stack
	// (updated in lab 4 to check per-CPU kernel stacks)
	for (n = 0; n < NCPU; n++)
	{
		uint64_t base = KSTACKTOP - (KSTKSIZE + KSTKGAP) * (n + 1);
		for (i = 0; i < KSTKSIZE; i += PGSIZE)
			assert(check_va2pa(pml4e, base + KSTKGAP + i) == PADDR(percpu_kstacks[n]) + i);
		for (i = 0; i < KSTKGAP; i += PGSIZE)
			assert(check_va2pa(pml4e, base + i) == ~0);
	}

	pdpe_t *pdpe = KADDR(PTE_ADDR(boot_pml4e[1]));
	pde_t *pgdir = KADDR(PTE_ADDR(pdpe[0]));
	// check PDE permissions
	for (i = 0; i < NPDENTRIES; i++)
	{
		switch (i)
		{
		//case PDX(UVPT):
		case PDX(KSTACKTOP - 1):
		case PDX(UPAGES):
		case PDX(UENVS):
			assert(pgdir[i] & PTE_P);
			break;
		default:
			if (i >= PDX(KERNBASE))
			{
				if (pgdir[i] & PTE_P)
					assert(pgdir[i] & PTE_W);
				else
					assert(pgdir[i] == 0);
			}
			break;
		}
	}
	cprintf("check_boot_pml4e() succeeded!\n");
}

// This function returns the physical address of the page containing 'va',
// defined by the 'pml4e'.  The hardware normally performs
// this functionality for us!  We define our own version to help check
// the check_boot_pml4e() function; it shouldn't be used elsewhere.

static physaddr_t
check_va2pa(pml4e_t *pml4e, uintptr_t va)
{
	pte_t *pte;
	pdpe_t *pdpe;
	pde_t *pde;
	// cprintf("%x", va);
	pml4e = &pml4e[PML4(va)];
	// cprintf(" %x %x " , PML4(va), *pml4e);
	if (!(*pml4e & PTE_P))
		return ~0;
	pdpe = (pdpe_t *)KADDR(PTE_ADDR(*pml4e));
	// cprintf(" %x %x " , pdpe, *pdpe);
	if (!(pdpe[PDPE(va)] & PTE_P))
		return ~0;
	pde = (pde_t *)KADDR(PTE_ADDR(pdpe[PDPE(va)]));
	// cprintf(" %x %x " , pde, *pde);
	pde = &pde[PDX(va)];
	if (!(*pde & PTE_P))
		return ~0;
	pte = (pte_t *)KADDR(PTE_ADDR(*pde));
	// cprintf(" %x %x " , pte, *pte);
	if (!(pte[PTX(va)] & PTE_P))
		return ~0;
	// cprintf(" %x %x\n" , PTX(va),  PTE_ADDR(pte[PTX(va)]));
	return PTE_ADDR(pte[PTX(va)]);
}

// check page_insert, page_remove, &c
static void
page_check(void)
{
	struct PageInfo *pp0, *pp1, *pp2, *pp3, *pp4, *pp5;
	struct PageInfo *fl;
	pte_t *ptep, *ptep1;
	pdpe_t *pdpe;
	pde_t *pde;
	void *va;
	int i;
	uintptr_t mm1, mm2;
	pp0 = pp1 = pp2 = pp3 = pp4 = pp5 = 0;
	assert(pp0 = page_alloc(0));
	assert(pp1 = page_alloc(0));
	assert(pp2 = page_alloc(0));
	assert(pp3 = page_alloc(0));
	assert(pp4 = page_alloc(0));
	assert(pp5 = page_alloc(0));

	assert(pp0);
	assert(pp1 && pp1 != pp0);
	assert(pp2 && pp2 != pp1 && pp2 != pp0);
	assert(pp3 && pp3 != pp2 && pp3 != pp1 && pp3 != pp0);
	assert(pp4 && pp4 != pp3 && pp4 != pp2 && pp4 != pp1 && pp4 != pp0);
	assert(pp5 && pp5 != pp4 && pp5 != pp3 && pp5 != pp2 && pp5 != pp1 && pp5 != pp0);

	// temporarily steal the rest of the free pages
	fl = page_free_list;
	page_free_list = NULL;

	// should be no free memory
	assert(!page_alloc(0));

	// there is no page allocated at address 0
	assert(page_lookup(boot_pml4e, (void *)0x0, &ptep) == NULL);

	// there is no free memory, so we can't allocate a page table
	assert(page_insert(boot_pml4e, pp1, 0x0, 0) < 0);

	// free pp0 and try again: pp0 should be used for page table
	page_free(pp0);
	assert(page_insert(boot_pml4e, pp1, 0x0, 0) < 0);
	page_free(pp2);
	page_free(pp3);

	//cprintf("pp0 ref count = %d\n",pp0->pp_ref);
	//cprintf("pp2 ref count = %d\n",pp2->pp_ref);
	assert(page_insert(boot_pml4e, pp1, 0x0, 0) == 0);
	assert((PTE_ADDR(boot_pml4e[0]) == page2pa(pp0) || PTE_ADDR(boot_pml4e[0]) == page2pa(pp2) || PTE_ADDR(boot_pml4e[0]) == page2pa(pp3)));
	assert(check_va2pa(boot_pml4e, 0x0) == page2pa(pp1));
	assert(pp1->pp_ref == 1);
	assert(pp0->pp_ref == 1);
	assert(pp2->pp_ref == 1);
	//should be able to map pp3 at PGSIZE because pp0 is already allocated for page table
	assert(page_insert(boot_pml4e, pp3, (void *)PGSIZE, 0) == 0);
	assert(check_va2pa(boot_pml4e, PGSIZE) == page2pa(pp3));
	assert(pp3->pp_ref == 2);

	// should be no free memory
	assert(!page_alloc(0));

	// should be able to map pp3 at PGSIZE because it's already there
	assert(page_insert(boot_pml4e, pp3, (void *)PGSIZE, 0) == 0);
	assert(check_va2pa(boot_pml4e, PGSIZE) == page2pa(pp3));
	assert(pp3->pp_ref == 2);

	// pp3 should NOT be on the free list
	// could happen in ref counts are handled sloppily in page_insert
	assert(!page_alloc(0));
	// check that pgdir_walk returns a pointer to the pte
	pdpe = KADDR(PTE_ADDR(boot_pml4e[PML4(PGSIZE)]));
	pde = KADDR(PTE_ADDR(pdpe[PDPE(PGSIZE)]));
	ptep = KADDR(PTE_ADDR(pde[PDX(PGSIZE)]));
	assert(pml4e_walk(boot_pml4e, (void *)PGSIZE, 0) == ptep + PTX(PGSIZE));

	// should be able to change permissions too.
	assert(page_insert(boot_pml4e, pp3, (void *)PGSIZE, PTE_U) == 0);
	assert(check_va2pa(boot_pml4e, PGSIZE) == page2pa(pp3));
	assert(pp3->pp_ref == 2);
	assert(*pml4e_walk(boot_pml4e, (void *)PGSIZE, 0) & PTE_U);
	assert(boot_pml4e[0] & PTE_U);

	// should not be able to map at PTSIZE because need free page for page table
	assert(page_insert(boot_pml4e, pp0, (void *)PTSIZE, 0) < 0);

	// insert pp1 at PGSIZE (replacing pp3)
	assert(page_insert(boot_pml4e, pp1, (void *)PGSIZE, 0) == 0);
	assert(!(*pml4e_walk(boot_pml4e, (void *)PGSIZE, 0) & PTE_U));

	// should have pp1 at both 0 and PGSIZE
	assert(check_va2pa(boot_pml4e, 0) == page2pa(pp1));
	assert(check_va2pa(boot_pml4e, PGSIZE) == page2pa(pp1));
	// ... and ref counts should reflect this
	assert(pp1->pp_ref == 2);
	assert(pp3->pp_ref == 1);

	// unmapping pp1 at 0 should keep pp1 at PGSIZE
	page_remove(boot_pml4e, 0x0);
	assert(check_va2pa(boot_pml4e, 0x0) == ~0);
	assert(check_va2pa(boot_pml4e, PGSIZE) == page2pa(pp1));
	assert(pp1->pp_ref == 1);
	assert(pp3->pp_ref == 1);

	// Test re-inserting pp1 at PGSIZE.
	// Thanks to Varun Agrawal for suggesting this test case.
	assert(page_insert(boot_pml4e, pp1, (void *)PGSIZE, 0) == 0);
	assert(pp1->pp_ref);
	assert(pp1->pp_link == NULL);

	// unmapping pp1 at PGSIZE should free it
	page_remove(boot_pml4e, (void *)PGSIZE);
	assert(check_va2pa(boot_pml4e, 0x0) == ~0);
	assert(check_va2pa(boot_pml4e, PGSIZE) == ~0);
	assert(pp1->pp_ref == 0);
	assert(pp3->pp_ref == 1);

#if 0
	// should be able to page_insert to change a page
	// and see the new data immediately.
	memset(page2kva(pp1), 1, PGSIZE);
	memset(page2kva(pp2), 2, PGSIZE);
	page_insert(boot_pgdir, pp1, 0x0, 0);
	assert(pp1->pp_ref == 1);
	assert(*(int*)0 == 0x01010101);
	page_insert(boot_pgdir, pp2, 0x0, 0);
	assert(*(int*)0 == 0x02020202);
	assert(pp2->pp_ref == 1);
	assert(pp1->pp_ref == 0);
	page_remove(boot_pgdir, 0x0);
	assert(pp2->pp_ref == 0);
#endif

	// forcibly take pp3 back
	assert(PTE_ADDR(boot_pml4e[0]) == page2pa(pp3));
	boot_pml4e[0] = 0;
	assert(pp3->pp_ref == 1);
	page_decref(pp3);
	// check pointer arithmetic in pml4e_walk
	page_decref(pp0);
	page_decref(pp2);
	va = (void *)(PGSIZE * 100);
	ptep = pml4e_walk(boot_pml4e, va, 1);
	pdpe = KADDR(PTE_ADDR(boot_pml4e[PML4(va)]));
	pde = KADDR(PTE_ADDR(pdpe[PDPE(va)]));
	ptep1 = KADDR(PTE_ADDR(pde[PDX(va)]));
	assert(ptep == ptep1 + PTX(va));

	// check that new page tables get cleared
	page_decref(pp4);
	memset(page2kva(pp4), 0xFF, PGSIZE);
	pml4e_walk(boot_pml4e, 0x0, 1);
	pdpe = KADDR(PTE_ADDR(boot_pml4e[0]));
	pde = KADDR(PTE_ADDR(pdpe[0]));
	ptep = KADDR(PTE_ADDR(pde[0]));
	for (i = 0; i < NPTENTRIES; i++)
		assert((ptep[i] & PTE_P) == 0);
	boot_pml4e[0] = 0;

	// give free list back
	page_free_list = fl;

	// free the pages we took
	page_decref(pp0);
	page_decref(pp1);
	page_decref(pp2);

	// test mmio_map_region
	mm1 = (uintptr_t)mmio_map_region(0, 4097);
	mm2 = (uintptr_t)mmio_map_region(0, 4096);
	// check that they're in the right region
	assert(mm1 >= MMIOBASE && mm1 + 8096 < MMIOLIM);
	assert(mm2 >= MMIOBASE && mm2 + 8096 < MMIOLIM);
	// check that they're page-aligned
	assert(mm1 % PGSIZE == 0 && mm2 % PGSIZE == 0);
	// check that they don't overlap
	assert(mm1 + 8096 <= mm2);
	// check page mappings

	assert(check_va2pa(boot_pml4e, mm1) == 0);
	assert(check_va2pa(boot_pml4e, mm1 + PGSIZE) == PGSIZE);
	assert(check_va2pa(boot_pml4e, mm2) == 0);
	cprintf("check privilege success %x %x\n", mm2 + PGSIZE, check_va2pa(boot_pml4e, mm2 + PGSIZE));
	assert(check_va2pa(boot_pml4e, mm2 + PGSIZE) == ~0);
	// check permissions
	assert(*pml4e_walk(boot_pml4e, (void *)mm1, 0) & (PTE_W | PTE_PWT | PTE_PCD));
	assert(!(*pml4e_walk(boot_pml4e, (void *)mm1, 0) & PTE_U));
	// clear the mappings
	*pml4e_walk(boot_pml4e, (void *)mm1, 0) = 0;
	*pml4e_walk(boot_pml4e, (void *)mm1 + PGSIZE, 0) = 0;
	*pml4e_walk(boot_pml4e, (void *)mm2, 0) = 0;

	cprintf("check_page() succeeded!\n");
}
