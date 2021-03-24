/**
 * 解决变参问题的宏
 */

#ifndef ALVOS_INC_STDARG_H
#define	ALVOS_INC_STDARG_H

// va_list 型变量，指向参数的指针 argument pointer
typedef __builtin_va_list va_list;

// va_init 宏初始化一个 va_list 变量，
// ap: va_list 型变量，last: 可变的参数的前一个参数
#define va_start(ap, last) __builtin_va_start(ap, last)

// va_arg 返回可变的参数，
// ap: va_list 型变量，type: 想要返回的类型
// va_arg每次是以地址往后增长取出下一个参数变量的地址。默认编译器是以从右往左的顺序将参数入栈的。
// 因为栈是从高往低的方向增长的。后压栈的参数放在了内存的低地址，所以如果要从左往右的顺序依次取出每个变量，
// 那么编译器必须以相反的顺序即从右往左将参数压栈。(或给定参数的个数)
#define va_arg(ap, type) __builtin_va_arg(ap, type)

// va_end 结束可变参数的获取
#define va_end(ap) __builtin_va_end(ap)

#define va_copy(d,s)    __builtin_va_copy(d,s)

#endif	/* !ALVOS_INC_STDARG_H */
