#ifndef ALVOS_INC_TYPES_H
#define ALVOS_INC_TYPES_H

#ifndef NULL
#define NULL ((void *)0)
#endif

// 布尔变量
typedef _Bool bool;
enum
{
	false,
	true
};

// 调整整数类型的大小(类型名称上可见)
typedef __signed char int8_t;
typedef unsigned char uint8_t;
typedef short int16_t;
typedef unsigned short uint16_t;
typedef int int32_t;
typedef unsigned int uint32_t;
typedef long long int64_t;
typedef unsigned long long uint64_t;

// 指针和地址长度为32位.
// 使用指针类型表示虚拟地址，uintptr_t 表示虚拟地址的数值，并使用 physaddr_t 表示物理地址.
// 虚拟地址长度为64位.
typedef int32_t intptr_t;
typedef uint64_t uintptr_t;
typedef uint64_t physaddr_t;

// size_t 用于表示内存对象的大小.
typedef uint64_t size_t;
// ssize_t 是 size_t 的有符号版本，用于防止可能出现错误返回.
typedef int32_t ssize_t;

// off_t 用于文件偏移和长度.
typedef int32_t off_t;

// 高效的比较最小和最大的操作
#define MIN(_a, _b)             \
	({                          \
		typeof(_a) __a = (_a);  \
		typeof(_b) __b = (_b);  \
		__a <= __b ? __a : __b; \
	})
#define MAX(_a, _b)             \
	({                          \
		typeof(_a) __a = (_a);  \
		typeof(_b) __b = (_b);  \
		__a >= __b ? __a : __b; \
	})

// 实际作用就是将 sz 向下取整成 PGSIZE 的倍数，如 sz=5369, PGSIZE=4096, 那么 addr=4096
#define ROUNDDOWN(a, n)               \
	({                                \
		uint64_t __a = (uint64_t)(a); \
		(typeof(a))(__a - __a % (n)); \
	})
// 实际作用就是将 sz 向上取整(对齐)成 PGSIZE 的倍数，如 sz=5369, PGSIZE=4096, 那么 addr=8192
#define ROUNDUP(a, n)                                         \
	({                                                        \
		uint64_t __n = (uint64_t)(n);                         \
		(typeof(a))(ROUNDDOWN((uint64_t)(a) + __n - 1, __n)); \
	})

// 返回 成员属性 相对于结构类型开头的偏移量
#define offsetof(type, member) ((size_t)(&((type *)0)->member))

#endif
