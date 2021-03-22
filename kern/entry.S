#include "inc/mmu.h"
#include "inc/memlayout.h"

# 逻辑右移
#define SRL(val, shamt)		(((val) >> (shamt)) & ~(-1 << (32 - (shamt))))


##############################################################################################
# 内核(这段代码)被链接到地址 ~(KERNBASE+1MB)，但是bootloader在地址 ~1MB 加载它
# 因为 RELOC(x) 将变量x从它的链接地址(线性地址)映射到它在物理内存中的实际位置(它的加载地址)
#
# 那为什么要减0xF0000000呢？因为现在还没开启分页模式，entry_pgdir这个符号代表的地址又是以0xF0000000为基址的
# 在链接时，链接器根据/kern/kernel.ld中的. = 0xF0100000;来指定的文件的链接地址
# 参考《程序员的自我修养》p127-使用ld链接脚本
##############################################################################################

#define	RELOC(x) ((x) - KERNBASE)

#define MULTIBOOT_HEADER_MAGIC (0x1BADB002)
#define MULTIBOOT_HEADER_FLAGS (0)
#define CHECKSUM (-(MULTIBOOT_HEADER_MAGIC + MULTIBOOT_HEADER_FLAGS))

##############################################################################################
# entry point
##############################################################################################
.code64
.set CODE_SEL, 0x8
.set DATA_SEL, 0x10

.text

# The Multiboot header
.align 4
.long MULTIBOOT_HEADER_MAGIC
.long MULTIBOOT_HEADER_FLAGS
.long CHECKSUM

# '_start' 指定 ELF 的 entry point.
# 因为在 bootloader 进入这段代码时，还没有设置虚拟内存，所以需要 bootloader 跳转到 entry point 的 *物理*地址
.globl		_start
_start = RELOC(entry)

.globl entry
entry:

/* .globl _back_from_head64 */
/* _back_from_head64: */
    # 内核ELF文件中的第一条指令
    movabs   $gdtdesc_64, %rax
    lgdt     (%rax)
    movw    $DATA_SEL, %ax
    movw    %ax, %ds
    movw    %ax, %ss
    movw    %ax, %fs
    movw    %ax, %gs
    movw    %ax, %es
    pushq   $CODE_SEL
    movabs  $relocated, %rax
    pushq   %rax
    lretq
relocated:

	# 清除内核栈帧指针寄存器(RBP)，这样一旦进入调试C代码，内核栈回溯(stack backtraces)就会被正确终止.
	movq	$0x0, %rbp			# RBP 指向内核栈帧底(栈帧基址)

	# 设置内核栈指针为 bootstacktop，RSP 指向内核栈栈顶
	movabs	$(bootstacktop), %rax
	movq  %rax, %rsp

	# 现在间接调用函数 i386_init() 
    movabs $i386_init, %rax
	call *%rax

	# 以防内核代码出错而返回，内核自旋.
spin:	jmp	spin


.data
##############################################################################################
# boot stack
##############################################################################################
	.p2align	PGSHIFT		# 强制页对齐
	.globl		bootstack
bootstack:
	.space		KSTKSIZE
	.globl		bootstacktop   
bootstacktop:

.data
    
    .p2align 3
    .globl kernel_64
kernel_64:
    SEG_NULL
    SEG64(STA_X|STA_R, 0x0, 0xffffffff)    # 64 bit CS
    SEG64(STA_R|STA_W, 0x0, 0xffffffff)    # 64 bit DS
    SEG64USER(STA_X|STA_R, 0x0, 0xffffffff) # 64 bit USER CS
    SEG64USER(STA_R|STA_W, 0x0, 0xffffffff) # 64 bit USER DS
    .quad   0x0080890000000000  /* TS descriptor */ 
    .quad   0x0000000000000000 /* TS continued */

     .globl gdtdesc_64
gdtdesc_64:
    .word 0x37 
    .quad kernel_64

