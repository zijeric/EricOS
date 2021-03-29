#include "inc/mmu.h"
#include "inc/memlayout.h"

# entry.S 的任务
# 1.重新设置gdt，为了隔离用户环境，以及为了上下文切换的 TSS 任务状态段表
# 2.为了能运行 C 代码，设置临时的内核栈
# 3.跳转到内核真正的代码 kern/init.c i386_init()

# 逻辑右移
#define SRL(val, shamt)		(((val) >> (shamt)) & ~(-1 << (32 - (shamt))))


##############################################################################################
# 内核(这段代码)被链接到地址 ~(KERNBASE+1MB)，但是bootloader在地址 ~1MB 加载它
# 因为 RELOC(x) 将变量x从它的链接地址(线性地址)映射到它在物理内存中的实际位置(它的加载地址)
#
# 那为什么要减0xF0000000呢？因为现在还没开启分页模式，_bootstrap_pgdir这个符号代表的地址又是以0xF0000000为基址的
# 在链接时，链接器根据/kern/kernel.ld中的. = 0xF0100000;来指定的文件的链接地址
# 参考《程序员的自我修养》p127-使用ld链接脚本
##############################################################################################

#define	RELOC(x) ((x) - KERNBASE)

#define MULTIBOOT_HEADER_MAGIC (0x1BADB002)
#define MULTIBOOT_HEADER_FLAGS (0)
#define CHECKSUM (-(MULTIBOOT_HEADER_MAGIC + MULTIBOOT_HEADER_FLAGS))

##############################################################################################
# AlvOS kernel entry
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

# 因为 bootstrap 进入这段代码时，还没有设置虚拟内存，所以需要 bootloader 跳转到 _bootstrap 的 *物理*地址
.globl		_start
_start = RELOC(_bootstrap)

.globl _bootstrap
_bootstrap:

    # 加载新的 GDT
    movabs   $gdtdesc_64, %rax
    lgdt     (%rax)
    # 初始化以下段寄存器为数据段选择子
    movw    $DATA_SEL, %ax
    movw    %ax, %ds
    movw    %ax, %ss
    movw    %ax, %fs
    movw    %ax, %gs
    movw    %ax, %es
    # 通过 push 64-bit CS 和 $relocated 地址，再 lret 切换到新的 GDT 中的 CS:RIP
    pushq   $CODE_SEL
    movabs  $relocated, %rax
    pushq   %rax
    lretq
relocated:

	# 为了使内核栈回溯(stack backtraces)不会返回到这里，清除内核栈帧指针寄存器(%ebp).
    # %ebp 指向内核栈帧底(栈帧基址)，内核栈回溯(pic/内核栈回溯.png)直到 %ebp 为0才会停止
	movq	$0x0, %rbp

	# 设置内核栈指针为 bootstacktop，%esp 指向内核栈栈顶
	movabs	$(bootstacktop), %rax
	movq  %rax, %rsp

	# 间接调用函数 i386_init() 
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
	.space		KSTKSIZE    # 从此处地址开始的 KSTKSIZE:32KB(inc/memlayout.h) 字节的区域都属于临时内核栈
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

