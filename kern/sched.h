#ifndef ALVOS_KERN_SCHED_H
#define ALVOS_KERN_SCHED_H
#ifndef ALVOS_KERNEL
# error "This is a AlvOS kernel header; user programs should not #include it"
#endif

// 此函数不会返回.
void sched_yield(void) __attribute__((noreturn));

#endif
