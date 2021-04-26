// 父子环境之间的测试对话
// Contributed by Varun Agrawal at Stony Brook

#include "inc/lib.h"

const char *str1 = "hello child environment! how are you?";
const char *str2 = "hello parent environment! I'm good.";

#define TEMP_ADDR	((char*)0xa00000)
#define TEMP_ADDR_CHILD	((char*)0xb00000)

void
umain(int argc, char **argv)
{
	envid_t who;

	if ((who = fork()) == 0) {
		// Child
		// who: from_env, 父进程ID(在sys_ipc_try_send更改)
		// 将发送进程的物理页映射到 TEMP_ADDR_CHILD
		// 进入接受阻塞态，让出CPU
		ipc_recv(&who, TEMP_ADDR_CHILD, 0);
		cprintf("%x got message : %s\n", who, TEMP_ADDR_CHILD);
		if (strncmp(TEMP_ADDR_CHILD, str1, strlen(str1)) == 0)
			cprintf("child received correct message\n");

		memcpy(TEMP_ADDR_CHILD, str2, strlen(str1) + 1);
		ipc_send(who, 0, TEMP_ADDR_CHILD, PTE_P | PTE_W | PTE_U);
		return;
	}

	// Parent
	// 分配一页物理内存，并将其以 perm 权限映射 envid 环境 va 所对应的一页地址空间
	sys_page_alloc(thisenv->env_id, TEMP_ADDR, PTE_P | PTE_W | PTE_U);
	// 复制 str1 到 TEMP_ADDR
	memcpy(TEMP_ADDR, str1, strlen(str1) + 1);
	// who: 子进程ID，以 perm 传递 TEMP_ADDR 一页物理页到 who 对应的环境
	ipc_send(who, 0, TEMP_ADDR, PTE_P | PTE_W | PTE_U);

	ipc_recv(&who, TEMP_ADDR, 0);
	cprintf("%x got message : %s\n", who, TEMP_ADDR);
	if (strncmp(TEMP_ADDR, str2, strlen(str2)) == 0)
		cprintf("parent received correct message\n");
	return;
}

