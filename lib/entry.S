#include "inc/mmu.h"
#include "inc/memlayout.h"

.data
	// 定义全局符号'envs'、'pages'、'uvpt'、'uvpd'、'uvpde'、'uvpml4e'
	// 这样它们就可以像普通全局数组一样在 C 程序中使用
	// uvpt: 0x80000000(2GB), uvpd: 0x400000(4MB), uvpde: 0x2000(8KB), uvpml4e: 0x1000(4KB)
	.globl envs
	.set envs, UENVS
	.globl pages
	.set pages, UPAGES
	.globl uvpt
	.set uvpt, UVPT
	.globl uvpd
	.set   uvpd, uvpt + 0x80000000
	.globl  uvpde
	.set    uvpde, uvpd + 0x400000
	.globl  uvpml4e
	.set    uvpml4e, uvpde + 0x2000


// Entrypoint - 当最初被加载到一个新的环境中时，内核(或者父环境)在这里开始运行.
.text
.globl _start
_start:
	// 判断是否从栈上的参数开始初始化
	movabs $USTACKTOP, %rax
	cmpq %rax,%rsp
	// $USTACKTOP != %rsp 说明有参数，跳转到 args_exist
	jne args_exist

	// 如果不是(新环境被内核加载的时候)，则 push 伪造的 argc, argv 参数
	pushq $0
	pushq $0

args_exist:
	movq 8(%rsp), %rsi
	movq (%rsp), %rdi
	call libmain

// 不应该返回
1:	jmp 1b

