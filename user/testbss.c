// 测试读写一个大的.bss节

#include "inc/lib.h"

#define ARRAYSIZE (1024 * 1024)  // 1M

uint32_t bigarray[ARRAYSIZE];

void umain(int argc, char **argv)
{
	int i;

	cprintf("Ensure AlvOS has initialized .bss section correctly...\n");
	for (i = 0; i < ARRAYSIZE; i++)
		if (bigarray[i] != 0)
			panic("bigarray[%d] isn't cleared!\n", i);
	for (i = 0; i < ARRAYSIZE; i++)
		bigarray[i] = i;
	for (i = 0; i < ARRAYSIZE; i++)
		if (bigarray[i] != i)
			panic("bigarray[%d] didn't hold its value! \n", i);

	cprintf(".bss section is initialized correctly! \n");
	cprintf("Now try to assign to the space outside the array...\n");
	bigarray[ARRAYSIZE + 1024] = 0;
	// panic("应该导致异常！");
}
