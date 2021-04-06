#include "inc/mmu.h"
#include "inc/memlayout.h"

# bootloader 引导加载程序，bootstrap 内核引导程序
# 从 boot/main.c 来到内核映像的入口点(ELF entry point)，bootstrap.S 的任务:
# 1.将 multiboot_info(E820) 传递给内核
# 2.检查 CPU 是否支持长模式，并从保护模式切换到长模式
# 3.开启 IA-32e 分页模式，为了让内核代码能正常在高地址运行，要设置引导阶段的页表映射
# 开启分页时尚未跳转到内核代码，因此还要保证在低地址也能运行内核(分页映射)，
# 因此 boot 页表需要映射高低两段虚拟地址到内核的物理地址

# 参考: AMD64_Architecture_Programmers_Manual Volume 2 System Programming
# 为了开启 IA-32e 分页模式，需要做如下事情：
# 1.设置 CR0.PG 位；(boot/boot.S 已设置)
# 2.设置 CR4.PAE 位；
# 3.设置 IA32_EFER.LME 位
#
# 为了设置 x86_64 引导阶段的4级页表映射，需要建立5个页表：
# - 1个 pml4 (4级页映射表)，包含1个项
# - 2个 pdpt (页目录指针)，包含4个项
# - 2个 pde (页目录表)，一共包含2048个项
# bootstrap.S 设置的页表映射: cr3 -> pml4 -> pdpe -> pgdir
# 为了跳过 pgdir->pt，在 pgdir 中设置 PTE_MBZ 位
.code32
.set pml4, pml4virt         # pml4: 0x102000
.set pdpt1, pml4 + 0x1000   # pml4, pdpt, pde 大小: 4096-bit(PGSIZE)
.set pdpt2, pml4 + 2 * 0x1000
.set pde1, pml4 + 3 * 0x1000
.set pde2, pml4 + 4 * 0x1000

##############################################################################################
# ELF entry point
##############################################################################################

# kern/kernel.ld 将 entry(0x100000) 指定 ELF 的 entry point.
# 0x100000 还在 256MB 物理内存内(可使用)，CPU 可以正常执行 bootstrap 代码
# 注意，在 kern/kernel.ld 设置内核基址为 0x8004000000(pml4表偏移1 + pdpt偏移0 + pd偏移32)
# 内核基址可以设置在任何可用的高地址处
.text
.globl entry
entry:      # 0x100000

    # %eax = $multiboot_info, 值为其地址 0x107000 (重定向)
    movl $multiboot_info, %eax
    # %ebx 存储了 multiboot_info 的地址(0x7000)，%eax($multiboot_info) 指向 0x7000
    movl %ebx, (%eax)

    # warm boot
	movw $0x1234, 0x472
    # 重置内核栈指针，以防不是从 bootloader 进入本程序
    movl $0x7c00, %esp

    call verify_cpu     # 检查 CPU 是否支持长模式
    movl $CR4_PAE, %eax # 2.设置 CR4.PAE 位
    movl %eax, %cr4

# 在 0x102000 处构建早期引导 pml4

    # 初始化页表空间(清空)
    # 1.把 pml4 的地址放到 edi 寄存器中
    # 2.清空 eax 寄存器
    # 3.将 ecx 寄存器赋值为 5120 (共有5120项，每项4-bit)
    # 4.清空页表所在的物理内存，rep stosl: 把 eax 的值(0)写到 edi 指向的地址(pml4所在)，然后 edi+4, ecx-4，重复直到 ecx<=0 
    movl $pml4, %edi
    xorl %eax, %eax
    movl $((4096 / 4) * 5), %ecx
    rep stosl

    # 设置引导阶段页表映射
    # pml4第一项：pml4->pdpt1: PTE_P, PTE_W
    movl $pml4, %eax
    movl $pdpt1, %ebx
    orl $PTE_P, %ebx
    orl $PTE_W, %ebx
    movl %ebx, (%eax)

    # pml4第二项：pml4->pdpt2: PTE_P, PTE_W
    movl $pdpt2, %ebx
    orl $PTE_P, %ebx
    orl $PTE_W, %ebx
    movl %ebx, 0x8(%eax)

    # pdpt1第一项：pdpt1->pde1: PTE_P, PTE_W
    movl $pdpt1, %edi
    movl $pde1, %ebx
    orl $PTE_P, %ebx
    orl $PTE_W, %ebx
    movl %ebx, (%edi)

    # pdpt2第一项：pdpt2->pde2: PTE_P, PTE_W
    movl $pdpt2, %edi
    movl $pde2, %ebx
    orl $PTE_P, %ebx
    orl $PTE_W, %ebx
    movl %ebx, (%edi)
    
    # 设置 pgdir 2MB页表映射，使物理内存的前256MB LA=PA，且映射到 KERNBASE(0x8004000000) 后的256MB
    movl $128, %ecx
    movl $pde1, %edi
    movl $pde2, %ebx
    # 为了让 CPU 能执行内核代码，映射内核的基址，256/8 = 32偏移 (一个页表项占8个字节)
    addl $256, %ebx

    # $0x00000183 = (PTE_P|PTE_W|PTE_MBZ)，如图: pic/IA-32e的3级页表映射.png
    # PTE_MBZ 对应于 手册中的 PDE.PS 位，取消 Level 1 PT，从而使映射的物理页帧帧(物理内存)大小为 2MB
    movl $0x00000183, %eax

    # VPN range             Entry         Flags            Physical page            Size
    #  [000000000-000000000] PML4[000]     ----A---WP
    #    [000000000-00003ffff]  PDP1[000]     ----A---WP
    #      [000000000-0000001ff]  PDE[000]     -GSDA---WP 0000000000-00000001ff
    #      [000000200-0000003ff]  PDE[001]     -GS-A---WP 0000000200-00000003ff
    #      [000000400-00000ffff]  PDE[002-07f] -GS-----WP 0000000400-000000ffff     128*2MB=256MB
    #  [008000000-008000000] PML4[001]     ----A---WP
    #    [008000000-00803ffff]  PDP2[000]     ----A---WP
    #     [008004000-0080043ff]  PDE[020-021] -GSDA---WP 0000000000-00000003ff
    #     [008004400-008013fff]  PDE[022-09f] -GS-----WP 0000000400-000000ffff      128*2MB=256MB
    # 在页目录表 pde1, pde2 各映射128项
    # 1. la=va[0x0, 0x00ffff000] -> pa[0x0, 0x00ffff000];
    # 2. la=va[0x8004000000(KERNBASE), 0x8013fff000] -> pa[0x0, 0x00ffff000]
    # 使物理内存的前256MB LA=PA，且映射到 KERNBASE(0x8004000000) 后的256MB
  1:
     movl %eax, (%edi)
     movl %eax, (%ebx)
     addl $0x8, %edi
     addl $0x8, %ebx
     addl $0x00200000, %eax     # 0x00200000[i(1~128)+21:0]
     decl %ecx
     jnz 1b

    # 将 pml4 表的地址存储在 cr3 寄存器
    # 根据手册：CPU 通过 cr3 寄存器获得 PML4 地址，然后解析虚拟地址的页表映射获取各级页表偏移量，最后转换为物理页帧(内存)
    movl $pml4, %eax
    movl %eax,  %cr3
    # 完成了最难却最重要的页表映射(为了 CPU 运行内核代码)
	
    # 切换 long mode: 在 MSR 启用长模式，$EFER_MSR = 0xC0000080, $EFER_LME = 8 (inc/mmu.h)
    movl $EFER_MSR, %ecx
    rdmsr
    btsl $EFER_LME, %eax    # 3.设置 IA32_EFER.LME 位
    wrmsr
    
    # 启用分页，以后 CPU 就会根据 cr3 寄存器寻址
    movl %cr0, %eax
    orl $CR0_PE, %eax
    orl $CR0_PG, %eax
    orl $CR0_AM, %eax
    orl $CR0_WP, %eax
    orl $CR0_MP, %eax
    movl %eax, %cr0
    # 跳转到长模式且 CS=0

    # 加载 GDT
    movl $gdtdesc_64, %eax
    lgdt (%eax)
    # push GDT 64-bit CS 进栈
    pushl $0x8
    # push kern/entry.S 的地址进栈
    movl $_start, %eax
    pushl %eax

    .globl jumpto_longmode
    .type jumpto_longmode, @function
jumpto_longmode:
    # 通过刷新 CS:RIP，CS 加载栈中 GDT 64-bit CS，RIP 加载栈中的 entry.S 的地址，从而跳转到 kern/entry.S
    lret

# 检测 CPU 是否支持64位 long mode
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
    .word 0x17                           # sizeof(gdt) - 1
    .quad gdt_64                         # address gdt

    .p2align PGSHIFT                     # 页对齐

.globl pml4virt
pml4virt:
    .space 5*PGSIZE     # [0x102000, 0x107000]

.globl multiboot_info
multiboot_info:         # 4-bit
    .long 0


