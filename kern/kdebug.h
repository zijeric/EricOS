#ifndef ALVOS_KERN_KDEBUG_H
#define ALVOS_KERN_KDEBUG_H

#include "inc/types.h"
#include "kern/dwarf.h"

// 关于 RIP 指令指针的调试信息
struct Ripdebuginfo
{
	const char *rip_file; // RIP 的源代码文件名
	int rip_line;		  // RIP 的源代码行数

	const char *rip_fn_name; // 包含 RIP 的函数名称
		//  - 注意，不能以 NULL 结尾！
	int rip_fn_namelen;		  // 函数名的长度
	uintptr_t rip_fn_addr;	  // 函数的基址
	int rip_fn_narg;		  // 函数参数的数量
	int size_fn_arg[10];	  // 函数中每个参数的大小
	Dwarf_Regtable reg_table; // DWARF 寄存器表
};

int debuginfo_rip(uintptr_t rip, struct Ripdebuginfo *info);

#endif
