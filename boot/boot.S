#include "inc/mmu.h"
# bootlaoder: <AT&T> boot.S + <C> main.c
# boot.S: 将 CPU 从实模式转换到32位保护模式，再跳转到main.c
# 只有在保护模式中才嗯那个访问到物理内存高于 1MB 的空间
#
# BIOS将该代码从硬盘的第一个扇区加载到物理地址为0x7C00的内存中，
# 并在%cs=0 %rip=0x7C00的实模式下开始执行.
# 80386CPU为了兼容8086程序，最开始启动时是以16位的实模式工作
# 生成kernel.img磁盘映像时，boot.S中的引导代码将会被放在引导扇区
# 80386 CPU 加电启动后，会执行BIOS中的默认引导程序，BIOS引导程序会将引导扇区中(第一个磁盘块)的内容读入内存，并放置在0x7C00
# 随后CPU会跳转到0x7c00处开始第一条指令的执行，即boot.S的第一条指令(start:)

.set PROT_MODE_CSEG, 0x8         # 内核代码段选择子
.set PROT_MODE_DSEG, 0x10        # 内核数据段选择子
.set CR0_PE_ON,      0x1         # 启用保护模式的标志位

// 构造 multiboot 结构，用于将引导信息从引导加载程序传递到内核的标准
// multiboot 的信息放在该 boot 扇区地址之后
.set multiboot_info, 0x7000
.set e820_map, multiboot_info + 52

.set MB_flag, multiboot_info
.set MB_mmap_len, multiboot_info + 44
.set MB_mmap_addr, multiboot_info + 48

.globl multiboot_info
.globl start
start:
  .code16                     # 声明以下为16位模式的汇编代码
  cli                         # 禁用中断，当前内核仍无法处理中断
  cld                         # 关闭字符串操作顺序递增(从低地址到高地址)

  # 初始化重要的段寄存器 (DS, ES, SS). 
  xorw    %ax, %ax             # ax置零，为段寄存器赋值
  movw    %ax, %ds             # 0 -> 数据段
  movw    %ax, %es             # 0 -> 拓展段
  movw    %ax, %ss             # 0 -> 栈段

  # 启用 A20:    —— A20总线是专门用来转换地址总线的第二十一位
  #   为了向下兼容8086 CPU, 任何分段方式访问超过 1MB 的内存都会使得溢出的第二十一位(A20)为0
  #   即使能够在保护模式下寻址 4GB 的地址空间，地址最终还是在 1MB 内.   下面这段代码会禁用这个设置.
seta20.1:
  inb     $0x64, %al               # 等待 0x64 号 I/O 端口空闲
  testb   $0x2, %al
  jnz     seta20.1

  movb    $0xd1, %al               # 空闲: 0xd1 -> port 0x64
  outb    %al, $0x64

seta20.2:
  inb     $0x64, %al               # 等待 0x64 号 I/O 端口空闲
  testb   $0x2, %al
  jnz     seta20.2

  movb    $0xdf, %al               # 空闲: 0xdf -> port 0x60
  outb    %al, $0x60

  # 从 BIOS 获取 E820 内存映射
  # 通过 BIOS 中断服务程序 INT 15h 的主功能号 E820h 来获取物理地址空间
  # 物理地址空间信息结构是 20B 的结构体(8B: 起始地址，8B: 长度(Byte)，4B: 内存类型)
  # 内存类型: 可用物理内存、保留或无效值、ACPI 回收内存、ACPINVS 内存、未定义
do_e820:
  # 调用传入值
  movl $0xe820, %eax              # %eax = 0xe820
  movl $e820_map, %ebx
  movl %ebx, %edi
  addl $4, %edi                   # es:edi => 返回结果的缓存区地址
  xorl %ebx, %ebx                 # %ebx = 0x0
  movl $0x534D4150, %edx          # %edx = 0x534D4150 = str "SMAP"
  movl $24, %ecx                  # %ecx = 预设返回结果的缓存区结构体长度，字节为单位
  int $0x15                       # int 15h
  jc failed
  cmpl %eax, %edx
  jne failed
  testl %ebx, %ebx
  je failed
  movl $24, %ebp

next_entry:
  # 若调用 e820 成功，调用返回值，%edi自增，下一个 entry
  movl %ecx, -4(%edi)
  addl $24, %edi
  movl $0xe820, %eax
  movl $24, %ecx
  int $0x15
  jc done
  addl $24, %ebp
  testl %ebx, %ebx
  je done
  jmp next_entry

done:
  # 物理地址空间信息读取完成
  movl %ecx, -4(%edi)
  movw $0x40, (MB_flag) # multiboot 的信息标志位
  movl $e820_map, (MB_mmap_addr)
  movl %ebp, (MB_mmap_len)
  
failed:
  # 将 GDT 表的首地址加载到 GDTR，并设置寄存器 cr0 的保护模式允许位，使处理器从实模式切换到保护模式
  # 而且分段映射的虚拟地址与其对应的实际地址相同[0~4G]，因此有效内存映射在转换期间不会更改
  lgdt    gdtdesc
  movl    %cr0, %eax
  orl     $CR0_PE_ON, %eax
  movl    %eax, %cr0
 
  # $PROT_MODE_CSEG 代表的是段选择子0x0，偏移地址 $protcseg 接下来的链接地址
  # 通过跳转刷新CPU流水线上的指令，刷新CS、IP等寄存器，从而切换处理器为32位保护模式
  # 令CPU清空实模式下已经存在流水线中的指令，由于当前处于保护模式下执行实模式的指令会出现各种问题
  ljmp    $PROT_MODE_CSEG, $protcseg

  .code32                     # 声明为32位保护模式汇编代码
protcseg:
  # 跳转至保护模式后，需要刷新数据段寄存器(因为32位引入了特权级保护，避免数据段寄存器之前的值不对而出现漏洞)
  movw    $PROT_MODE_DSEG, %ax    # 数据段选择子
  movw    %ax, %ds                # -> DS: 数据段
  movw    %ax, %es                # -> ES: 拓展段
  movw    %ax, %fs                # -> FS
  movw    %ax, %gs                # -> GS
  movw    %ax, %ss                # -> SS: 栈段
  
  # 设置栈指针ESP=0x7c00，从而可以调用C语言编写的 bootmain(为了尽量少用汇编语言写内核).
  movl    $start, %esp
  # 通过 %ebx 传参 $multiboot_info
  movl $multiboot_info, %ebx
  # 调用 boot/main.c 的 bootmain
  call bootmain

  # 如果指令从 bootmain 返回(正常情况下不会返回), 死循环.
spin:
  jmp spin

 # 引导器阶段的全局描述符表 GDT，强制4字节对齐
.p2align 2
 # 采用平坦内存模型(映射整个内存大小)，所以代码段和数据段在内核中均只存在一个
gdt:
  # 按照x86 GDT的规定，第一个为NULL段
  SEG_NULL				# null seg
  SEG(STA_X|STA_R, 0x0, 0xffffffff)	  # 代码段
  SEG(STA_W, 0x0, 0xffffffff)	        # 数据段
gdtdesc:          # gdt描述信息，长度和地址
  .word   0x17                            # sizeof(gdt) - 1
  .long   gdt                             # address gdt
