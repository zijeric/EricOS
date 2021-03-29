#ifndef ALVOS_INC_ASSERT_H
#define ALVOS_INC_ASSERT_H

#include "inc/stdio.h"

void _panic(const char *, int, const char *, ...) __attribute__((noreturn));

#define panic(...) _panic(__FILE__, __LINE__, __VA_ARGS__)

#define assert(x)                              \
	do                                         \
	{                                          \
		if (!(x))                              \
			panic("assertion failed: %s", #x); \
	} while (0)

// 如果'x'为false，则 static_assert(x) 将在生成编译时产生错误.
#define static_assert(x) \
	switch (x)           \
	case 0:              \
	case (x):

#endif /* !ALVOS_INC_ASSERT_H */
