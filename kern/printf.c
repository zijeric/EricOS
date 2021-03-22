// 基于printfmt()和内核控制台的cputchar(), 为AlvOS内核简单实现了cprintf控制台输出

#include "inc/types.h"
#include "inc/stdio.h"
#include "inc/stdarg.h"


static void
putch(int ch, int *cnt)
{
	cputchar(ch);
	*cnt++;
}

int
vcprintf(const char *fmt, va_list ap)
{
	int cnt = 0;
    va_list aq;
    va_copy(aq,ap);
	vprintfmt((void*)putch, &cnt, fmt, aq);
    va_end(aq);
	return cnt;

}

int
cprintf(const char *fmt, ...)
{
	va_list ap;
	int cnt;
	va_start(ap, fmt);
    va_list aq;
    va_copy(aq,ap);
	cnt = vcprintf(fmt, aq);
	va_end(aq);

	return cnt;
}

