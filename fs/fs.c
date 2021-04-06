// 规范文件系统的磁盘结构，并实现文件系统的核心功能，比如文件的增删和读写
#include "inc/string.h"
#include "fs.h"

// --------------------------------------------------------------
// 超级块
// --------------------------------------------------------------

// 验证文件系统的超级块Validate the file system super-block.
void check_super(void)
{
	if (super->s_magic != FS_MAGIC)
		panic("bad file system magic number");

	if (super->s_nblocks > DISKSIZE / BLKSIZE)
		panic("file system is too large");

	cprintf("superblock is good\n");
}

// --------------------------------------------------------------
// 文件系统结构
// --------------------------------------------------------------

/**
 * 初始化文件系统(初始化super和bitmap全局指针变量)
 * 文件系统环境只要访问虚拟内存[DISKMAP, DISKMAP+DISKMAX]范围中的地址addr，就会访问到磁盘((uint32_t)addr - DISKMAP) / BLKSIZE block中的数据
 * 如果block数据还没复制到内存物理页，bc_pgfault()缺页处理函数会将数据从磁盘拷贝到某个物理页，并且将addr映射到该物理页
 * 这样FS环境只需要访问虚拟地址空间[DISKMAP, DISKMAP+DISKMAX]就能访问磁盘了
 */
void fs_init(void)
{
	static_assert(sizeof(struct File) == 256);

	// 找到一个 AlvOS 磁盘.
	// 如果第二个磁盘(number 1)可用，则使用它 Use the second IDE disk (number 1) if available.
	if (ide_probe_disk1())
		ide_set_disk(1);
	else
		ide_set_disk(0);

	bc_init();

	// 配置 super 指向超级块.
	super = diskaddr(1);
	check_super();
}

/**
 * 当要将一个修改后的文件flush回磁盘，就需要使用该函数找一个文件中连接的所有磁盘块，将它们都flush block
 * 实现：
 * 寻找文件结构f中的第filebno个块指向的硬盘块编号放入ppdiskbno
 * 即如果 filebno小于 NDIRECT，则返回属于 f-direct[INDIRECT]中的相应链接，否则返回 f_indirect中查找的块
 * 如果alloc为真且相应硬盘块不存在，则分配一个
 * 成功：返回0 (*ppdiskbno = 0)
 * 失败：
 * -E_NOT_FOUND: 需要分配间接块，但是alloc=0
 * -E_NO_DISK: 磁盘上没有用于间接块的空间
 * -E_INVAL: filebno 超出范围
 * 类比: 这就像文件的 pml4e_walk()
 */
static int
file_block_walk(struct File *f, uint32_t filebno, uint32_t **ppdiskbno, bool alloc)
{
	/**
	 * 这里涉及到了对文件中对于磁盘块链接的操作，一定要明确一个概念：
	 * File结构中无论是f_direct还是f_indirect，他们存储的都是指向的物理磁盘块的编号！
	 * 如果要对指向的磁盘块进行读写，那么必须用 diskaddr转换成文件系统地址空间后才可以进行相应的操作
	 */
	if (filebno >= NDIRECT + NINDIRECT)
	{
		return -E_INVAL;
	}
	uint32_t nblock = f->f_size / BLKSIZE;
	if (filebno > nblock)
	{
		return -E_NOT_FOUND;
	}
	if (filebno < NDIRECT)
	{
		*ppdiskbno = &f->f_direct[filebno];
		return 0;
	}
	else
	{
		if (!f->f_indirect)
		{
			return -E_NOT_FOUND;
		}
		uint32_t *index = (uint32_t *)diskaddr(f->f_indirect);
		*ppdiskbno = &index[filebno - NDIRECT];
	}
	return 0;
}

/**
 * 将 *blk 设置为内存中映射文件f的第filebno块的地址.
 * 成功：返回0
 * 失败：
 *  -E_NO_DISK: 需要分配一个块，但磁盘已满
 *  -E_INVAL: filebno 超出范围
 */
int file_get_block(struct File *f, uint32_t filebno, char **blk)
{
	if (filebno >= NDIRECT + NINDIRECT)
	{
		return -E_INVAL;
	}
	uint32_t *ppdiskbno;
	int r = file_block_walk(f, filebno, &ppdiskbno, false);
	if (r < 0)
		return r;
	if (!*ppdiskbno)
		return -E_NO_DISK;
	*blk = (char *)diskaddr(*ppdiskbno);
	return 0;
}

/**
 * 在参数 dir 尝试寻找名为 name 的文件，如果找到设置 *file 指向文件
 * 成功：返回0并设置 *file
 * 失败：-E_NOT_FOUND，没有找到对应文件
 */
static int
dir_lookup(struct File *dir, const char *name, struct File **file)
{
	int r;
	uint32_t i, j, nblock;
	char *blk;
	struct File *f;

	// 在 dir 中搜索 name.
	// 保持不变条件: 目录文件的大小始终是文件系统块大小的倍数.
	assert((dir->f_size % BLKSIZE) == 0);
	nblock = dir->f_size / BLKSIZE;
	for (i = 0; i < nblock; i++)
	{
		if ((r = file_get_block(dir, i, &blk)) < 0)
			return r;
		f = (struct File *)blk;
		for (j = 0; j < BLKFILES; j++)
			if (strcmp(f[j].f_name, name) == 0)
			{
				*file = &f[j];
				return 0;
			}
	}
	return -E_NOT_FOUND;
}

/**
 * 用于路径中的字符串处理，跳过斜杠'/'.
 */
static const char *
skip_slash(const char *p)
{
	while (*p == '/')
		p++;
	return p;
}

/**
 * 从根目录开始查找，解析路径 path，保存其路径指向的文件
 * 成功：则把相应的文件File结构赋值给 *pf，其所在目录的File结构赋值给 **pdir
 * 如果找不到文件，但是找到了文件所在的目录，设置 *pdir 并将最终的 path 复制到 lastelem
 */
static int
walk_path(const char *path, struct File **pdir, struct File **pf, char *lastelem)
{
	const char *p;
	char name[MAXNAMELEN];
	struct File *dir, *f;
	int r;

	path = skip_slash(path);
	f = &super->s_root;
	dir = 0;
	name[0] = 0;

	if (pdir)
		*pdir = 0;
	*pf = 0;
	while (*path != '\0')
	{
		dir = f;
		p = path;
		while (*path != '/' && *path != '\0')
			path++;
		if (path - p >= MAXNAMELEN)
			return -E_BAD_PATH;
		memmove(name, p, path - p);
		name[path - p] = '\0';
		path = skip_slash(path);

		if (dir->f_type != FTYPE_DIR)
			return -E_NOT_FOUND;

		if ((r = dir_lookup(dir, name, &f)) < 0)
		{
			if (r == -E_NOT_FOUND && *path == '\0')
			{
				if (pdir)
					*pdir = dir;
				if (lastelem)
					strcpy(lastelem, name);
				*pf = 0;
			}
			return r;
		}
	}

	if (pdir)
		*pdir = dir;
	*pf = f;
	return 0;
}

// --------------------------------------------------------------
// 文件操作
// --------------------------------------------------------------

// Open "path".  On success set *pf to point at the file and return 0.
// On error return < 0.
int file_open(const char *path, struct File **pf)
{
	return walk_path(path, 0, pf, 0);
}

// Read count bytes from f into buf, starting from seek position
// offset.  This meant to mimic the standard pread function.
// Returns the number of bytes read, < 0 on error.
ssize_t
file_read(struct File *f, void *buf, size_t count, off_t offset)
{
	int r, bn;
	off_t pos;
	char *blk;

	if (offset >= f->f_size)
		return 0;

	count = MIN(count, f->f_size - offset);

	for (pos = offset; pos < offset + count;)
	{
		if ((r = file_get_block(f, pos / BLKSIZE, &blk)) < 0)
			return r;
		bn = MIN(BLKSIZE - pos % BLKSIZE, offset + count - pos);
		memmove(buf, blk + pos % BLKSIZE, bn);
		pos += bn;
		buf += bn;
	}

	return count;
}
