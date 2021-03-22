#include "inc/mmu.h"
#include "inc/memlayout.h"

#define	RELOC(x) ((x) - KERNBASE)

# 页翻译: cr3->pml4->pdpe->pgdir
# 为了跳过最后一步，我们在 pgdir 中设置 PTE_MBZ 位
.code32
.set pml4,  pml4virt
.set pdpt1,  pml4 + 0x1000
.set pdpt2,  pml4 + 2 * 0x1000
.set pde1, pml4 + 3 * 0x1000
.set pde2, pml4 + 4 * 0x1000

.text
.globl _head64
_head64:

# 保存 bootloader 传递的 multiboot_info 地址 (从 boot/main.c)
    movl $multiboot_info, %eax
    movl %ebx, (%eax)

	movw $0x1234, 0x472			# warm boot
# 重置内核栈指针，以防不是从 bootloader 来的
    movl $0x7c00, %esp

    call verify_cpu   # 检查 CPU 是否支持长模式
    movl $CR4_PAE, %eax
    movl %eax, %cr4

# 在 0x8000 处构建早期引导 pml4

    # 初始化页表
    movl $pml4, %edi
    xorl %eax, %eax
    # 将这些单词移动到6页内存，4个二级页 + 1个三级页 + 1个四级页
    movl $((4096 / 4) * 5), %ecx  # moving these many words to the 6 pages with 4 second level pages + 1 3rd level + 1 4th level pages 
    rep stosl
    # 创建 4GB 的引导页表
    # 设置第四级页表(PML4)，只需要第二个条目
    movl $pml4, %eax
    movl $pdpt1, %ebx
    orl $PTE_P, %ebx
    orl $PTE_W, %ebx
    movl %ebx, (%eax)

    movl $pdpt2, %ebx
    orl $PTE_P, %ebx
    orl $PTE_W, %ebx
    movl %ebx, 0x8(%eax)

    # 设置第三级页表(PDPE) 4个条目(ecx中的计数器)，指向[0xa0000, 0xd000]中的下四个物理页(pgdirs) pgdirs
    movl $pdpt1, %edi
    movl $pde1, %ebx
    orl $PTE_P, %ebx
    orl $PTE_W, %ebx
    movl %ebx, (%edi)

    movl $pdpt2, %edi
    movl $pde2, %ebx
    orl $PTE_P, %ebx
    orl $PTE_W, %ebx
    movl %ebx, (%edi)
    
    # 设置 pgdir，使 LA = PA 映射在 KERNBASE 的第一个 1GB 内存
    movl $128, %ecx
    # Start at the end and work backwards 从结尾开始，然后倒过来
    # leal (pml4 + 5*0x1000 - 0x8), %edi
    movl $pde1, %edi
    movl $pde2, %ebx
    # 64th entry - 0x8004000000
    addl $256, %ebx 
    # PTE_P|PTE_W|PTE_MBZ
    movl $0x00000183, %eax
  1:
     movl %eax, (%edi)
     movl %eax, (%ebx)
     addl $0x8, %edi
     addl $0x8, %ebx
     addl $0x00200000, %eax
     subl $1, %ecx
     cmp $0x0, %ecx
     jne 1b

    # 设置最后一个二级页表，使 LA-KERNBASE = PA
    # leal (pml4 + 5*0x1000 + 8*384), %edi  #start with the 384th entry
    /* movl $iter2, %edi */
    /* movl $128, %ecx  # 512 - 384 = 128 */
    /* movl $0x00000183, %eax */
 /* 1: */
 /*    movl %eax, (%edi) */
 /*    addl $0x8, %edi */
 /*    addl $0x00200000, %eax */
 /*    subl $1, %ecx */
 /*    cmp $0x0, %ecx */
 /*    jne 1b */

    # 设置 cr3 寄存器，存储 pml4 的地址
    movl $pml4, %eax
    movl %eax,  %cr3

	
    # 在 MSR 启用长模式
    movl $EFER_MSR, %ecx
    rdmsr
    btsl $EFER_LME, %eax
    wrmsr
    
    # 启用分页
    movl %cr0, %eax
    orl $CR0_PE, %eax
    orl $CR0_PG, %eax
    orl $CR0_AM, %eax
    orl $CR0_WP, %eax
    orl $CR0_MP, %eax
    movl %eax, %cr0
    # 跳转到长模式且 CS=0

    movl $gdtdesc_64, %eax
    lgdt (%eax)
    pushl $0x8
    movl $_start, %eax
    pushl %eax
    
    .globl jumpto_longmode
    .type jumpto_longmode, @function
jumpto_longmode:
    lret

/* long_mode: */
/* .code64 */
/*     pushq $0x08 */
/*     movabs $_back_from_head64, %rax */
/*     pushq %rax */
/*     lretq */

verify_cpu:
    pushfl                   # 在eax中获取eflags -- 检查cpuid的标准方法
    popl %eax
    movl %eax, %ecx
    xorl $0x200000, %eax
    pushl %eax
    popfl
    pushfl
    popl %eax
    cmpl %eax, %ebx
    jz verify_cpu_no_longmode   # 无 cpuid -- 无 long mode

    movl $0x0, %eax              # 查看 cpuid 1是否已经实现
    cpuid
    cmpl $0x1, %eax
    jb verify_cpu_no_longmode    # cpuid 1 尚未实现


    mov $0x80000001,  %eax
    cpuid                 
    test $(1 << 29), %edx                 # 测试 LM-bit 是否已设置.
    jz verify_cpu_no_longmode

    ret

verify_cpu_no_longmode:
    jmp verify_cpu_no_longmode
 
   
.data 
    .p2align 2

gdt_64:
    SEG_NULL
    .quad  0x00af9a000000ffff            # 64 bit CS
    .quad  0x00cf92000000ffff            # 64 bit DS

gdtdesc_64:
    .word 0x17 
    .quad gdt_64

    .p2align PGSHIFT

.globl pml4virt
pml4virt:
    .space 5*PGSIZE

.globl multiboot_info
multiboot_info:
    .long 0

