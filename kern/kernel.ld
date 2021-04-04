
/* AlvOS 内核的简单链接器脚本.
   参考 GNU ld 'info' 手册 ("info ld"). */
/* 将内核链接后的输出文件格式(默认、大端、小端)定义为 elf64-x86-64
    内核计算机架构为 i386:x86-64，
    内核的 e_entry 定义为 entry (kern/bootstrap.S)，内核(执行程序)的第一条指令所在地址 */
OUTPUT_FORMAT("elf64-x86-64", "elf64-x86-64", "elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(entry)

SECTIONS
{
    /* "."是定位器，用于调整程序被链接到内存的偏移位置(地址)，将 bootstrap.o 位置设置在地址 0x100000(线性地址=加载地址) */
    . = 0x100000;

    .bootstrap : {
        /* .bootstrap引导程序段 包含文件 obj/kern/bootstrap.o 的.text .data .bss(代码段 非零数据段 (静态/零)数据段) */
        obj/kern/bootstrap.o (.text .data .bss)
    }

	/* 将 text 段设置在内存的偏移 0x8004200000(线性地址)，加载地址为 0x200000 */
	. = 0x8004200000;

	.text : AT(0x200000) {
        /* obj/kern/bootstrap.o 在 .bootstrap 引导段已经链接，接下来内核的各段都会过滤此文件的所有段 */
        /* .text程序段保存着 除了 EXCLUDE_FILE 声明的文件，其他所有输入文件的(.text .stub .text.* .gnu.linkonce.t.*)的程序段 */
		*(EXCLUDE_FILE(obj/kern/bootstrap.o) .text .stub .text.* .gnu.linkonce.t.*)
	}

    /* PROVIDE(symbol = expression) 初始化etext[]，etext标识符代表 .text 程序段的末尾线性地址 */
	PROVIDE(etext = .);

    /* .rodata 只读数据段位于 .text 代码段之后 */
	.rodata : {
		*(EXCLUDE_FILE(obj/kern/bootstrap.o) .rodata .rodata.* .gnu.linkonce.r.*)
	}

	/* 按4096字节对齐，允许将数据段的地址调整到下一页 */
	. = ALIGN(0x1000);

	/* 非零数据段 */
	.data : {
		*(EXCLUDE_FILE(obj/kern/bootstrap.o) .data)
	}

    /* 初始化 kern/init.c 中的 edata[]，edata 标识符代表 .data 数据段的末尾线性地址 */
	PROVIDE(edata = .);

    /* (静态/未初始化/初始值为0)数据段 */
	.bss : {
		*(EXCLUDE_FILE(obj/kern/bootstrap.o) .bss)
	}
    /* 初始化 kern/init.c 中的 end[]，end 标识符代表*内核*的末尾线性地址  */
	PROVIDE(end = .);

	/* 从 Linux kernel 借来的... */
	/* 在内核内存中包含调试信息 */
        /* DWARF 1 */                                           
	.debug          0 : { *(.debug) }
        .line           0 : { *(.line) }
        /* GNU DWARF 1 extensions */
        .debug_srcinfo  0 : { *(.debug_srcinfo) }
        .debug_sfnames  0 : { *(.debug_sfnames) }
        /* DWARF 1.1 and DWARF 2 */
        .debug_aranges  0 : { *(.debug_aranges) }
        .debug_pubnames 0 : { *(.debug_pubnames) }
        /* DWARF 2 */
        .debug_info     0 : { *(.debug_info
        .gnu.linkonce.wi.*) }
        .debug_abbrev   0 : { *(.debug_abbrev) }
        .debug_line     0 : { *(.debug_line) }
        .debug_frame    0 : { *(.debug_frame) }
        .debug_str      0 : { *(.debug_str) }
        .debug_loc      0 : { *(.debug_loc) }
        .debug_macinfo  0 : { *(.debug_macinfo) }
        /* SGI/MIPS DWARF 2 extensions */ 
        .debug_weaknames 0 : { *(.debug_weaknames) }
        .debug_funcnames 0 : { *(.debug_funcnames) }
        .debug_typenames 0 : { *(.debug_typenames) }
        .debug_varnames  0 : { *(.debug_varnames) }

	/DISCARD/ : {
		*(.eh_frame .note.GNU-stack)
	}
}
