#ifndef ALVOS_KERN_MONITOR_H
#define ALVOS_KERN_MONITOR_H
#ifndef ALVOS_KERNEL
# error "This is a AlvOS kernel header; user programs should not #include it"
#endif

struct Trapframe;

// 激活内核监视器，可以选择提供一个指示当前状态的陷阱帧(如果没有，则为 NULL).
void monitor(struct Trapframe *tf);

// 实现监视器命令的函数.
int mon_help(int argc, char **argv, struct Trapframe *tf);
int mon_kerninfo(int argc, char **argv, struct Trapframe *tf);
int mon_backtrace(int argc, char **argv, struct Trapframe *tf);

#endif
