
#include "inc/lib.h"

/*
 * 无法解决的致命错误会导致死机
 * 打印"死机:<消息>”，然后导致处理器休眠进入HLT状态
 */
void _panic(const char *file, int line, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);

	// 输出死机的信息
	cprintf("[%08x] user panic in %s at %s:%d: ",
			sys_getprocid(), binaryname, file, line);
	vcprintf(fmt, ap);
	cprintf("\n");

	// 触发断点中断
	while (1)
		asm volatile("int3");
}
