// 基于printfmt()和内核控制台的cputchar(), 为AlvOS内核简单实现了cprintf控制台输出

#include "inc/types.h"
#include "inc/stdio.h"
#include "inc/stdarg.h"

/**
 * 输出一个字符在屏幕上
 * ch: 要输出的字符 [0, 7]:ASCII，[8, 15]:输出字符的格式，高16位未使用
 * cnt 只想要给整型变量的指针，每当 putch 函数输出一个字符就 cnt++.
 */
static void
putch(int ch, int *cnt)
{
	cputchar(ch);
	*cnt++;
}

/**
 * fmt: 显示字符串的指针，可变参数之前的固定参数
 * ap: 函数可变参数指针
 */
int vcprintf(const char *fmt, va_list ap)
{
	int cnt = 0;
	va_list aq;
	va_copy(aq, ap);
	// putch 作为函数指针，输出一个字符在屏幕上
	vprintfmt((void *)putch, &cnt, fmt, aq);
	va_end(aq);
	return cnt;
}

/**
 * 可变参函数
 * fmt: 显示字符串的指针，可变参数之前的固定参数
 * 函数参数都存放在内存的栈，从右至左依次 push 进栈，每个参数根据类型分配相应大小的空间
 */
int cprintf(const char *fmt, ...)
{
	// va_list 指向参数的指针
	va_list ap;
	int cnt;
	// 初始化 ap 变量，ap -> 可变参数1
	va_start(ap, fmt);
	va_list aq;
	// aq(ap) -> 可变参数1
	va_copy(aq, ap);
	cnt = vcprintf(fmt, aq);
	// 结束对可变参数的获取
	va_end(aq);

	return cnt;
}
