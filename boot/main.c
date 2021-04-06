#include "inc/x86.h"
#include "inc/elf.h"

/**************************************************************************************
 * main.c 唯一的工作就是从第一个 IDE 硬盘引导一个 ELF 内核映像.
 * 即，加载内核映像(ELF Linux可执行文件)到 物理地址0x10000(ELFHDR)
 *
 * 磁盘布局
 *  * 这个程序(boot.S and main.c)是引导加载器程序，应该被保存在磁盘的第一个扇区
 *
 *  * 第二个扇区往后保存着内核映像
 *
 *  * 内核映像必须必须是ELF格式的
 *
 * 引导内核映像步骤
 *  * 当 CPU 启动时，它将 BIOS 加载到内存中并执行它
 *
 *  * BIOS 初始化设备，设置中断例程，并将引导设备的第一个扇区(例如，硬盘驱动器)读入内存并跳转到它.
 *
 *  * 假设此引导加载程序存储在硬盘驱动器的第一个扇区中，则此代码将接管 CPU 控制权...
 *
 *  * 控制从 boot.S 中开始 -- boot.S 设置保护模式和内核栈，为了运行 C 代码，然后调用 bootmain()
 *
 *  * 这个文件中的 bootmain() 将接管内核，读取内核到内存并跳转到内核.
 **************************************************************************************/

#define SECTSIZE 512
// 定义一个指向内存中 ELF 文件头存放位置的结构体指针
// (类似于数组名，不可以通过指针修改指向变量的值)
// 0x10000 已经是是高地址的最低处，也可以是其它高地址处
#define ELFHDR ((struct Elf *)0x10000)

void readseg(uint32_t, uint32_t, uint32_t); // 读取 ELF 文件中的一个段

void bootmain(void)
{
	// ph: ELF头部的程序头，eph: ELF头部所有程序之后的结尾处
	struct Proghdr *ph, *eph;

	// 需要引用 boot.S 中的 multiboot_info，利用 extern
	extern char multiboot_info[];
	// 从磁盘读取出第一个页(首4096Byte)的数据
	// 即，将操作系统映像文件的前 0~4096(PAGESIZE 4KB) 读取到内存 ELFHDR(0x10000) 处, 这其中包括 ELF 文件头部
	// 根据 ELF 文件头部规定属性格式，可以找到文件的每一段的位置
	readseg((uint32_t)ELFHDR, SECTSIZE * 8, 0);

	// ELF 文件的头部就是用来描述这个 ELF 文件如何在存储器中存储，
	// 文件是可链接文件还是可执行文件，都会有不同的 ELF 头部格式
	// 对于一个可执行程序，通常包含存放代码的文本段(text section)，
	// 存放全局变量的 data 段，以及存放字符串常量的 rodata 段

	// 通过判断 ELF 头部的魔数是否为正确的ELF
	if (ELFHDR->e_magic != ELF_MAGIC)
		return;

	// ph: 指向程序段头表的指针，准备加载所有程序段
	ph = (struct Proghdr *)((uint8_t *)ELFHDR + ELFHDR->e_phoff);
	// 明确 ELF 文件中程序段的个数: eph = (struct Proghdr *)0x10000 + e_phoff + e_phnum;
	eph = ph + ELFHDR->e_phnum;

	// 将内核的每一段依次加载到内存相应的位置，ph++(struct Proghdr)
	for (; ph < eph; ph++)
		// 将ELFHDR中所有的程序段信息读入 ph
		// p_pa: 目标加载地址(期望包含该段的目的物理地址: 0x100000)，由 kernel.ld 决定内核的起始物理地址
		// p_memsz: 在内存中的大小，p_offset: 被读取时的偏移量
		readseg(ph->p_pa, ph->p_memsz, ph->p_offset);

	// 为了传递参数 multiboot_info，将其传入 EBX
	__asm __volatile("movl %0, %%ebx"
					 :
					 : "r"(multiboot_info));
	// bootstrap 执行的最后一条指令：将内核 ELF 文件载入内存后，转移到入口地址 entry 执行, 且永不返回
	// 此处的 entry 并非 kern/entry.S 的 entry 标识符，
	((void (*)(void))((uint32_t)(ELFHDR->e_entry)))();
}

// 从内核的 offset 处读取 count 个字节到物理地址 pa 处
// 可能读取会超过count个（扇区对齐）
void readseg(uint32_t pa, uint32_t count, uint32_t offset)
{
	uint32_t end_pa;

	// 结束物理地址
	end_pa = pa + count;
	// 偏移地址
	uint32_t orgoff = offset;

	// 向下舍入到扇区边界
	pa &= ~(SECTSIZE - 1);

	// offset 从字节转换为硬盘的第i扇区，内核从扇区3开始，
	offset = (offset / SECTSIZE) + 1;

	// 如果速度太慢，可以一次读取许多扇区.
	// 会在内存中写入比要求的更多的内容，但是没关系 —— 以递增的顺序加载.
	while (pa < end_pa)
	{
		// 循环等待磁盘就绪
		while ((inb(0x1F7) & 0xC0) != 0x40)
			;

		outb(0x1F2, 1);
		outb(0x1F3, offset);
		outb(0x1F4, offset >> 8);
		outb(0x1F5, offset >> 16);
		outb(0x1F6, (offset >> 24) | 0xE0);
		outb(0x1F7, 0x20);

		// 循环等待磁盘就绪
		while ((inb(0x1F7) & 0xC0) != 0x40)
			;

		// 读取一个扇区，数据存放在内存中的 pa
		insl(0x1F0, (uint8_t *)pa, SECTSIZE / 4);

		pa += SECTSIZE;
		offset++;
	}
}
/*
void
waitdisk(void)
{
	// wait for disk ready
	while ((inb(0x1F7) & 0xC0) != 0x40)
		;
}

void
readsect(void *dst, uint32_t offset)
{
	// wait for disk to be ready
	waitdisk();

	outb(0x1F2, 1);		// count = 1
	outb(0x1F3, offset);
	outb(0x1F4, offset >> 8);
	outb(0x1F5, offset >> 16);
	outb(0x1F6, (offset >> 24) | 0xE0);
	outb(0x1F7, 0x20);	// cmd 0x20 - read sectors

	// wait for disk to be ready
	waitdisk();

	// read a sector
	insl(0x1F0, dst, SECTSIZE/4);
}
*/
