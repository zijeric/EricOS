#include "inc/stdio.h"
#include "inc/string.h"
#include "inc/assert.h"
#include "inc/memlayout.h"

#include "kern/monitor.h"
#include "kern/console.h"
#include "kern/kdebug.h"
#include "kern/dwarf_api.h"

uint64_t end_debug;


// 测试栈回溯函数 mon_backtrace
void
test_backtrace(int x)
{
	cprintf("entering test_backtrace %d\n", x);
	if (x > 0)
		test_backtrace(x-1);
	else
		mon_backtrace(0, 0, 0);
	cprintf("leaving test_backtrace %d\n", x);
}

void
i386_init(void)
{
    /* __asm __volatile("int $12"); */

	extern char edata[], end[];

	// 在执行任何其他操作之前，必须先完成 ELF 加载过程.
	// 为了确保所有静态/全局变量初始值为 0，清除程序中未初始化的全局数据(BSS)部分.
	memset(edata, 0, end - edata);

	// 初始化控制台. 在此之后才能调用 cprintf()!
	cons_init();

	cprintf("6828 decimal is %o octal!\n", 6828);

    extern char end[];
    end_debug = read_section_headers((0x10000+KERNBASE), (uintptr_t)end); 


	// 测试栈回溯函数 mon_backtrace
	test_backtrace(5);

	// 陷入内核监视器.
	while (1)
		monitor(NULL);
}


/*
 * 变量 panicstr 包含第一次调用 panic 的参数; 用作标志表示内核已经调用 panic
 */
const char *panicstr;

/*
 * 遇到无法解决的致命错误时调用panic.
 * 打印"panic: mesg"，然后进入内核监视器.
 */
void
_panic(const char *file, int line, const char *fmt,...)
{
	va_list ap;

	if (panicstr)
		goto dead;
	panicstr = fmt;

	// 特别确保机器处于合理的状态
	__asm __volatile("cli; cld");

	va_start(ap, fmt);
	cprintf("kernel panic at %s:%d: ", file, line);
	vcprintf(fmt, ap);
	cprintf("\n");
	va_end(ap);

dead:
	/* 直接陷入内核监视器 */
	while (1)
		monitor(NULL);
}

