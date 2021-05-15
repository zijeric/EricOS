// 父子进程之间的通信测试
// 由用户程序创建子进程并发送物理页
// 父进程分配一个物理页 TEMP_PAGE，拷贝需要发送的字符串到该物理页，系统调用 Send 发送给子进程
// 子进程对比字符串是否正确，再发送一个物理页 TEMP_PAGE_CHILD 给父进程，父进程检验字符串
// 若父进程和子进程均对比字符串后检测正确，说明进程间通信正确

#include "inc/lib.h"

const char *str1 = "你好，子进程！写完毕业论文了吗？";
const char *str2 = "你好，父进程！还差亿点点。";

#define TEMP_PAGE ((char *)0xa00000)
#define TEMP_PAGE_CHILD ((char *)0xb00000)

void umain(int argc, char **argv)
{
	int32_t who;

	if ((who = fork()) == 0)
	{
		// Child
		// 子进程设置接收状态，并进入阻塞态让出CPU
		ipc_recv(&who, TEMP_PAGE_CHILD, 0);
		// 由父进程将物理页 TEMP_PAGE 映射到 TEMP_PAGE_CHILD
		cprintf("%x got message : %s\n", who, TEMP_PAGE_CHILD);
		// 对比字符串
		if (strncmp(TEMP_PAGE_CHILD, str1, strlen(str1)) == 0)
			cprintf("child received correct message\n");

		// 复制 str2 到 TEMP_PAGE_CHILD
		memcpy(TEMP_PAGE_CHILD, str2, strlen(str1) + 1);
		// who: 父进程ID，以用户可读写的权限传递 TEMP_PAGE_CHILD 一页物理页到父进程
		ipc_send(who, 0, TEMP_PAGE_CHILD, PTE_P | PTE_W | PTE_U);
		return;
	}

	// Parent
	// 为自己分配一页物理内存，并将其以用户可读写的权限映射到当前进程虚拟地址空间的TEMP_PAGE
	sys_page_alloc(thisproc->proc_id, TEMP_PAGE, PTE_P | PTE_W | PTE_U);
	// 复制 str1 到 TEMP_PAGE
	memcpy(TEMP_PAGE, str1, strlen(str1) + 1);
	// who: 子进程ID，以用户可读写的权限传递 TEMP_PAGE 一页物理页到子进程
	ipc_send(who, 0, TEMP_PAGE, PTE_P | PTE_W | PTE_U);

	// 父进程设置接收状态，并进入阻塞态让出CPU
	ipc_recv(&who, TEMP_PAGE, 0);
	cprintf("%x got message : %s\n", who, TEMP_PAGE);
	// 对比字符串
	if (strncmp(TEMP_PAGE, str2, strlen(str2)) == 0)
		cprintf("parent received correct message\n");
	return;
}
