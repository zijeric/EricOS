#ifndef ALVOS_INC_MEMLAYOUT_H
#define ALVOS_INC_MEMLAYOUT_H

#ifndef __ASSEMBLER__
#include "inc/types.h"
#include "inc/mmu.h"
#endif /* not __ASSEMBLER__ */

/*
 * 这个文件包含操作系统中内存管理的定义，与内核态和用户态软件都相关。
 */

// 全局描述符索引(选择子)
#define GD_KT 0x08	 // 内核代码
#define GD_KD 0x10	 // 内核数据
#define GD_UT 0x18	 // 用户代码
#define GD_UD 0x20	 // 用户数据
#define GD_TSS0 0x28 // CPU0的TSS(任务状态段选择子)

/*
 * 64-bit CPU 线性地址空间分为两部分，内核控制 ULIM 之上的地址空间，为内核保留大约256MB[0x80 03c0 0000, 0xff ffff ffff]的虚拟地址空间，
 * 用户环境控制下方部分，约3.72G[0x0, 0x8003c00000]
 *
 * 用户环境将没有对以上 ULIM 内存的任何权限，只有内核能够读写这个内存；
 * [UTOP, ULIM]，内核和用户环境都可以读取但不能写入这个地址范围，此地址范围用于向用户环境公开某些只读内核数据结构；
 * UTOP 下的地址空间供用户环境使用;用户环境将设置访问此内存的权限
 *
 * Virtual memory map:                                Permissions
 *                                                    kernel/user
 * 
 *                     +------------------------------+
 *                     |                              |
 *                     |~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~|
 *                     :              .               :
 *                     :              .               :
 *                     :              .               :
 *                     |~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~|
 *                     |                              |
 *                     +------------------------------+ 0x10080403000
 *                     |           uvpml4e[]          | R-/R-  PTSIZE 
 *                     |            uvpde[]           |
 *                     |            uvpd[]            |
 *     (1 TB) /        |            uvpt[]            |
 *     UVPT -------->  +------------------------------+ 0x10000000000
 *                     |                              |
 *                     |~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~|
 *                     :              .               :
 *                     :              .               :
 *                     :              .               :
 *                     |~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~| RW/--
 *                     |                              | RW/--
 *                     |   Remapped Physical Memory   | RW/--
 *                     |                              | RW/--
 *    KERNBASE, ---->  +------------------------------+ 0x8004000000    --+
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
 *    MMIOLIM ------>  +------------------------------+ 0x8003e00000    --+
 *                     |       Memory-mapped I/O      | RW/--  PTSIZE
 * ULIM, MMIOBASE -->  +------------------------------+ 0x8003c00000
 *                     |           RO PAGES           | R-/R-  25*PTSIZE
 *                     |                              |
 *                     :              .               :
 *                     :              .               : 
 *    UPAGES    ---->  +------------------------------+ 0x8000a00000
 *                     |           RO ENVS            | R-/R-  PTSIZE
 * UTOP,UENVS ------>  +------------------------------+ 0x8000800000
 *                     .                              .
 *                     .                              .
 *                     .                              .
 * UXSTACKTOP ------>  +------------------------------+ 0xef800000
 *                     |     User Exception Stack     | RW/RW  PGSIZE
 *                     +------------------------------+ 0xef7ff000
 *                     |       Empty Memory (*)       | --/--  PGSIZE
 *    USTACKTOP  --->  +------------------------------+ 0xef7fe000
 *                     |      Normal User Stack       | RW/RW  PGSIZE
 *                     +------------------------------+ 0xef7fd000
 *                     |                              |
 *                     |                              |
 *                     ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *                     .                              .
 *                     .                              .
 *                     .                              .
 *                     |~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~|
 *                     |     Program Data & Heap      |
 *    UTEXT -------->  +------------------------------+ 0x00800000  	
 *                     |       Empty Memory (*)       |
 *                     |                              |	       PTSIZE+PGSIZE
 *                     |                              |
 *    PFTEMP ------->  +------------------------------+
 *                     |       Empty Memory (*)       |        PTSIZE-PGSIZE
 *                     |                              |
 *    UTEMP -------->  +------------------------------+ 0x00400000      --+
 *                     |       Empty Memory (*)       |                   |
 *                     | - - - - - - - - - - - - - - -|        PTSIZE     |
 *                     |  User STAB Data (optional)   |                2* PTSIZE
 *    USTABDATA ---->  +------------------------------+ 0x00200000        |
 *                     |       Empty Memory (*)       |        PTSIZE     |
 *    0 ------------>  +------------------------------+                 --+
 *
 * (*) 注意: AlvOS 内核确保"Invalid Memory"永不映射(作保护页).
 *     "Empty Memory"有必要用才会映射
 * 	   例外: AlvOS 用户程序*有权*根据需要在 UTEMP 临时映射物理页.
 *     其他都是内核正常运行所必须映射的虚拟内存.
 */

// 内核所有的物理内存必须映射到 KERNBASE 才能执行
#define KERNBASE 0x8004000000

// 在 IOPHYSMEM (640K) 有一个 384K 的 I/O 区域.
// 在内核，IOPHYSMEM 可以通过 [KERNBASE + IOPHYSMEM] 访问.
#define IOPHYSMEM 0x0A0000
// I/O 区域结束于物理地址 EXTPHYSMEM
#define EXTPHYSMEM 0x100000

// 内核栈虚拟地址.
#define KSTACKTOP KERNBASE
// 内核栈大小
#define KSTKSIZE (16 * PGSIZE)
// 内核栈保护页大小
#define KSTKGAP (8 * PGSIZE)

// 内存映射 I/O.
#define MMIOLIM (KSTACKTOP - PTSIZE)
#define MMIOBASE (MMIOLIM - PTSIZE)

#define ULIM (MMIOBASE)

/*
 * [ULIM, UTOP]的所有内容都是用户只读映射！
 * 内容是在 env 分配时映射到的全局物理页.
 */
// 用户只读虚拟页表，包含512GB个页表项的数组(addr 1TB)
// UVPT: 2<<39 = 0x10000000000, uvpd: 2<<39|2<<30 = 0x10080000000
// uvpde: 2<<39|2<<30|2<<21 = 0x10080400000
// uvpml4e: 2<<39|2<<30|2<<21|2<<12 = 0x10080402000
#define UVPT    0x10000000000
// pageInfo 结构 pages[] 的只读映射副本
#define UPAGES		(ULIM - 25 * PTSIZE)
// 全局 env 结构 envs[] 的只读映射副本
#define UENVS (UPAGES - PTSIZE)

/*
 * 用户虚拟内存的顶部，用户只可以操作[0,UTOP-1]的虚拟内存
 */
// 用户可访问的虚拟内存顶部
#define UTOP UENVS

// 用户异常栈的顶部(PGSIZE大小)
#define UXSTACKTOP 0xef800000
// 为了防止栈溢出异常，下一页(保护页)无效
#define USTACKTOP (UXSTACKTOP - 2 * PGSIZE)

// 用户程序一般从 UTEXT(0x00800000) 开始
#define UTEXT (4 * PTSIZE)


// 用于临时页映射，类型为'void *'，方便转换为任何类型
#define UTEMP ((void *)((int)(2 * PTSIZE)))
// 用于用户页故障处理程序的临时页映射（不应与其他临时页映射冲突）
#define PFTEMP (UTEMP + PTSIZE - PGSIZE)
// 用户态 STABS 数据结构的虚拟地址
#define USTABDATA (PTSIZE)

// 非引导 CPUs(APs) 的引导代码的物理地址，可以是 640KB(0xA0000) 以下的未使用、页对齐的物理地址
#define MPENTRY_PADDR 0x7000

#ifndef __ASSEMBLER__

typedef uint64_t pml4e_t;
typedef uint64_t pdpe_t;
typedef uint64_t pte_t;
typedef uint64_t pde_t;

// 由用户态的环境使用
#if ALVOS_USER
/**
 * 页目录项对应的虚拟地址范围[UVPT, UVPT + PTSIZE)指向页目录本身
 * 因此，页目录既是页表，也是页目录
 * 将页目录作为页表处理的有两个好处：
 * 1.所有 pte 都可以通过虚拟地址 UVPT(lib/entry.S) 的"虚拟页表"访问
 * PGNUM(页码)为 N 的 PTE 存储在 uvpt[N] 中
 * 2.当前页目录的内容将始终在虚拟地址(UVPT + (UVPT >> PGSHIFT))中可用
 * uvpd在lib/entry.S中设置
 */
extern volatile pte_t uvpt[];	 // VA of "virtual page table"
extern volatile pde_t uvpd[];	 // VA of current page directory
extern volatile pde_t uvpde[];	 // VA of current page directory pointer
extern volatile pde_t uvpml4e[]; // VA of current page map level 4
#endif

/**
 * 页结构，映射到 UPAGES
 * 内核: 读/写，用户程序: 只读
 * 
 * 每个 PageInfo 结构体存储一个页的元数据，页和 struct PageInfo 之间的一对一的对应关系
 * 可以page2pa()将结构 (PageInfo*) 映射到相应的地址
 * 将物理内存切成每个元素 PGSIZE 大小的数组，并用 struct PageInfo 存储当前物理页引用次数
 * 同时，struct PageInfo是链表结构，所以可以通过 free_page_list 快速地找到可用空闲页
 */
struct PageInfo
{
	// 指向空闲页列表中的下一个页(结构)，非空闲页的 pp_link 为NULL
	struct PageInfo *pp_link;

	// 该页被引用的次数，即被映射(map)到虚拟地址的数量（通常在页表页项中）
	// 当引用数为 0，即可释放
	// 随着内核实现的深入会将同样的页映射到多个虚拟地址空间，需要在 pp_ref 保持页被引用的次数(当引用计数归0，页不再被使用，可以被释放)
	// 总的来说，引用计数应当等于页在 UTOP 之下出现的次数(UTOP之上的页会在boot阶段被内核分配且永不被释放，所以不需要对其进行引用计数)
	// 引用计数也会被用来追踪指向页目录页的指针数，以及页目录对页表页的引用数，页目录指针页和第4级页表类似
	uint16_t pp_ref;
};

#endif /* !__ASSEMBLER__ */
#endif /* !ALVOS_INC_MEMLAYOUT_H */
