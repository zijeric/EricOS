#include "inc/fs.h"
#include "inc/string.h"
#include "inc/lib.h"

#define debug 0

union Fsipc fsipcbuf __attribute__((aligned(PGSIZE)));

// Send an inter-environment request to the file server, and wait for
// a reply.  The request body should be in fsipcbuf, and parts of the
// response may be written back to fsipcbuf.
// type: request code, passed as the simple integer IPC value.
// dstva: virtual address at which to receive reply page, 0 if none.
// Returns result from the file server.
static int
fsipc(unsigned type, void *dstva)
{
	static envid_t fsenv;
	if (fsenv == 0)
		fsenv = ipc_find_env(ENV_TYPE_FS);

	//static_assert(sizeof(fsipcbuf) == PGSIZE);

	if (debug)
		cprintf("[%08x] fsipc %d %08x\n", thisenv->env_id, type, *(uint32_t *)&fsipcbuf);

	ipc_send(fsenv, type, &fsipcbuf, PTE_P | PTE_W | PTE_U);
	return ipc_recv(NULL, dstva, NULL);
}

static int devfile_flush(struct Fd *fd);
static ssize_t devfile_read(struct Fd *fd, void *buf, size_t n);
static ssize_t devfile_write(struct Fd *fd, const void *buf, size_t n);
static int devfile_stat(struct Fd *fd, struct Stat *stat);
static int devfile_trunc(struct Fd *fd, off_t newsize);

struct Dev devfile =
	{
		.dev_id = 'f',
		.dev_name = "file",
		.dev_read = devfile_read,
		.dev_close = devfile_flush,
		.dev_stat = devfile_stat,
};

// Open a file (or directory).
//
// Returns:
// 	The file descriptor index on success
// 	-E_BAD_PATH if the path is too long (>= MAXPATHLEN)
// 	< 0 for other errors.

int open(const char *path, int mode)
{
	if (strlen(path) >= MAXPATHLEN)
	{
		return -E_BAD_PATH;
	}
	// Find an unused file descriptor page using fd_alloc.
	// Then send a file-open request to the file server.
	// Include 'path' and 'omode' in request,
	// and map the returned file descriptor page
	// at the appropriate fd address.
	// FSREQ_OPEN returns 0 on success, < 0 on failure.
	//
	// (fd_alloc does not allocate a page, it just returns an
	// unused fd address.  Do you need to allocate a page?)
	//
	// Return the file descriptor index.
	// If any step after fd_alloc fails, use fd_close to free the
	// file descriptor.
	struct Fd *new_fd;
	int r = fd_alloc(&new_fd);
	if (r < 0)
	{
		return r;
	}
	fsipcbuf.open.req_omode = mode;
	strcpy(fsipcbuf.open.req_path, path);
	r = fsipc(FSREQ_OPEN, new_fd);
	if (r < 0)
	{
		fd_close(new_fd, 0);
		return r;
	}
	return fd2num(new_fd);
}

/**
 * 将缓冲区写入磁盘称为缓冲区刷新。
 * 当用户线程修改缓冲区中的数据时，它将缓冲区标记为dirty。
 * 当数据库服务器将缓冲区刷新到磁盘时，它随后将缓冲区标记为未弄脏，并允许覆盖缓冲区中的数据
 * flush 文件描述符，在此之后，fileid 无效
 * 这个函数被 fd_close() 调用
 * fd_close() 将负责解除这个环境中的FD页面映射
 * 由于服务器使用FD页面上的引用计数来检测哪些文件打开了，取消它的映射就足以释放服务器端资源
 * 除此之外，我们只需要确保我们的更改被 flush 到磁盘上
 */
static int
devfile_flush(struct Fd *fd)
{
	fsipcbuf.flush.req_fileid = fd->fd_file.id;
	return fsipc(FSREQ_FLUSH, NULL);
}

// Read at most 'n' bytes from 'fd' at the current position into 'buf'.
//
// Returns:
// 	The number of bytes successfully read.
// 	< 0 on error.
static ssize_t
devfile_read(struct Fd *fd, void *buf, size_t n)
{
	// Make an FSREQ_READ request to the file system server after
	// filling fsipcbuf.read with the request arguments.  The
	// bytes read will be written back to fsipcbuf by the file
	// system server.
	fsipcbuf.read.req_fileid = fd->fd_file.id;
	fsipcbuf.read.req_n = n;
	ssize_t nbytes = fsipc(FSREQ_READ, NULL);
	if (nbytes > 0)
	{
		memmove(buf, fsipcbuf.readRet.ret_buf, nbytes);
	}
	return nbytes;
}

static int
devfile_stat(struct Fd *fd, struct Stat *st)
{
	int r;

	fsipcbuf.stat.req_fileid = fd->fd_file.id;
	if ((r = fsipc(FSREQ_STAT, NULL)) < 0)
		return r;
	strcpy(st->st_name, fsipcbuf.statRet.ret_name);
	st->st_size = fsipcbuf.statRet.ret_size;
	st->st_isdir = fsipcbuf.statRet.ret_isdir;
	return 0;
}
