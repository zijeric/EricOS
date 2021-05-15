#ifndef ALVOS_KERN_ENV_H
#define ALVOS_KERN_ENV_H

#include "inc/env.h"
#include "kern/cpu.h"

// kern/env.c 中定义的 procs[NENV]
extern struct Env *procs;
// 当前运行环境
#define curenv (thiscpu->cpu_env)
extern struct Segdesc gdt[];

void env_init(void);
void env_init_percpu(void);
int env_alloc(struct Env **e, envid_t parent_id);
void env_free(struct Env *e);
void create_proc(uint8_t *binary, enum EnvType type);
// if e == curenv, 不返回
void env_destroy(struct Env *e);

int envid2env(envid_t envid, struct Env **env_store, bool checkperm);
// 接下来两个函数不返回
void env_run(struct Env *e) __attribute__((noreturn));
void env_pop_tf(struct Trapframe *tf) __attribute__((noreturn));

// 没有这个宏，由于 C 预处理器的参数预扫描规则，无法将的宏传递给 create_proc.
#define PROC_PASTE3(x, y, z) x##y##z

#define CREATE_PROC(x)                                   \
	do                                                        \
	{                                                         \
		extern uint8_t PROC_PASTE3(_binary_obj_user_, x, _start)[]; \
		create_proc(PROC_PASTE3(_binary_obj_user_, x, _start),       \
				   PROC_TYPE_USER);                                     \
	} while (0)

#endif
