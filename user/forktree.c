// Fork 进程的二叉树结构并输出结构.

#include "inc/lib.h"

// 树结构的深度
#define DEPTH 10

void forktree(const char *cur);

void forkchild(const char *cur, char branch)
{
	char nxt[DEPTH + 1];

	// 字符串的长度 = 树结构深度就结束
	if (strlen(cur) >= DEPTH)
		return;

	snprintf(nxt, DEPTH + 1, "%s%c", cur, branch);
	if (fork() == 0)
	{
		forktree(nxt);
		exit();
	}
}

void forktree(const char *cur)
{
	// 输出进程的树结构
	cprintf("%04x: I am '%s'\n", sys_getprocid(), cur);

	// fork为二叉树
	forkchild(cur, '0');
	forkchild(cur, '1');
}

void umain(int argc, char **argv)
{
	forktree("");
}
