/**
 * 与Unix进程相类似，AlvOS 环境结合了线程(thread)和地址空间(address space)的概念：
 * Unix的线程主要通过保存寄存器的值(env_tf)来定义，地址空间主要通过保存页目录和页表页(eng_pgdir)来定义
 *
 * 要运行一个环境，内核必须为它设置合适的寄存器的值和合适的地址空间
 * 在 AlvOS 系统中，任意时刻只能有一个环境处于活跃状态，因此 AlvOS 内核只需要一个内核栈
 */
#ifndef ALVOS_INC_ENV_H
#define ALVOS_INC_ENV_H

#include "inc/types.h"
#include "inc/trap.h"
#include "inc/memlayout.h"

/**
 * 一个环境ID: envid_t 有三个部分:
 * 
 * +1+---------------21-----------------+--------10--------+
 * |0|          Uniqueifier             |   Environment    |
 * | |                                  |      Index       |
 * +------------------------------------+------------------+
 *                                       \--- ENVX(eid) --/
 * 环境索引 ENVX(eid) 是 envs[] 数组中的环境索引，可以调用来获取任意环境
 * Uniqueifier 用于区分在不同时间创建的环境，但是共享相同的环境索引
 * 
 * 所有实际环境的
 * 1.envid_t > 0  (因此符号位为零)
 * 2.envid_t < 0: 环境错误
 * 3.envid_t == 0: 当前的环境(特殊)
 */
typedef int32_t envid_t;
extern pml4e_t *boot_pml4e;
extern physaddr_t boot_cr3;

// 1<<10 = 2^10 = 1024，系统运行环境最多容纳 1024 个环境，即环境并发容量
#define LOG2NENV 10
#define NENV (1 << LOG2NENV)
// 为了限制内核只能运行NENV个环境，(envid) & (NENV - 1)
#define ENVX(envid) ((envid) & (NENV - 1))
// 用户的DPL
#define DPL_USER		3

// struct Env 中 env_status 属性的值
enum
{
	// Env 处于空闲状态，存在于 env_free_list 链表
	ENV_FREE = 0,

	// Env 是僵尸环境，将在下一次陷入内核时被释放
	ENV_DYING,

	// Env 处于就绪态
	ENV_RUNNABLE,

	// Env 是当前正在运行的环境
	ENV_RUNNING,

	// Env 是当前正在运行的环境，但却没有准备好运行
	ENV_NOT_RUNNABLE
};

// 特殊环境类型
enum EnvType
{
	ENV_TYPE_USER = 0,
};

/**
 * Env 结构体存储环境的状态信息
 * Env 综合了Unix的线程和地址空间，线程由 env_tf 的环境帧定义，地址空间由 env_pgdir 指向的页目录和页表定义
 * 为了运行一个环境，内核必须用存储的环境帧，以及合适的地址空间设置 CPU
 * 
 * struct Env和xv6的struct proc很像，两种结构体都持有环境的用户模式寄存器状态（通过struct TrapFrame），
 * 因为AlvOS内核中同时只能有一个运行环境，因此AlvOS只需要一个内核栈
 */
struct Env
{
	// 环境帧(环境寄存器, 参考AMD64手册)的一个暂存区，环境挂起时暂存环境帧，环境恢复时重新载入(栈)
	// eg. 用户态与内核态之间的切换，或者环境调度的切换，都需要存储当前环境帧，以便后续该环境恢复执行
	struct Trapframe env_tf;

	// 索引下一个空闲的 Env 结构，指向空闲环境链表 env_free_list 中的下一个 Env 结构
	struct Env *env_link;

	/**
	 * +1+---------------21-----------------+--------10--------+
	 * |0|          Uniqueifier             |   Environment    |
	 * | |                                  |      Index       |
	 * +------------------------------------+------------------+
	 *                                       \--- ENVX(eid) --/
	 * 当前环境 Env 的 ID. 因为环境ID是正数，所以符号位是0，而中间的21位是标识符，
	 * 标识在不同时间创建但是却共享同一个环境索引号的环境
	 * 最后10位是环境的索引号，要用envs索引环境管理结构 Env 就要用 ENVX(env_id)
	 * 环境恢复运行依赖于 env_id.
	 */
	envid_t env_id;

	// 创建 当前环境 的环境的env_id，通过该方式构建一个环境树，用于安全方面的决策
	envid_t env_parent_id;

	// 用于区分特殊环境，对于大部分环境，该值为ENV_TYPE_USER
	enum EnvType env_type;

	// 当前环境的状态
	unsigned env_status;
	// ENV_FREE - struct Env环境处于空闲状态，位于env_free_list中
	// ENV_RUNNABLE - struct Env代表的环境正在等待运行于 CPU 上
	// ENV_RUNNING - struct Env代表的环境为正在运行的环境
	// ENV_NOT_RUNNABLE - struct Env代表了一个正在运行的环境，但却没有准备好运行
	// ENV_DYING - struct Env代表了一个僵尸环境，僵尸环境将在下一次陷入内核时被释放

	// 环境运行的次数
	uint32_t env_runs;

	// 正在运行环境的 CPU
	int env_cpunum;

	// 存储地址空间
	// 用于保存环境pml4的*虚拟地址空间*
	pml4e_t *env_pml4e;
	physaddr_t env_cr3;

	// Exception handling
	void *env_pgfault_upcall; // Page fault upcall entry point

	// IPC
	bool env_ipc_recving;	// Env is blocked receiving
	void *env_ipc_dstva;	// VA at which to map received page
	uint32_t env_ipc_value; // Data value sent to us
	envid_t env_ipc_from;	// envid of the sender
	int env_ipc_perm;		// Perm of page mapping received
	uint8_t *elf;
};

#endif /* !ALVOS_INC_ENV_H */
