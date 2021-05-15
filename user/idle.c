// 空闲循环

#include "inc/x86.h"
#include "inc/lib.h"

void umain(int argc, char **argv)
{
	binaryname = "idle";

	// 死循环，简单地试图放弃不同的进程
	// 真正实现时，与其像这样忙着等待，不如使用处理器的HLT指令使处理器停止执行，
	// 直到下一个中断唤醒 —— 这样做可以使处理器更有效地节能
	while (1)
	{
		sys_yield();
	}
}
