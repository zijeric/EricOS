// 触发断点的用户程序

#include "inc/lib.h"

void umain(int argc, char **argv)
{
	asm volatile("int $3");
}
