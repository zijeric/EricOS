#ifndef ALVOS_KERN_SYSCALL_H
#define ALVOS_KERN_SYSCALL_H
#ifndef ALVOS_KERNEL
#endif

#include "inc/syscall.h"

int64_t syscall(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);

#endif /* !ALVOS_KERN_SYSCALL_H */
