/* AlvOS 用户态程序的链接器脚本.
   参考 GNU ld 'info' 手册 ("info ld").  */

OUTPUT_FORMAT("elf64-x86-64", "elf64-x86-64", "elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS
{
    /* "."是定位器，用于调整程序被链接到内存的偏移位置(地址)，将用户态程序位置设置在地址 0x800020(线性地址=加载地址) */
	. = 0x800020;

	/* 代码段 */
	.text : {
		*(.text .stub .text.* .gnu.linkonce.t.*)
	}

    /* PROVIDE(symbol = expression) 初始化etext[]，etext标识符代表 .text 程序段的末尾线性地址 */
	PROVIDE(etext = .);

    /* .rodata 只读数据段位于 .text 代码段之后 */
	.rodata : {
		*(.rodata .rodata.* .gnu.linkonce.r.*)
	}

	/* 按4096字节对齐，允许将数据段的地址调整到下一页 */
	. = ALIGN(0x1000);

	/* 非零数据段 */
	.data : {
		*(.data)
	}

    /* edata标识符代表 .data 数据段的末尾线性地址 */
	PROVIDE(edata = .);

    /* (静态/未初始化/初始值为0)数据段 */
	.bss : {
		*(.bss)
	}

    /* end标识符代表 用户程序 的末尾线性地址 */
	PROVIDE(end = .);

	/DISCARD/ : {
		*(.note.GNU-stack .comment)
	}
}
