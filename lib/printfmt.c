// 精简的 printf 格式的例程, 通常由 printf, sprintf, fprintf 等使用
// 内核和用户程序都使用这段代码

#include "inc/types.h"
#include "inc/stdio.h"
#include "inc/string.h"
#include "inc/stdarg.h"
#include "inc/error.h"

/*
 * 只支持数字格式的空格或零填充和字段宽度.
 *
 * 特殊格式 %e 采用一个整数错误代码并打印一个描述错误的字符串.
 * 整数可以是正数也可以是负数，因此 -E_NO_MEM 和 E_NO_MEM 是等价的.
 */

static const char *const error_string[MAXERROR] = {
		[E_UNSPECIFIED] = "unspecified error",
		[E_BAD_ENV] = "bad environment",
		[E_INVAL] = "invalid parameter",
		[E_NO_MEM] = "out of memory",
		[E_NO_FREE_ENV] = "out of environments",
		[E_FAULT] = "segmentation fault",
		[E_IPC_NOT_RECV] = "env is not recving",
		[E_EOF] = "unexpected end of file",
		[E_NO_DISK] = "no free space on disk",
		[E_MAX_OPEN] = "too many files are open",
		[E_NOT_FOUND] = "file or block not found",
		[E_BAD_PATH] = "invalid path",
		[E_FILE_EXISTS] = "file already exists",
		[E_NOT_EXEC] = "file is not a valid executable",
		[E_NOT_SUPP] = "operation not supported",
};

/*
 * 使用指定的 putch 函数和关联的指针 putdat 倒序打印一个数字(base <= 16)
 * num: 需要打印出来的整型数，base: 整型数的进制，其他参数同 vprintfmt
 */
static void
printnum(void (*putch)(int, void *), void *putdat,
		 unsigned long long num, unsigned base, int width, int padc)
{
	// 当 num 超过一位数，首先递归函数自身打印所有高位的(更重要的)数字
	if (num >= base)
	{
		printnum(putch, putdat, num / base, base, width - 1, padc);
	}
	else
	{
		// 按照右对齐的格式在左侧补齐填充字符 padc
		while (--width > 0)
			putch(padc, putdat);
	}

	// 然后打印个位数字
	putch("0123456789abcdef"[num % base], putdat);
}

// Get an unsigned int of various possible sizes from a varargs list,
// depending on the lflag parameter.
static unsigned long long
getuint(va_list *ap, int lflag)
{
	unsigned long long x;
	if (lflag >= 2)
		x = va_arg(*ap, unsigned long long);
	else if (lflag)
		x = va_arg(*ap, unsigned long);
	else
		x = va_arg(*ap, unsigned int);
	return x;
}

// Same as getuint but signed - can't use getuint
// because of sign extension
static long long
getint(va_list *ap, int lflag)
{
	long long x;
	if (lflag >= 2)
		x = va_arg(*ap, long long);
	else if (lflag)
		x = va_arg(*ap, long);
	else
		x = va_arg(*ap, int);
	return x;
}

// Main function to format and print a string.
void printfmt(void (*putch)(int, void *), void *putdat, const char *fmt, ...);

void vprintfmt(void (*putch)(int, void *), void *putdat, const char *fmt, va_list ap)
{
	register const char *p;
	register int ch, err;
	unsigned long long num;
	// width: 代表的一个字符串或一个数字在屏幕上所占的宽度
	// precision: 一个字符串在屏幕上应显示的长度，precision > width
	// lflag: 在输出数字时(不支持浮点数)，0: 视参数为int输出，1:视参数为long输出，2: 视参数为long long输出
	// altflag: 当lflag = 1，若输出乱码用'?'代替
	int base, lflag, width, precision, altflag;

	// 填充字符，在显示字符串的'%'后读到'-'或'0'时，赋值到 padc
	char padc;
	va_list aq;
	va_copy(aq, ap);
	while (1)
	{
		while ((ch = *(unsigned char *)fmt++) != '%')
		{
			if (ch == '\0')
				return;
			putch(ch, putdat);
		}

		// 处理 % 转义序列
		// padc 初始化为空格符，padc='-'代表字符串左对齐，右边补空格，padc=' '代表右对齐，左边补空格，padc='0'右对齐，左边补0
		padc = ' ';
		width = -1;
		// 默认-1代表显示长度为字符串本来的长度
		precision = -1;
		lflag = 0;
		altflag = 0;
	reswitch:
		switch (ch = *(unsigned char *)fmt++)
		{

		// flag to pad on the right
		case '-':
			padc = '-';
			goto reswitch;

		// flag to pad with 0's instead of spaces
		case '0':
			padc = '0';
			goto reswitch;

		// width field
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
			for (precision = 0;; ++fmt)
			{
				precision = precision * 10 + ch - '0';
				ch = *fmt;
				if (ch < '0' || ch > '9')
					break;
			}
			goto process_precision;

		case '*':
			precision = va_arg(aq, int);
			goto process_precision;

		case '.':
			if (width < 0)
				width = 0;
			goto reswitch;

		case '#':
			altflag = 1;
			goto reswitch;

		process_precision:
			if (width < 0)
				width = precision, precision = -1;
			goto reswitch;

		// long flag (doubled for long long)
		case 'l':
			lflag++;
			goto reswitch;

		// character
		case 'c':
			putch(va_arg(aq, int), putdat);
			break;

		// error message
		case 'e':
			err = va_arg(aq, int);
			if (err < 0)
				err = -err;
			if (err >= MAXERROR || (p = error_string[err]) == NULL)
				printfmt(putch, putdat, "error %d", err);
			else
				printfmt(putch, putdat, "%s", p);
			break;

		// string
		case 's':
			// 从可变参数中读入字符串指针
			if ((p = va_arg(aq, char *)) == NULL)
				p = "(null)"; // 当字符串为空时，将它指向"(null)"字符串
			// 判断左对齐还是右对齐，pad='-'左对齐，否则右对齐
			if (width > 0 && padc != '-')
				for (width -= strnlen(p, precision); width > 0; width--)
					putch(padc, putdat); // 字符串右对齐，左边补相应数量(width-实际长度)的空格或者 0
			for (; (ch = *p++) != '\0' && (precision < 0 || --precision >= 0); width--)
				if (altflag && (ch < ' ' || ch > '~'))
					putch('?', putdat);
				else
					putch(ch, putdat); // 打印相应长度的字符串
			for (; width > 0; width--)
				putch(' ', putdat); // 当字符串是左对齐的时候打印相应数量的空格
			break;

		// (signed) decimal
		case 'd':
			num = getint(&aq, 3);
			if ((long long)num < 0)
			{
				putch('-', putdat);
				num = -(long long)num;
			}
			base = 10;
			goto number;

		// unsigned decimal
		case 'u':
			num = getuint(&aq, 3);
			base = 10;
			goto number;

		// (unsigned) octal
		case 'o':
			// Gets the variable argument with type integer from the point which was processed last by va_arg
			// calls getuint which returns a long long value, which then calls printnum which prints based on
			// base value.
			num = getuint(&aq, 3);
			base = 8;
			goto number;

		// pointer
		case 'p':
			putch('0', putdat);
			putch('x', putdat);
			num = (unsigned long long)(uintptr_t)va_arg(aq, void *);
			base = 16;
			goto number;

		// (unsigned) hexadecimal
		case 'x':
			num = getuint(&aq, 3);
			base = 16;
		number:
			printnum(putch, putdat, num, base, width, padc);
			break;

		// escaped '%' character
		case '%':
			putch(ch, putdat);
			break;

		// unrecognized escape sequence - just print it literally
		default:
			putch('%', putdat);
			for (fmt--; fmt[-1] != '%'; fmt--)
				/* do nothing */;
			break;
		}
	}
	va_end(aq);
}

void printfmt(void (*putch)(int, void *), void *putdat, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintfmt(putch, putdat, fmt, ap);
	va_end(ap);
}

struct sprintbuf
{
	char *buf;
	char *ebuf;
	int cnt;
};

static void
sprintputch(int ch, struct sprintbuf *b)
{
	b->cnt++;
	if (b->buf < b->ebuf)
		*b->buf++ = ch;
}

int vsnprintf(char *buf, int n, const char *fmt, va_list ap)
{
	va_list aq;
	va_copy(aq, ap);
	struct sprintbuf b = {buf, buf + n - 1, 0};

	if (buf == NULL || n < 1)
		return -E_INVAL;

	// print the string to the buffer
	vprintfmt((void *)sprintputch, &b, fmt, aq);
	va_end(aq);
	// null terminate the buffer
	*b.buf = '\0';

	return b.cnt;
}

int snprintf(char *buf, int n, const char *fmt, ...)
{
	va_list ap;
	int rc;
	va_list aq;
	va_start(ap, fmt);
	va_copy(aq, ap);
	rc = vsnprintf(buf, n, fmt, aq);
	va_end(aq);

	return rc;
}
