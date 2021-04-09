#ifndef ALVOS_INC_MMU_H
#define ALVOS_INC_MMU_H

/*TODO
 * 这个文件包含x86内存管理单元(MMU)的定义，包括与分页和分段相关的数据结构和常量、%cr0、%cr4、%eflags 寄存器和陷阱相关定义.
 */

/*
 *
 *	Part 1.  分页需要的数据机构和常数.
 *
 */

// 线性地址'la'有五部分结构，PML4、PDPE、PDX、PTX、PGOFF 和 PGNUM 宏将线性地址分解为如下所示:
//
// +-------9--------+-------9--------+--------9-------+--------9-------+----------12---------+
// |Page Map Level 4|Page Directory  | Page Directory |   Page Table   | Offset within Page  |
// |      Index     |  Pointer Index |      Index     |      Index     |                     |
// +----------------+----------------+----------------+--------------------------------------+
// \----PML4(la)----/\--- PDPE(la)---/\--- PDX(la) --/ \--- PTX(la) --/ \---- PGOFF(la) ----/
//  \------------------------------ PGNUM(la) -------------------------/
//
// 通过使用 PGADDR(PML4(la)、PDPE(la)、PDX(la)、PTX(la)、PGOFF(la))
// 构造由 PML4(la)、PDPE(la)、PDX(la)、PTX(la) 和 PGOFF(la) 组成的线性地址 la.(如上图)

// 用于索引到 vpt[], (PML4 + PDPE + PDX + PTX) 右移12位
#define PPN(pa) (((uintptr_t)(pa)) >> PTXSHIFT)
#define VPN(la) PPN(la)	  // 用于索引到 vpt[], 同 PPN
#define PGNUM(la) PPN(la) // 用于索引到 vpt[], 同 PPN

// 用于索引到 vpd[], (PML4 + PDPE + PDX) 右移21位
#define VPD(la) (((uintptr_t)(la)) >> PDXSHIFT)
// 索引到 vpdpe[], (PML4 + PDPE) 右移30位
#define VPDPE(la) (((uintptr_t)(la)) >> PDPESHIFT)
// 索引到 vpml4e[], (PML4) 右移39位
#define VPML4E(la) (((uintptr_t)(la)) >> PML4SHIFT)
// pd索引(page directory index)
#define PDX(la) ((((uintptr_t)(la)) >> PDXSHIFT) & 0x1FF)
// pml4索引(page mapping level 4 index)
#define PML4(la) ((((uintptr_t)(la)) >> PML4SHIFT) & 0x1FF)

// pt索引(page table index)
#define PTX(la) ((((uintptr_t)(la)) >> PTXSHIFT) & 0x1FF)
// pdpe索引(page directory pointer index)
#define PDPE(la) ((((uintptr_t)(la)) >> PDPESHIFT) & 0x1FF)

// 物理页帧的偏移(索引)
#define PGOFF(la) (((uintptr_t)(la)) & 0xFFF)

// 由索引和偏移构造线性地址
#define PGADDR(m, p, d, t, o) ((void *)((m) << PML4SHIFT | (p) << PDPESHIFT | (d) << PDXSHIFT | (t) << PTXSHIFT | (o)))

// 页表页、页目录、页目录指针页、第4级页表页的常数.
#define NPMLENTRIES 512 // 每个4级页表的页表页项(pml4e)数目为512
#define NPDPENTRIES 512 // 每个页目录指针页表的页表项(pdpe)数目为512
#define NPDENTRIES 512	// 每个页目录的页目录项(pde)数目为512
#define NPTENTRIES 512	// 每个页表页的页表页项(pte)数目为512

#define PGSIZE 4096 // 一个物理页映射的字节数，即页大小为 4KB
#define PGSHIFT 12	// log2(PGSIZE)

#define PTSIZE (PGSIZE * NPTENTRIES) // 页表页项映射的字节(映射的实际物理内存大小)，512 * 4KB = 2MB
#define PTSHIFT 21					 // log2(PTSIZE)

#define PTXSHIFT 12	 // 线性地址中页表页索引(PTX)的偏移
#define PDXSHIFT 21	 // 线性地址中页目录索引(PDX)的偏移
#define PDPESHIFT 30 // 线性地址中页目录指针页索引(PDPE)的偏移
#define PML4SHIFT 39 // 线性地址中4级页表索引(PML4)的偏移

// 4级映射页表页/页目录指针页/页目录表/页表的权限位，在访问对应页时CPU会自动地判断，如果访问违规，会产生异常.
#define PTE_P 0x001	  // Present		存在位
#define PTE_W 0x002	  // Writeable	可写位，同时影响 kernel 和 user
#define PTE_U 0x004	  // User			用户1->(0,1,2,3), 管理员0->(0,1,2)
#define PTE_PWT 0x008 // Write-Through	页级通写位，内存 or 高速缓存
#define PTE_PCD 0x010 // Cache-Disable	页级高速缓存禁止位
#define PTE_A 0x020	  // Accessed		访问位，由CPU设置，1:已访问可换出到外存
#define PTE_D 0x040	  // Dirty		脏页位，针对页表项，CPU写时置为1
#define PTE_PS 0x080  // Page Size	页大小位，0:4KB, 1:4MB
#define PTE_MBZ 0x180 // Bits must be zero 启用2MB物理页该位必须为0

// 内核不使用 PTE_AVAIL 位，硬件也不对其进行解释.
// 供用户环境使用
#define PTE_AVAIL 0xE00

// PTE_SYSCALL 中的标志只能在系统调用中使用.
#define PTE_SYSCALL (PTE_AVAIL | PTE_P | PTE_W | PTE_U)

// 在系统调用中只能使用 PTE_USER 中的标志.
#define PTE_USER (PTE_AVAIL | PTE_P | PTE_W | PTE_U)

// 返回对应级别页表项中的PPN索引，将低12位(0~11) Flags 状态位置为0.
#define PTE_ADDR(pte) ((physaddr_t)(pte) & ~0xFFF)

// 控制寄存器标志位
#define CR0_PE 0x00000001 // 保护模式启动位
#define CR0_MP 0x00000002 // 监控协处理器(Monitor coProcessor)
#define CR0_EM 0x00000004 // Emulation
#define CR0_TS 0x00000008 // Task Switched
#define CR0_ET 0x00000010 // Extension Type
#define CR0_NE 0x00000020 // Numeric Error
#define CR0_WP 0x00010000 // 写保护位
#define CR0_AM 0x00040000 // Alignment Mask
#define CR0_NW 0x20000000 // Not Writethrough
#define CR0_CD 0x40000000 // 禁用 Cache
#define CR0_PG 0x80000000 // 分页位

#define CR4_PCE 0x00000100 // Performance counter enable
#define CR4_MCE 0x00000040 // Machine Check Enable
#define CR4_PSE 0x00000010 // Page Size Extensions
#define CR4_DE 0x00000008  // Debugging Extensions
#define CR4_TSD 0x00000004 // Time Stamp Disable
#define CR4_PVI 0x00000002 // Protected-Mode Virtual Interrupts
#define CR4_VME 0x00000001 // V86 Mode Extensions

// x86_64 的变化
#define CR4_PAE 0x00000020
#define EFER_MSR 0xC0000080
#define EFER_LME 8

// Eflags 寄存器(标志位)
#define FL_CF 0x00000001		// Carry Flag
#define FL_PF 0x00000004		// Parity Flag
#define FL_AF 0x00000010		// Auxiliary carry Flag
#define FL_ZF 0x00000040		// Zero Flag
#define FL_SF 0x00000080		// Sign Flag
#define FL_TF 0x00000100		// Trap Flag
#define FL_IF 0x00000200		// Interrupt Flag
#define FL_DF 0x00000400		// Direction Flag
#define FL_OF 0x00000800		// Overflow Flag
#define FL_IOPL_MASK 0x00003000 // I/O 特权级位掩码
#define FL_IOPL_0 0x00000000	//   IOPL == 0
#define FL_IOPL_1 0x00001000	//   IOPL == 1
#define FL_IOPL_2 0x00002000	//   IOPL == 2
#define FL_IOPL_3 0x00003000	//   IOPL == 3
#define FL_NT 0x00004000		// Nested Task
#define FL_RF 0x00010000		// Resume Flag
#define FL_VM 0x00020000		// Virtual 8086 mode
#define FL_AC 0x00040000		// Alignment Check
#define FL_VIF 0x00080000		// Virtual Interrupt Flag
#define FL_VIP 0x00100000		// Virtual Interrupt Pending
#define FL_ID 0x00200000		// ID flag

// 页错误的错误码
#define FEC_PR 0x1 // 保护违规引起的页错误
#define FEC_WR 0x2 // 由写引起的页错误
#define FEC_U 0x4  // 在用户态出现页错误

/*
 *
 *	Part 2.  分段需要的数据结构和常数.
 *
 */

#ifdef __ASSEMBLER__

/*
 * Macros to build GDT entries in assembly.
 */
#define SEG_NULL \
	.word 0, 0;  \
	.byte 0, 0, 0, 0
#define SEG(type, base, lim)                        \
	.word(((lim) >> 12) & 0xffff), ((base)&0xffff); \
	.byte(((base) >> 16) & 0xff), (0x90 | (type)),  \
		(0xC0 | (((lim) >> 28) & 0xf)), (((base) >> 24) & 0xff)

#define SEG64(type, base, lim)                      \
	.word(((lim) >> 12) & 0xffff), ((base)&0xffff); \
	.byte(((base) >> 16) & 0xff), (0x90 | (type)),  \
		(0xA0 | (((lim) >> 28) & 0xF)), (((base) >> 24) & 0xff)

#define SEG64USER(type, base, lim)                  \
	.word(((lim) >> 12) & 0xffff), ((base)&0xffff); \
	.byte(((base) >> 16) & 0xff), (0xf0 | (type)),  \
		(0xA0 | (((lim) >> 28) & 0xF)), (((base) >> 24) & 0xff)
#else // not __ASSEMBLER__

#include "inc/types.h"

	// Segment Descriptors
	struct Segdesc
{
	unsigned sd_lim_15_0 : 16;	// Low bits of segment limit
	unsigned sd_base_15_0 : 16; // Low bits of segment base address
	unsigned sd_base_23_16 : 8; // Middle bits of segment base address
	unsigned sd_type : 4;		// Segment type (see STS_ constants)
	unsigned sd_s : 1;			// 0 = system, 1 = application
	unsigned sd_dpl : 2;		// Descriptor Privilege Level
	unsigned sd_p : 1;			// Present
	unsigned sd_lim_19_16 : 4;	// High bits of segment limit
	unsigned sd_avl : 1;		// Unused (available for software use)
	unsigned sd_l : 1;			// Reserved
	unsigned sd_db : 1;			// 0 = 16-bit segment, 1 = 32-bit segment
	unsigned sd_g : 1;			// Granularity: limit scaled by 4K when set
	unsigned sd_base_31_24 : 8; // High bits of segment base address
};
struct SystemSegdesc64
{
	unsigned sd_lim_15_0 : 16;
	unsigned sd_base_15_0 : 16;
	unsigned sd_base_23_16 : 8;
	unsigned sd_type : 4;
	unsigned sd_s : 1;			// 0 = system, 1 = application
	unsigned sd_dpl : 2;		// Descriptor Privilege Level
	unsigned sd_p : 1;			// Present
	unsigned sd_lim_19_16 : 4;	// High bits of segment limit
	unsigned sd_avl : 1;		// Unused (available for software use)
	unsigned sd_rsv1 : 2;		// Reserved
	unsigned sd_g : 1;			// Granularity: limit scaled by 4K when set
	unsigned sd_base_31_24 : 8; // High bits of segment base address
	uint32_t sd_base_63_32;
	unsigned sd_res1 : 8;
	unsigned sd_clear : 8;
	unsigned sd_res2 : 16;
};
// Null segment
#define SEG_NULL \
	(struct Segdesc) { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }
// Segment that is loadable but faults when used
#define SEG_FAULT \
	(struct Segdesc) { 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 1, 0, 0 }
// Normal segment
#define SEG(type, base, lim, dpl)                                     \
	(struct Segdesc)                                                  \
	{                                                                 \
		((lim) >> 12) & 0xffff, (base)&0xffff, ((base) >> 16) & 0xff, \
			type, 1, dpl, 1, (unsigned)(lim) >> 28, 0, 0, 1, 1,       \
			(unsigned)(base) >> 24                                    \
	}
#define SEG64(type, base, lim, dpl)                                   \
	(struct Segdesc)                                                  \
	{                                                                 \
		((lim) >> 12) & 0xffff, (base)&0xffff, ((base) >> 16) & 0xff, \
			type, 1, dpl, 1, (unsigned)(lim) >> 28, 0, 1, 0, 1,       \
			(unsigned)(base) >> 24                                    \
	}

#define SEG16(type, base, lim, dpl)                             \
	(struct Segdesc)                                            \
	{                                                           \
		(lim) & 0xffff, (base)&0xffff, ((base) >> 16) & 0xff,   \
			type, 1, dpl, 1, (unsigned)(lim) >> 16, 0, 0, 1, 0, \
			(unsigned)(base) >> 24                              \
	}

#endif /* !__ASSEMBLER__ */

// Application segment type bits
#define STA_X 0x8 // 可执行段
#define STA_E 0x4 // Expand down (non-executable segments)
#define STA_C 0x4 // Conforming code segment (executable only)
#define STA_W 0x2 // 可写 (non-executable segments)
#define STA_R 0x2 // 可读 (executable segments)
#define STA_A 0x1 // Accessed

// System segment type bits
//#define STS_T16A	0x1	    // Available 16-bit TSS
#define STS_LDT 0x2 // Local Descriptor Table
//#define STS_T16B	0x3	    // Busy 16-bit TSS
//#define STS_CG16	0x4	    // 16-bit Call Gate
//#define STS_TG		0x5	    // Task Gate / Coum Transmitions
//#define STS_IG16	0x6	    // 16-bit Interrupt Gate
//#define STS_TG16	0x7	    // 16-bit Trap Gate
#define STS_T64A 0x9 // Available 64-bit TSS
#define STS_T64B 0xB // Busy 64-bit TSS
#define STS_CG64 0xC // 64-bit Call Gate
#define STS_IG64 0xE // 64-bit Interrupt Gate
#define STS_TG64 0xF // 64-bit Trap Gate

/*
 *
 *	Part 3.  中断/陷阱(Traps).
 *
 */

#ifndef __ASSEMBLER__

// 任务状态段TSS(Task state segment)格式 (参照 Pentium Architecture Book 中的 TSS 结构定义)
struct Taskstate
{
	uint32_t ts_res1;  // -- 长模式中的保留位
	uintptr_t ts_esp0; // 栈指针
	uintptr_t ts_esp1;
	uintptr_t ts_esp2;
	uint64_t ts_res2; // 长模式中的保留位
	uint64_t ts_ist1;
	uint64_t ts_ist2;
	uint64_t ts_ist3;
	uint64_t ts_ist4;
	uint64_t ts_ist5;
	uint64_t ts_ist6;
	uint64_t ts_ist7;
	uint64_t ts_res3;
	uint16_t ts_res4;
	uint16_t ts_iomb; // I/O 映射基址
} __attribute__((packed));

// 中断门和陷阱门的描述符(Gate descriptors for interrupts and traps)结构体，定义门各个段的字节数
struct Gatedesc
{
	unsigned gd_off_15_0 : 16;	// 段中低16位的偏移量
	unsigned gd_ss : 16;		// 段描述符
	unsigned gd_ist : 3;		// # args, 0 for interrupt/trap gates
	unsigned gd_rsv1 : 5;		// 保留位
	unsigned gd_type : 4;		// 类型(STS_{TG,IG32,TG32})
	unsigned gd_s : 1;			// 必须为0 (system)
	unsigned gd_dpl : 2;		// 描述符(新的)特权级别，0:kernel, 3:user
	unsigned gd_p : 1;			// 存在位
	unsigned gd_off_31_16 : 16; // 段中高16位的偏移量
	uint32_t gd_off_32_63;		// 段中高32位的偏移量
	uint32_t gd_rsv2;			// 保留位
};

// 构造 TSS 表
#define SETTSS(desc, type, base, lim, dpl)                             \
	{                                                                  \
		(desc)->sd_lim_15_0 = (uint64_t)(lim)&0xffff;                  \
		(desc)->sd_base_15_0 = (uint64_t)(base)&0xffff;                \
		(desc)->sd_base_23_16 = ((uint64_t)(base) >> 16) & 0xff;       \
		(desc)->sd_type = type;                                        \
		(desc)->sd_s = 0;                                              \
		(desc)->sd_dpl = 0;                                            \
		(desc)->sd_p = 1;                                              \
		(desc)->sd_lim_19_16 = ((uint64_t)(lim) >> 16) & 0xf;          \
		(desc)->sd_avl = 0;                                            \
		(desc)->sd_rsv1 = 0;                                           \
		(desc)->sd_g = 0;                                              \
		(desc)->sd_base_31_24 = ((uint64_t)(base) >> 24) & 0xff;       \
		(desc)->sd_base_63_32 = ((uint64_t)(base) >> 32) & 0xffffffff; \
		(desc)->sd_res1 = 0;                                           \
		(desc)->sd_clear = 0;                                          \
		(desc)->sd_res2 = 0;                                           \
	}

// 构造中断/陷阱门描述符(interrupt/trap gate descriptor)，格式如: pic/中断门.png
// - istrap: 1->trap(=exception)gate, 0->interrupt gate
//	根据i386参考文献的9.6.1.3:“中断门(interrupt gate)和陷阱门(trap gate)的区别在于对IF(interrupt-enable中断使能标志)的影响
//	通过中断门引导的中断会将IF标志位复位，从而防止其他中断干扰当前的 中断处理程序(interrupt handler)
//	随后的 IRET指令 将IF标志位恢复到栈上的 EFLAGS 映像中的值
//	但是，通过陷阱门 trap(=exception) 的中断不会改变IF标志位. ”
// - sel: 中断处理程序(interrupt/trap handler)的代码段选择子
// - off: 中断处理程序的代码段中的偏移量
// - dpl: 描述符特权级别(DPL) -
//    软件使用 int 指令显式调用该中断/陷阱门(interrupt/trap gate)所需的特权级别
#define SETGATE(gate, istrap, sel, off, dpl)                        \
	{                                                               \
		(gate).gd_off_15_0 = (uint64_t)(off)&0xffff;                \
		(gate).gd_ss = (sel);                                       \
		(gate).gd_ist = 0;                                          \
		(gate).gd_rsv1 = 0;                                         \
		(gate).gd_type = (istrap) ? STS_TG64 : STS_IG64;            \
		(gate).gd_s = 0;                                            \
		(gate).gd_dpl = (dpl);                                      \
		(gate).gd_p = 1;                                            \
		(gate).gd_off_31_16 = ((uint64_t)(off) >> 16) & 0xffff;     \
		(gate).gd_off_32_63 = ((uint64_t)(off) >> 32) & 0xffffffff; \
		(gate).gd_rsv2 = 0;                                         \
	}

// 构造调用门描述符(call gate descriptor).
#define SETCALLGATE(gate, ss, off, dpl)                             \
	{                                                               \
		(gate).gd_off_15_0 = (uint32_t)(off)&0xffff;                \
		(gate).gd_ss = (ss);                                        \
		(gate).gd_ist = 0;                                          \
		(gate).gd_rsv1 = 0;                                         \
		(gate).gd_type = STS_CG64;                                  \
		(gate).gd_s = 0;                                            \
		(gate).gd_dpl = (dpl);                                      \
		(gate).gd_p = 1;                                            \
		(gate).gd_off_31_16 = ((uint32_t)(off) >> 16) & 0xffff;     \
		(gate).gd_off_32_63 = ((uint64_t)(off) >> 32) & 0xffffffff; \
		(gate).gd_rsv2 = 0;                                         \
	}

// LGDT、LLDT 和 LIDT 指令使用的伪描述符.
struct Pseudodesc
{
	uint16_t pd_lim;  // 界限 Limit
	uint64_t pd_base; // 基址 Base address
} __attribute__((packed));

#endif /* !__ASSEMBLER__ */

#endif
