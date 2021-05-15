// 触发通用保护性异常的用户程序

#include "inc/lib.h"

void umain(int argc, char **argv)
{
	// 尝试加载内核的数据段选择子到DS寄存器
	asm volatile("movw $0x10, %ax; movw %ax, %ds");
}
