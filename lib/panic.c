#include "inc/lib.h"

/*
 * 在遇到无法解决的致命错误时调用 panic.
 * 输出"panic: <message>"，然后引起断点异常，从而导致 AlvOS 进入内核监视器.
 */
void _panic(const char *file, int line, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);

	// Print the panic message
	cprintf("[%08x] user panic in %s at %s:%d: ",
			sys_getenvid(), binaryname, file, line);
	vcprintf(fmt, ap);
	cprintf("\n");

	// Cause a breakpoint exception
	while (1)
		asm volatile("int3");
}
