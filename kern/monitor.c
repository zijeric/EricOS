// 为了高效的控制内核和交互式地探索系统，参考 JOS 实现简单的命令行内核监视器

#include "inc/stdio.h"
#include "inc/string.h"
#include "inc/memlayout.h"
#include "inc/assert.h"
#include "inc/x86.h"

#include "kern/console.h"
#include "kern/monitor.h"
#include "kern/dwarf.h"
#include "kern/kdebug.h"
#include "kern/dwarf_api.h"


#define CMDBUF_SIZE	80	// VGA 文本行数的最大值(屏幕显示最大行数)

struct Command {
	const char *name;
	const char *desc;
	// 返回 -1 强制监视器退出
	int (*func)(int argc, char** argv, struct Trapframe* tf);
};

static struct Command commands[] = {
	{ "help", "Display this list of commands", mon_help },
	{ "kerninfo", "Display information about the kernel", mon_kerninfo },
	{ "backtrace", "Displays the backtrace information for debugging", mon_backtrace },
};
#define NCOMMANDS (sizeof(commands)/sizeof(commands[0]))

/************************* 基本内核监控命令的实现 *************************/

int
mon_help(int argc, char **argv, struct Trapframe *tf)
{
	int i;

	for (i = 0; i < NCOMMANDS; i++)
		cprintf("%s - %s\n", commands[i].name, commands[i].desc);
	return 0;
}

int
mon_kerninfo(int argc, char **argv, struct Trapframe *tf)
{
	extern char _start[], entry[], etext[], edata[], end[];

	cprintf("Special kernel symbols:\n");
	cprintf("  _start                  %08x (phys)\n", _start);
	cprintf("  entry  %08x (virt)  %08x (phys)\n", entry, entry - KERNBASE);
	cprintf("  etext  %08x (virt)  %08x (phys)\n", etext, etext - KERNBASE);
	cprintf("  edata  %08x (virt)  %08x (phys)\n", edata, edata - KERNBASE);
	cprintf("  end    %08x (virt)  %08x (phys)\n", end, end - KERNBASE);
	cprintf("Kernel executable memory footprint: %dKB\n",
		ROUNDUP(end - entry, 1024) / 1024);
	return 0;
}

/**
 * 实现过程:
 * 1.通过objdump命令,观察内核中不同的段
 * 2.objdump -h obj/kern/kernel
 *    需要注意.stab 和 .stabstr两段
 * 3.objdump -G obj/kern/kernel > stabs.txt
 *    由于显示内容较多,可以将结果输出到文件中
 *    文件(N_SO)和函数(N_FUN)项在.stab中按照指令地址递增的顺序组织
 * 4.根据eip 和 n_type(N_SO, N_SOL或N_FUN), 在.stab段中查找相应的Stab结构项(调用stab_binsearch)
 * 5.根据相应 Stab 结构的 n_strx 属性，找到其在.stabstr段中的索引，从该索引开始的字符串就是其对应的名字（文件名或函数名）
 * 6(*).根据 eip 和 n_type(N_SLINE)，在.stab段中找到相应的行号(n_desc字段)
 */ 
int
mon_backtrace(int argc, char **argv, struct Trapframe *tf)
{
	// 显示被调用函数的回溯.
	// rsp: stack pointer 栈指针(push/pop 操作), ebp: base pointer 当前函数栈的开头
	// rip: read_rip(rip)
	// 1. 利用 read_ebp() 函数获取当前ebp的值，read_rip(rip) 获取当前 rip 的值
	// 2. 利用 rbp 的初始值与0比较，判断是否停止(0:停止)
	// 3. 利用数组指针运算来获取 eip 以及 args 参数
	// 4. 利用 Eipdebuginfo 结构体的属性参数获取文件名信息
	uint64_t* rbp = (uint64_t*)read_rbp();
	uint64_t rip;
	read_rip(rip);
	int count = 0;
	cprintf("Stack backtrace:\n"); 
	while (rbp != 0) {// 到达函数调用的顶部时停止
		// 输出rbp和rip的当前值，然后取消引用先前的值
		cprintf("  rbp %#016x  rip %#016x\n", (uint64_t)rbp, rip);
		struct Ripdebuginfo info;
		if (debuginfo_rip(rip, &info) == 0) {
			// 检查结构是否已填充.
			cprintf("       %s:%d: %s+%#016x  args:%d", info.rip_file, info.rip_line, info.rip_fn_name, (uint64_t)rip-info.rip_fn_addr, info.rip_fn_narg);
			// 输出参数
			int args = info.rip_fn_narg;
			int argc = 1;
			while (args > 0) {
				cprintf("  %#016x", *(rbp-argc)>>32);
				args--;
				argc++;
			}
			cprintf("\n");
		}
		rip = *(rbp + 1);	
		rbp = (uint64_t*) *rbp;
		count++;
	}
	return count;
}

/************************* 内核监控命令解释器 *************************/

#define WHITESPACE "\t\r\n "
#define MAXARGS 16

static int
runcmd(char *buf, struct Trapframe *tf)
{
	int argc;
	char *argv[MAXARGS];
	int i;

	// 将命令缓冲区解析为以空格分隔的参数
	argc = 0;
	argv[argc] = 0;
	while (1) {
		// 清除空白(\t\r\n)
		while (*buf && strchr(WHITESPACE, *buf))
			*buf++ = 0;
		if (*buf == 0)
			break;

		// 保存并扫描下一个参数
		if (argc == MAXARGS-1) {
			cprintf("Too many arguments (max %d)\n", MAXARGS);
			return 0;
		}
		argv[argc++] = buf;
		while (*buf && !strchr(WHITESPACE, *buf))
			buf++;
	}
	argv[argc] = 0;

	// 查找并调用命令
	if (argc == 0)
		return 0;
	for (i = 0; i < NCOMMANDS; i++) {
		if (strcmp(argv[0], commands[i].name) == 0)
			return commands[i].func(argc, argv, tf);
	}
	cprintf("Unknown command '%s'\n", argv[0]);
	return 0;
}

void
monitor(struct Trapframe *tf)
{
	char *buf;

	cprintf("Welcome to the AlvOS kernel monitor!\n");
	cprintf("Type 'help' for a list of commands.\n");


	while (1) {
		buf = readline("K> ");
		if (buf != NULL)
			if (runcmd(buf, tf) < 0)
				break;
	}
}

