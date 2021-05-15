
#include "inc/string.h"
#include "inc/lib.h"

void
cputchar(int ch)
{
	char c = ch;

	// 与标准的Unix的putchar不同，cputchar() 总是输出到系统控制台.
	sys_cputs(&c, 1);
}

int
getchar(void)
{
	int r;
	// sys_cgetc does not block, but getchar should.
	while ((r = sys_cgetc()) == 0)
		sys_yield();
	return r;
}
