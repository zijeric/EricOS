#include "inc/fs.h"
#include "inc/lib.h"

// 一个环境可以拥有100TB，文件系统的环境可以处理的磁盘大小最多为 512GB
// AlvOS 系统中，将这512GB空间固定在文件系统环境的地址空间[DISKMAP, MAP + DISKSIZE]作为缓冲区
// 当磁盘读入内存时，用来映射相关的页，如，磁盘0永远映射在0x10000000，磁盘1永远映射在0x10001000
// 由于文件系统环境的虚拟地址空间独立于系统中其他环境的虚拟地址空间，而其惟一要做的事情就是提供文件的访问，因此以这种方法保留文件系统大部分的地址空间是很有效的

#define SECTSIZE 512                  // bytes per disk sector
#define BLKSECTS (BLKSIZE / SECTSIZE) // sectors per block

/* 磁盘块 n，当在内存时，被映射进文件系统服务器的地址空间 DISKMAP + (n*BLKSIZE). */
#define DISKMAP 0x10000000

// 0xC0000000 (3GB)
/* AlvOS 可处理的最大磁盘大小 (512GB) */
#define DISKSIZE 0x8000000000

struct Super *super; // superblock
uint32_t *bitmap;    // bitmap blocks mapped in memory

/* ide.c */

bool ide_probe_disk1(void);
void ide_set_disk(int diskno);
int ide_read(uint32_t secno, void *dst, size_t nsecs);
int ide_write(uint32_t secno, const void *src, size_t nsecs);

/* bc.c */

void *diskaddr(uint64_t blockno);
bool va_is_mapped(void *va);
bool va_is_dirty(void *va);
void flush_block(void *addr);

void bc_init(void);

/* fs.c */

void fs_init(void);
int file_get_block(struct File *f, uint32_t file_blockno, char **pblk);
int file_create(const char *path, struct File **f);
int file_open(const char *path, struct File **f);
ssize_t file_read(struct File *f, void *buf, size_t count, off_t offset);
int file_write(struct File *f, const void *buf, size_t count, off_t offset);
int file_set_size(struct File *f, off_t newsize);
void file_flush(struct File *f);
int file_remove(const char *path);
void fs_sync(void);

/* int	map_block(uint32_t); */
bool block_is_free(uint32_t blockno);
int alloc_block(void);

/* test.c */

void fs_test(void);
