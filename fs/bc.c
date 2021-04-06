/**
 * 提供磁盘的块缓存实现机制. 
 * 因为AlvOS支持的最大磁盘大小为512GB，所以可以使用类似实现fork的COW页机制:
 * 1.用文件系统服务环境的虚拟地址空间(256TB)对应到磁盘的地址空间上(512GB)
 * 2.初始文件系统服务环境里什么物理页都没映射，如果要访问一个磁盘的地址空间，则发生页错误
 * 3.在页错误处理程序中，在内存中申请一个块的空间映射到相应的文件系统虚拟地址上
 *   然后去实际的物理磁盘上读取这个区域的内容到这个内存区域上，然后恢复文件系统服务环境
 *
 * block cache(磁盘块缓存)专门为文件系统服务环境实现这部分功能的模块
 * 这样就使用用户进程的机制完成了对于物理磁盘的读写机制，并且尽量少节省了内存
 * 当然这里也有一个取巧的地方就是用虚拟地址空间模拟磁盘地址空间
 */
#include "fs.h"

// 返回磁盘块所对应的虚拟地址.
void *
diskaddr(uint64_t blockno)
{
	if (blockno == 0 || (super && blockno >= super->s_nblocks))
		panic("bad block number %08x in diskaddr", blockno);
	return (char *)(DISKMAP + blockno * BLKSIZE);
}

/**
 * bc_pgfault() 在页错误时从磁盘中加载页(从磁盘加载到内存中的任何磁盘块都会出现故障)
 */
static void
bc_pgfault(struct UTrapframe *utf)
{
	// 注意，addr可能没有对齐block边界
	void *addr = (void *)utf->utf_fault_va;
	uint64_t blockno = ((uint64_t)addr - DISKMAP) / BLKSIZE;
	int r;

	// 检查错误是否在块缓存区域内
	if (addr < (void *)DISKMAP || addr >= (void *)(DISKMAP + DISKSIZE))
		panic("page fault in FS: eip %08x, va %08x, err %04x",
			  utf->utf_rip, addr, utf->utf_err);

	// 检查块编号.
	if (super && blockno >= super->s_nblocks)
		panic("bc_pgfault: reading non-existent block %08x\n", blockno);

	// 在磁盘映射区域中分配一个物理页，将块的内容从磁盘读入该页面.
	// 注意，第一轮添加到页边界.
	void *new_addr = ROUNDDOWN(addr, PGSIZE);
	r = sys_page_alloc(0, new_addr, PTE_P | PTE_W | PTE_U);
	if (r < 0)
		panic("bc_pgfault: page allocation failed: %e", r);

	uint64_t sec_no = BLKSECTS * blockno;
	size_t nsecs = BLKSECTS;
	// ide_read() 操作的是 sectors 而不是 blocks
	r = ide_read(sec_no, new_addr, nsecs);
	if (r < 0)
		panic("bc_pgfault: Something wrong with reading from the disk: %e", r);
}

/**
 * 设置用户级页错误处理函数，并通过读取一次来缓存超级块
 * 主要是这里设置起了文件系统的超级块Super，文件系统将第Super块指向了文件系统的Block 1，然后块位图指向了Block 2
 * 这里对应了在2.1.3中呈现的那张磁盘规划图
 * 块位图是没有相关结构的，因为就是直接读取特定二进制位
 */
void bc_init(void)
{
	struct Super super;
	// 设置用户级页错误处理函数.
	set_pgfault_handler(bc_pgfault);

	// 超级块的位置是虚拟地址空间的第一块，被访问时，自然会使用磁盘块缓存机制读取到用户空间，读取来源是IDE磁盘.
	memmove(&super, diskaddr(1), sizeof super);
}
