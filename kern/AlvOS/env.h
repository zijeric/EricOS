/* See COPYRIGHT for copyright information. */

#ifndef ALVOS_KERN_ENV_H
#define ALVOS_KERN_ENV_H

#include "inc/env.h"
#include "cpu.h"

// kern/env.c 中定义的 envs[NENV]
extern struct Env *envs;
// 当前运行环境
#define curenv (thiscpu->cpu_env)
extern struct Segdesc gdt[];

void env_init(void);
void env_init_percpu(void);
int env_alloc(struct Env **e, envid_t parent_id);
void env_free(struct Env *e);
void env_create(uint8_t *binary, enum EnvType type);
void env_destroy(struct Env *e);

int envid2env(envid_t envid, struct Env **env_store, bool checkperm);
void env_run(struct Env *e) __attribute__((noreturn));
void env_pop_tf(struct Trapframe *tf) __attribute__((noreturn));

// Without this extra macro, we couldn't pass macros like TEST to
// ENV_CREATE because of the C pre-processor's argument prescan rule.
#define ENV_PASTE3(x, y, z) x##y##z

#define ENV_CREATE(x, type)                                   \
	do                                                        \
	{                                                         \
		extern uint8_t ENV_PASTE3(_binary_obj_, x, _start)[]; \
		env_create(ENV_PASTE3(_binary_obj_, x, _start),       \
				   type);                                     \
	} while (0)

#endif // !ALVOS_KERN_ENV_H
