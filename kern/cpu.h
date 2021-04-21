// 用于支持多处理器的内核私有定义
// Per-CPU: 
// 1.Per-CPU kernel stack 多个CPU可以同时 trap 到内核
// 2.Per-CPU TSS and TSS descriptor 寻址Per-CPU的内核栈
// 3.Per-CPU current environment pointer 指向Per-CPU的当前环境
// 4.Per-CPU Registers Per-CPU的所有寄存器私有(彼此隔离)，Per-CPU都要运行初始化特殊寄存器指令
// 内核唯一：envs数组、pages数组、内核页目录
#ifndef ALVOS_INC_CPU_H
#define ALVOS_INC_CPU_H

#include "inc/types.h"
#include "inc/memlayout.h"
#include "inc/mmu.h"
#include "inc/env.h"

// CPU最大装载数量
#define NCPU  8

// Values of status in struct Cpu
enum {
	CPU_UNUSED = 0,
	CPU_STARTED,
	CPU_HALTED,
};

// per-CPU 的状态信息
struct CpuInfo {
	// local APIC ID; 索引到cpus[]
	uint8_t cpu_id;

	// CPU的状态
	volatile unsigned cpu_status;

	// 指向当前 CPU 运行的环境
	// 调用 env_run() 的时候更新，实际上是更新当前 CPU 执行的环境
	struct Env *cpu_env;

	// x86 TSS任务状态段用于寻位 Per-CPU 的内核栈
	// CPUi 的TSS存于cpus[i].cpu_ts中，相关联的TSS描述符定义在GDT入口gdt[(GD_TSS0 >> 3) + i]
	// 覆盖 kern/trap.c 定义的全局ts变量
	struct Taskstate cpu_ts;
};

// 在 mpconfig.c 被初始化
extern struct CpuInfo cpus[NCPU];
// 在系统中CPU的总数
extern int ncpu;
// 引导处理器BSP(boot-strap processor)
extern struct CpuInfo *bootcpu;
// local APIC(Advanced Programmable Interrupt Controller) 局部高级可编程中断处理器
// local APIC 负责在整个系统中传递中断，也是其所连接 cpu 的ID
// local APIC 的物理 MMIO 地址
extern physaddr_t lapicaddr;

// Per-CPU 的内核栈，隔离他们运行的程序(security)
extern unsigned char percpu_kstacks[NCPU][KSTKSIZE];

// 返回调用它的CPU的ID，可以被用作是cpus数组的索引
int cpunum(void);
// 当前 CPU 的 strcut CpuInfo 缩略表示，形似 Java 的 this 指针
#define thiscpu (&cpus[cpunum()])

void mp_init(void);
void lapic_init(void);
void lapic_startap(uint8_t apicid, uint32_t addr);
void lapic_eoi(void);
void lapic_ipi(int vector);

#endif
