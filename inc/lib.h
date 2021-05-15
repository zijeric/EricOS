// 用户态支持库的主要公共头文件，其代码位于lib目录中.
// 为了链接到所有用户态应用程序(而不是内核或bootloader)，需要定义 lib.h 为内核的标准C库.

#ifndef ALVOS_INC_LIB_H
#define ALVOS_INC_LIB_H 1

#include "inc/types.h"
#include "inc/stdio.h"
#include "inc/stdarg.h"
#include "inc/string.h"
#include "inc/error.h"
#include "inc/assert.h"
#include "inc/env.h"
#include "inc/memlayout.h"
#include "inc/syscall.h"
#include "inc/trap.h"

#define USED(x) (void)(x)

// 主用户程序
void umain(int argc, char **argv);

// libmain.c or entry.S
extern const char *binaryname;
extern const volatile struct Env *thisproc;
extern const volatile struct Env procs[NENV];
extern const volatile struct PageInfo pages[];

// exit.c

void exit(void);

// pgfault.c

void set_pgfault_handler(void (*handler)(struct UTrapframe *utf));

// readline.c

char *readline(const char *buf);

// syscall.c

void sys_cputs(const char *string, size_t len);
int sys_cgetc(void);
envid_t sys_getprocid(void);
int sys_env_destroy(envid_t);
void sys_yield(void);
static envid_t sys_exofork(void);
int sys_env_set_status(envid_t env, int status);
int sys_env_set_trapframe(envid_t env, struct Trapframe *tf);
int sys_env_set_pgfault_upcall(envid_t env, void *upcall);
int sys_page_alloc(envid_t env, void *pg, int perm);
int sys_page_map(envid_t src_env, void *src_pg,
				 envid_t dst_env, void *dst_pg, int perm);
int sys_page_unmap(envid_t env, void *pg);
int sys_ipc_try_send(envid_t to_env, uint64_t value, void *pg, int perm);
int sys_ipc_recv(void *rcv_pg);

// 必须内联.
static __inline envid_t __attribute__((always_inline))
sys_exofork(void)
{
	envid_t ret;
	__asm __volatile("int %2"
					 : "=a"(ret)
					 : "a"(SYS_exofork),
					   "i"(T_SYSCALL));
	return ret;
}

// ipc.c
void ipc_send(envid_t to_env, uint32_t value, void *pg, int perm);
int32_t ipc_recv(envid_t *from_env_store, void *pg, int *perm_store);
envid_t ipc_find_env(enum EnvType type);

// fork.c

#define PTE_SHARE 0x400
envid_t fork(void);

#endif
