// 为了高效的控制内核和交互式地探索系统，参考 MIT OS 实现简单的命令行内核监视器

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
#include "kern/trap.h"

#define CMDBUF_SIZE 80 // enough for one VGA text line

struct Command
{
	const char *name; // 命令名字
	const char *desc; // 命令作用
	// 命令相关函数，返回 -1 强制监视器退出
	int (*func)(int argc, char **argv, struct Trapframe *tf);
};

static struct Command commands[] = {
	{"help", "Display this list of commands", mon_help},
};
#define NCOMMANDS (sizeof(commands) / sizeof(commands[0]))

/************************* 基本内核监控命令的实现 *************************/

int mon_help(int argc, char **argv, struct Trapframe *tf)
{
	int i;

	for (i = 0; i < NCOMMANDS; i++)
		cprintf("%s - %s\n", commands[i].name, commands[i].desc);
	return 0;
}

/************************* 内核监控命令解释器 *************************/

#define WHITESPACE "\t\r\n "
#define MAXARGS 16

/**
 * 处理命令字符串
 * buf: 指向命令字符串的指针，tf: 当前状态的陷阱帧
 */
static int
runcmd(char *buf, struct Trapframe *tf)
{
	// 命令参数个数
	int argc;
	// 指针数组，每个数组项指向一个子字符串(命令名字、命令参数)
	char *argv[MAXARGS];
	int i;

	// 将命令缓冲区解析为以空格分隔的参数
	argc = 0;
	argv[argc] = 0;
	while (1)
	{
		// 将所有空白(\t\r\n ) 制表符、换行符、空格 都置为空字符
		while (*buf && strchr(WHITESPACE, *buf))
			*buf++ = 0;
		if (*buf == 0)
			break; // 命令结束

		// 保存并扫描下一个参数
		if (argc == MAXARGS - 1)
		{
			cprintf("Too many arguments (max %d)\n", MAXARGS);
			return 0; // 参数个数超过最大个数限制 MAXARGS: 16
		}
		// 指向相应的字符串
		argv[argc++] = buf;
		// 跳过非空格的字符
		while (*buf && !strchr(WHITESPACE, *buf))
			buf++;
	}
	// argc = n + 1
	argv[argc] = 0;
	/**
	 * 以上是让指针指向了每个子字符串并且把命令字符串的空格替换为空字符
	 * 因为命令字符串的命令名、参数彼此之间都有空格相隔
	 * 处理后每个子字符串的结尾都是一个空字符'\0'
	 * 
	 * argv[0]		argv[1]		...
	 * 👇			 👇
	 * +------+------+------+------+------+------+
	 * |命令名 | '\0' | 参数1 | '\0' | 参数n | '\0' |...
	 * +------+------+------+------+------+------+
	 */

	// 查找并调用命令函数处理相应的命令
	if (argc == 0)
		return 0; // 没有命令则返回

	// 在所有可执行的命令中寻找与输入的命令名相同的命令，并将 argc 与 argv 当作命令函数的参数
	for (i = 0; i < NCOMMANDS; i++)
	{
		if (strcmp(argv[0], commands[i].name) == 0)
			return commands[i].func(argc, argv, tf);
	}
	// 无法识别的命令名
	cprintf("Unknown command '%s'\n", argv[0]);
	return 0;
}

void monitor(struct Trapframe *tf)
{
	char *buf;

	cprintf("Welcome to the AlvOS kernel monitor!\n");
	cprintf("Type 'help' for a list of commands.\n");

	if (tf != NULL)
		print_trapframe(tf);

	while (1)
	{
		// readline 等待用户输入一个命令字符串，"回车"代表命令行结束
		// buf 指向输入的字符串存放的位置
		buf = readline("A> ");
		if (buf != NULL)
			// 处理命令
			if (runcmd(buf, tf) < 0)
				break;
	}
}

