// 类 posix 文件描述符仿真层的公共定义，用户态库为应用程序的使用实现了这个定义
// 有关实现细节，请参阅lib目录中的代码.

#ifndef ALVOS_INC_FD_H
#define ALVOS_INC_FD_H

#include "inc/types.h"
#include "inc/fs.h"

struct Fd;
struct Stat;
struct Dev;
/* 系统还定义了文件描述符ID，为了支持用户环境使用库函数访问文件系统，主要有以下数据结构 */

// Per-device-class 文件描述符操作
// 对应一个块设备，并为此设备提供读、写、査看状态信息等操作，其指针函数对应 lib/file.c 的函数
struct Dev
{
	int dev_id;
	const char *dev_name;
	ssize_t (*dev_read)(struct Fd *fd, void *buf, size_t len);
	ssize_t (*dev_write)(struct Fd *fd, const void *buf, size_t len);
	int (*dev_close)(struct Fd *fd);
	int (*dev_stat)(struct Fd *fd, struct Stat *stat);
	int (*dev_trunc)(struct Fd *fd, off_t length);
};

// 将一个文件ID 对应一个文件对象
struct FdFile
{
	int id;
};

// 文件ID，其对应一个文件，保存文件的操作模式，比如只读、可写等
struct Fd
{
	int fd_dev_id;
	off_t fd_offset;
	// 文件的操作模式
	int fd_omode;
	union
	{
		// 文件服务器的文件
		struct FdFile fd_file;
	};
};

// 保存文件的状态信息，文件名、文件大小、文件类型以及文件所属设备
struct Stat
{
	char st_name[MAXNAMELEN];
	off_t st_size;
	int st_isdir;
	struct Dev *st_dev;
};

/* 针对这些数据结构，定义了一些宏以及函数来对这些结构进行操作 */

char *fd2data(struct Fd *fd);

uint64_t fd2num(struct Fd *fd);
int fd_alloc(struct Fd **fd_store);
int fd_close(struct Fd *fd, bool must_exist);
int fd_lookup(int fdnum, struct Fd **fd_store);
int dev_lookup(int devid, struct Dev **dev_store);

extern struct Dev devfile;
extern struct Dev devcons;
extern struct Dev devpipe;

#endif
