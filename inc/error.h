#ifndef ALVOS_INC_ERROR_H
#define ALVOS_INC_ERROR_H

enum
{
	// 内核错误代码 -- 与 lib/printfmt.c 中的列表保持同步
	E_UNSPECIFIED = 1,	// 未知的问题
	E_BAD_ENV = 2,		// 环境不存在或无法在请求的操作中使用
	E_INVAL = 3,		// 无效的参数
	E_NO_MEM = 4,		// 由于内存不足，请求失败
	E_NO_FREE_ENV = 5,	// 尝试创建超过允许的最大值的新环境
	E_FAULT = 6,		// 内存故障
	E_NO_SYS = 7,		// 未实现的系统调用
	E_IPC_NOT_RECV = 8, // 尝试发送到不接收的环境
	E_EOF = 9,			// 文件非正常关闭

	// File system error codes -- only seen in user-level
	E_NO_DISK = 10,		// No free space left on disk
	E_MAX_OPEN = 11,	// Too many files are open
	E_NOT_FOUND = 12,	// File or block not found
	E_BAD_PATH = 13,	// Bad path
	E_FILE_EXISTS = 14, // File already exists
	E_NOT_EXEC = 15,	// File not a valid executable
	E_NOT_SUPP = 16,	// Operation not supported

	MAXERROR
};

#endif
