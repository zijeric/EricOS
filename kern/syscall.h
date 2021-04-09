#ifndef ALVOS_KERN_SYSCALL_H
#define ALVOS_KERN_SYSCALL_H
#ifndef ALVOS_KERNEL
#error "This is a AlvOS kernel header; user programs should not #include it"
#endif

#include "inc/syscall.h"

int64_t syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);

#endif
