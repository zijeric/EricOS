#include "inc/mmu.h"
#include "inc/memlayout.h"

# 为了开启 IA-32e 分页模式，我们需要做如下事情：
# 1.设置 CR0.PG 位；(boot/boot.S 已设置)
# 2.设置 CR4.PAE 位；
# 3.设置 IA32_EFER.LME 位

# x86_64 内核使用4级页表，建立5个页表：
# - 1个 pml4 (4级页映射表)，包含1个项
# - 2个 pdpt (页目录指针)，包含4个项
# - 2个 pde (页目录表)，一共包含2048个项
# 页翻译: cr3->pml4->pdpe->pgdir
# 为了跳过最后一步，我们在 pgdir 中设置 PTE_MBZ 位
.code32
.set pml4, pml4virt         # pml4: 0x102000
.set pdpt1, pml4 + 0x1000
.set pdpt2, pml4 + 2 * 0x1000
.set pde1, pml4 + 3 * 0x1000
.set pde2, pml4 + 4 * 0x1000

# kern/kernel.ld 将 '_switch64' 指定 ELF 的 entry point.
.text
.globl _switch64
_switch64:      # 0x100000

# 保存 bootloader 传递的 multiboot_info 地址 (从 boot/main.c)
    movl $multiboot_info, %eax
    movl %ebx, (%eax)

	movw $0x1234, 0x472			# warm boot
# 重置内核栈指针，以防不是从 bootloader 进入本程序
    movl $0x7c00, %esp

    call verify_cpu     # 检查 CPU 是否支持长模式
    movl $CR4_PAE, %eax # 2.设置 CR4.PAE 位
    movl %eax, %cr4

# 在 0x102000 处构建早期引导 pml4

    # 初始化页表
    # 1.把 pml4 的地址放到 edi 寄存器中
    # 2.清空 eax 寄存器
    # 3.将 ecx 寄存器赋值为 5120 (共有5120项，每项4-bit)
    # 4.清空一块内存，rep stosl: 把 eax 的值(0)写到 edi 指向的地址(pml4所在)，然后 edi+4, ecx-4，重复直到 ecx<=0 
    movl $pml4, %edi
    xorl %eax, %eax
    movl $((4096 / 4) * 5), %ecx
    rep stosl
    # 创建 4GB 的引导页表
    # 设置第4级页表(PML4)，pml4第一项：pml4->pdpt1(PTE_P|PTE_W)
    movl $pml4, %eax
    movl $pdpt1, %ebx
    orl $PTE_P, %ebx
    orl $PTE_W, %ebx
    movl %ebx, (%eax)

    # pml4第二项：pml4->pdpt2(PTE_P|PTE_W)
    movl $pdpt2, %ebx
    orl $PTE_P, %ebx
    orl $PTE_W, %ebx
    movl %ebx, 0x8(%eax)

    # pdpt1第一项：pdpt1->pde1(PTE_P|PTE_W)
    movl $pdpt1, %edi
    movl $pde1, %ebx
    orl $PTE_P, %ebx
    orl $PTE_W, %ebx
    movl %ebx, (%edi)

    # pdpt2第一项：pdpt2->pde2(PTE_P|PTE_W)
    movl $pdpt2, %edi
    movl $pde2, %ebx
    orl $PTE_P, %ebx
    orl $PTE_W, %ebx
    movl %ebx, (%edi)
    
    # 设置 pgdir，使 LA=PA 映射在 KERNBASE(0x8004000000) 的第一个 1GB 内存
    movl $128, %ecx
    movl $pde1, %edi
    movl $pde2, %ebx
    addl $256, %ebx 
    # $0x00000183 = PTE_P|PTE_W|PTE_MBZ
    movl $0x00000183, %eax
    # pde1: 从第0项开始，映射到物理地址[0x0, 0x1000 0000]，共128项，每项 4KB
    # pde2: 从第255项开始，映射到物理地址[0x0, 0x1000 0000]，共128项，每项 4KB
    # 在页目录表 pde1, pde2(2级页表)建立256个项(每项都是 PRESENT+WRITE+MBZ 标记的 pt 1级页表)
    # 最后就会拥有256(128*2)个 2MB 大小的物理页(1GB 内存)
  1:
     movl %eax, (%edi)
     movl %eax, (%ebx)
     addl $0x8, %edi
     addl $0x8, %ebx
     addl $0x00200000, %eax     # 0x00200000(1+21:0)
     decl %ecx
     jnz 1b

    # 设置 cr3 寄存器，存储 pml4 的地址
    movl $pml4, %eax
    movl %eax,  %cr3

	
    # 切换 long mode: 在 MSR 启用长模式，$EFER_MSR = 0xC0000080, $EFER_LME = 8 (inc/mmu.h)
    movl $EFER_MSR, %ecx
    rdmsr
    btsl $EFER_LME, %eax    # 3.设置 IA32_EFER.LME 位
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

    # 加载 GDT
    movl $gdtdesc_64, %eax
    lgdt (%eax)
    pushl $0x8
    movl $_start, %eax
    pushl %eax
    
    .globl jumpto_longmode
    .type jumpto_longmode, @function
jumpto_longmode:
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

