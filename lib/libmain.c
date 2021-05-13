// 从 lib/entry.S 调用并继续，entry.S 已经定义了 envs, pages, uvpd, uvpt.

#include "inc/lib.h"

extern void umain(int argc, char **argv);

const volatile struct Env *thisenv;
const char *binaryname = "<unknown>";

/**
 *  用户环境从lib/libmain.c中的函数libmain开始执行，这个函数进而调用umain，用户写的程序入口只能是umain
 */
void libmain(int argc, char **argv)
{
	// 设置常量 thisenv 指向 envs[] 中当前环境的 Env 结构，ENVX 确保环境 id 未超过了 NENV
	// thisenv 指向当前环境的 Env 结构
	thisenv = &envs[ENVX(sys_getenvid())];

	// 为了能让panic()提示用户错误，存储程序的名称
	if (argc > 0)
		binaryname = argv[0];

	// 调用用户的主程序
	umain(argc, argv);

	// 退出当前环境，env_destroy
	sys_env_destroy(0);
}
