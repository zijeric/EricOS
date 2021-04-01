/* See COPYRIGHT for copyright information. */

#ifndef ALVOS_KERN_SCHED_H
#define ALVOS_KERN_SCHED_H
#ifndef ALVOS_KERNEL
#endif

// This function does not return.
void sched_yield(void) __attribute__((noreturn));

#endif // !ALVOS_KERN_SCHED_H
