// 用户态IPC库程序

#include "inc/lib.h"

/**
 * 通过 IPC 接收一个值(或物理页)并返回值
 * 如果'pg'为非null，则发送进程发送的任何物理页都将映射到该地址
 * 如果'from_env_store'不为空，则将 IPC 发送进程的envid存储在*from_env_store中
 * 如果'perm_store'不为null，则将IPC发送者的页面权限存储在*perm_store中(如果页面被成功传输到'pg'，则此权限为非零)
 * 如果系统调用失败，那么将0存储在*fromenv和*perm中(如果它们是非null)并返回错误
 * 否则，返回发送方发送的值
 * 
 * 使用‘thisenv’来发现值和发送进程
 * 如果 pg 为空，则传递 sys_ipc_recv 一个值，它将理解为“no page”的意思
 * (Zero 不是正确的值，因为这是映射页面的完美有效位置)
 */
int32_t
ipc_recv(envid_t *from_env_store, void *pg, int *perm_store)
{
	int r = 0;
	if(pg) {
		r = sys_ipc_recv(pg);
	}
	// pg == 0, pg > UTOP
	else {
		r = sys_ipc_recv((void*)KERNBASE);
	}
	if (r < 0) {
		*from_env_store =  (from_env_store != NULL) ? (envid_t)0 : *from_env_store;
		*perm_store = (perm_store != NULL) ? (int)0 : *perm_store;
		return r;
	}
	// 设置from_env_store为发送进程的进程ID
	if(from_env_store) {
		*from_env_store = thisenv->env_ipc_from;
	}
	// 如果传递物理页，该位不为0，设置perm_store为物理页的权限
	if(perm_store) {
		*perm_store = thisenv->env_ipc_perm;
	}
	// 返回发送进程发送的值
	return thisenv->env_ipc_value;
}

/**
 * 发送'val'(和'pg'带'perm'，如果'pg'是非null)给'toenv'
 * 这个函数一直尝试，直到成功
 * 除了-E_IPC_NOT_RECV之外的任何错误都应该panic()
 * 
 * 使用sys_yield()使cpu友好
 * 如果'pg'为null，则给sys_ipc_recv传递一个它会理解为“无页面”的值(0不是正确的值)
 */
void
ipc_send(envid_t to_env, uint32_t val, void *pg, int perm)
{
	int r = -E_IPC_NOT_RECV;
	// 子进程不是接收状态
	while(r == -E_IPC_NOT_RECV) {
		if(pg) {
			// 传递物理页
			r = sys_ipc_try_send(to_env, val, pg, perm);
		}
		// pg == 0, pg > UTOP
		else {
			// (void*)KERNBASE > UTOP
			r = sys_ipc_try_send(to_env, val, (void*)KERNBASE, perm);
		}
		// 让出CPU
		sys_yield();
	}
	if (r != 0) {
		panic("something went wrong with sending the page");
	}
}

// Find the first environment of the given type.  We'll use this to
// find special environments.
// Returns 0 if no such environment exists.
envid_t
ipc_find_env(enum EnvType type)
{
	int i;
	for (i = 0; i < NENV; i++)
		if (envs[i].env_type == type)
			return envs[i].env_id;
	return 0;
}
