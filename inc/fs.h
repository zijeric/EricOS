#ifndef ALVOS_INC_FS_H
#define ALVOS_INC_FS_H

#include "inc/types.h"
#include "inc/mmu.h"

// 文件 nodes (both in-memory and on-disk)

// 每个文件系统块字节 - 与物理页大小相同
#define BLKSIZE PGSIZE
#define BLKBITSIZE (BLKSIZE * 8)

// 文件名的最大大小(单路径组件)，包括 NULL 必须是4的倍数
#define MAXNAMELEN 128

// 完整路径名的最大大小，包括 NULL
#define MAXPATHLEN 1024

// 文件描述符中块指针的数量
#define NDIRECT 10
// 间接块中直接块指针的数量(1024)
#define NINDIRECT (BLKSIZE / 4)

#define MAXFILESIZE ((NDIRECT + NINDIRECT) * BLKSIZE)

// 文件结构
// 一个File结构的大小为256-Byte，所以一个块中可以放下4个File结构
struct File
{
	// 文件名
	char f_name[MAXNAMELEN];
	// 文件的大小(字节)
	off_t f_size;

	// 文件类型(文件/目录)
	// 文件系统不会解析代表文件的File结构的数据块的内容;
	// 但会解析代表目录的File结构数据块内容来获得其所包含的文件和子目录的信息
	uint32_t f_type;

	// 块(Block)指针(指向文件所包含磁盘块的指针).
	// 块指针只有在内存中才有意义，因此每次从磁盘读 File 结构到内存时，都要清空域值
	// 如果一个块的值!=0，说明已被分配到内存.
	uint32_t f_direct[NDIRECT]; // 直接磁盘块(块数组[10]存储10个块号)，大于40KB的文件(10*BLKSIZE)要间接寻址
	uint32_t f_indirect;		// 只有一个间接磁盘块，目录不使用间接磁盘块的前10个块号，最大存储1024个块号(4096/4)

	// 填充到256个字节
	uint8_t f_pad[256 - MAXNAMELEN - 8 - 4 * NDIRECT - 4];
} __attribute__((packed));

// 一个 inode 块恰好包含 BLKFILES 的文件结构
#define BLKFILES (BLKSIZE / sizeof(struct File))

// 文件类型
#define FTYPE_REG 0 // 普通文件
#define FTYPE_DIR 1 // 目录

// 文件系统的超级块 (both in-memory and on-disk)
// 文件系统通常在磁盘某个容易寻找的位置保留特定的磁盘块，用于存储文件系统的元数据描述属性
// 比如：block size、disk size、查找root目录所需的元数据、文件系统上次挂载时间、上次检查磁盘错误的时间等

#define FS_MAGIC 0x416C7661 // str: 'Alva!'

// 超级块结构
struct Super
{
	uint32_t s_magic;	// 文件系统的魔数: FS_MAGIC
	uint32_t s_nblocks; // 在磁盘中块的数量
	struct File s_root; // 根目录 node(登录点)
};

// 客户端对文件系统的请求的定义
enum
{
	FSREQ_OPEN = 1,
	FSREQ_SET_SIZE,
	// Read returns a Fsret_read on the request page
	FSREQ_READ,
	FSREQ_WRITE,
	// Stat returns a Fsret_stat on the request page
	FSREQ_STAT,
	FSREQ_FLUSH,
	FSREQ_REMOVE,
	FSREQ_SYNC
};

union Fsipc
{
	struct Fsreq_open
	{
		char req_path[MAXPATHLEN];
		int req_omode;
	} open;
	struct Fsreq_set_size
	{
		int req_fileid;
		off_t req_size;
	} set_size;
	struct Fsreq_read
	{
		int req_fileid;
		size_t req_n;
	} read;
	struct Fsret_read
	{
		char ret_buf[PGSIZE];
	} readRet;
	struct Fsreq_write
	{
		int req_fileid;
		size_t req_n;
		char req_buf[PGSIZE - (sizeof(int) + sizeof(size_t))];
	} write;
	struct Fsreq_stat
	{
		int req_fileid;
	} stat;
	struct Fsret_stat
	{
		char ret_name[MAXNAMELEN];
		off_t ret_size;
		int ret_isdir;
	} statRet;
	struct Fsreq_flush
	{
		int req_fileid;
	} flush;
	struct Fsreq_remove
	{
		char req_path[MAXPATHLEN];
	} remove;

	// Ensure Fsipc is one page
	char _pad[PGSIZE];
};

#endif
