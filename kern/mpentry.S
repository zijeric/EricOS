// APs 的汇编引导代码

#include "inc/mmu.h"
#include "inc/memlayout.h"

###################################################################
# entry point for APs
###################################################################

# 引导CPU(BSP) 通过 STARTUP IPI 启动每个 非引导CPU(AP)
# 根据《Multi-Processor Specification》的 b. 4.2节，AP 会以实模式开始，CS:IP初始值为 XY00:0000
# 其中 XY(len: 8-bit) 是与 STARTUP 一起发送的，因此，本文件的代码必然会从4096字节对齐的地址开始执行
#
# 因为本文件设置 DS=0，所以只能从物理内存中的低2^16字节的地址运行(实模式：16-bit)
#
# boot_aps()(kern/init.c)将本文件的代码复制到 MPENTRY_PADDR(满足上述限制)
# 然后，函数将分配的各个 AP 的内核栈地址存储在 mpentry_kstack 中，发送 STARTUP IPI，
# 并等待函数 mp_main()(kern/init.c) 确认当前 AP 已经启动
#
# 该代码类似于boot/boot.S，除了
# - 不需要启用A20
# - 使用 MPBOOTPHYS 来计算其符号的绝对地址，而不是依靠 linker 初始化
# 对于boot/boot.S来说，它是由 BIOS 加载到0x7c00的再开始代码，由于尚未开启分页，链接地址即为物理地址
# 所以boot/boot.S中标识符的物理地址是直接由链接地址确定的

# 因为该文件最终会在 MPENTRY_PADDR 运行，所以 mpentry.S 汇编中的地址符号标志都需要使用 MPBOOTPHYS，重定向到 MPENTRY_PADDR
# 对于boot/boot.S来说，它是由BIOS加载到0x7c00的，然后开始执行这部分代码，由于当时尚未开启分页，链接地址即为物理地址
# 所以boot/boot.S中标识符的物理地址是直接由链接地址确定的
# symbol = (symbol - mpentry_start)(s在该文件的声明偏移地址) + MPENTRY_PADDR(运行地址偏移)
#define MPBOOTPHYS(s) ((s) - mpentry_start + MPENTRY_PADDR)

.set LONG_MODE_CSEG, 0x8	# long mode kernel code segment selector
.set LONG_MODE_DSEG, 0x10	# long mode kernel data segment selector
.set PROT_MODE_CSEG, 0x8	# protected mode kernel code segment selector
.set PROT_MODE_DSEG, 0x10	# protected mode kernel data segment selector

# 复用 BSP 引导阶段的4级页表
.set pml4, pml4virt

.code16           
.globl mpentry_start
mpentry_start:
	# 禁用中断
	cli
	# 初始化段选择子
	xorw    %ax, %ax
	movw    %ax, %ds
	movw    %ax, %es
	movw    %ax, %ss

	# 加载 32-bit GDT，并切换到保护模式，MPBOOTPHYS 将地址更新为加载到对应位置后的地址
	lgdt    MPBOOTPHYS(gdt32desc)
	movl    %cr0, %eax
	orl     $CR0_PE, %eax
	movl    %eax, %cr0
	# 长跳转到下一个指令的symbol，刷新 CS:IP
	ljmp	$(PROT_MODE_CSEG),$(MPBOOTPHYS(start32))

.code32
start32:
	# 初始化保护模式的段寄存器
	movw    $(PROT_MODE_DSEG), %ax
	movw    %ax, %ds
	movw    %ax, %es
	movw    %ax, %ss
	movw    $0, %ax
	movw    %ax, %fs
	movw    %ax, %gs

	# 为了开启 IA-32e 分页模式，设置 CR4.PAE 位
	movl $CR4_PAE, %eax
	movl %eax, %cr4
	
	lgdt    MPBOOTPHYS(gdtdesc)
	movl    %cr0, %eax
	orl     $CR0_PE, %eax
	movl    %eax, %cr0

	# 设置初始页表，我们还不能使用 boot_pml4e，因为 EIP 仍然在低地址空间运行.
	movl    $pml4, %eax
	movl    %eax, %cr3

	# 在 MSR 启用 64-bit长模式
	movl $EFER_MSR, %ecx
	rdmsr
	btsl $EFER_LME, %eax
	wrmsr

	# 启用分页. 
	movl    %cr0, %eax
	orl     $(CR0_PE|CR0_PG|CR0_WP), %eax
	movl    %eax, %cr0

	# 长跳转到下一个指令的symbol，刷新 CS:IP
	ljmpl   $(LONG_MODE_CSEG), $(MPBOOTPHYS(start64))

.code64
start64:
	# 初始化长模式的段寄存器
	movw    $(LONG_MODE_DSEG), %ax
	movw    %ax, %ds
	movw    %ax, %es
	movw    %ax, %ss
	movw    $0, %ax
	movw    %ax, %fs
	movw    %ax, %gs

	/**
	 * 切换到 boot_aps() 中分配的 Per-CPU 栈
	 * 如果共享的内核栈，中断时，硬件会先自动将相关寄存器进栈，然后才执行锁的检查，共享内核栈可能会导致系统崩溃
	 * 支持多个 CPU 的时候，只有一份内核4级页表，所有CPU都会使用这个4级页表映射CPU栈
	 * 不同的 CPU 栈映射到不同的虚拟地址上
	 * 注意：不同的用户环境是可以同时将用户栈物理地址映射到 UXSTACKTOP 上的
	 * 因为每一个用户环境都有一份独立的4级页表，创建用户环境的时候会分配和映射一页用户栈物理页到UXSTACKTOP上
	 * 多个 CPU 同时运行多个用户环境的时候，实际上都是使用各自的4级页表进行寻址和存储数据到各自的用户栈物理页上的
	 */
	movabs    mpentry_kstack, %rax
	movq    %rax, %rsp
	movq    $0x0, %rbp       # nuke frame pointer

	# 调用 mp_main().  (Exercise for the reader: why the indirect call?)
	movabs    $mp_main, %rax
	call    *%rax

	# 如果 mp_main 返回 (it shouldn't), 自旋.
spin:
	jmp     spin

# Bootstrap GDT
.p2align 3					# 强制 8 字节对齐
gdt:
	SEG_NULL
	SEG64(STA_X|STA_R,0x0,0xffffffff)    # 64-bit CS
	SEG64(STA_R|STA_W,0x0,0xffffffff)    # 64-bit DS

gdtdesc:
	.word   0x17				# sizeof(gdt) - 1
	.long   MPBOOTPHYS(gdt)			# address gdt

.p2align 2
gdt32:
	SEG_NULL
	SEG(STA_X|STA_R,0x0,0xffffffff)      # 32-bit CS
	SEG(STA_R|STA_W,0x0,0xffffffff)      # 32-bit DS

gdt32desc:
	.word   0x17				# sizeof(gdt) - 1
	.long   MPBOOTPHYS(gdt32)			# address gdt


.globl mpentry_end
mpentry_end:
	nop
