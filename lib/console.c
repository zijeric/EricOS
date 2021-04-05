
#include "inc/string.h"
#include "inc/lib.h"

void
cputchar(int ch)
{
	char c = ch;

	// 与标准的Unix的putchar不同，cputchar() 总是输出到系统控制台.
	sys_cputs(&c, 1);
}

int
getchar(void)
{
	unsigned char c;
	int r;

	// 然而，AlvOS 支持标准的_input_(输入)重定向，允许用户将脚本文件重定向到shell等等
	// getchar()从文件描述符0读取一个字符
	r = read(0, &c, 1);
	if (r < 0)
		return r;
	if (r < 1)
		return -E_EOF;
	return c;
}


// "真实的"控制台文件描述符实现.
// 默认情况下，上面的 putchar()/getchar() 仍然会出现在这里，但是现在可以通过 fd 层重定向到文件、管道等.

static ssize_t devcons_read(struct Fd*, void*, size_t);
static ssize_t devcons_write(struct Fd*, const void*, size_t);
static int devcons_close(struct Fd*);
static int devcons_stat(struct Fd*, struct Stat*);

struct Dev devcons =
{
	.dev_id =	'c',
	.dev_name =	"cons",
	.dev_read =	devcons_read,
	.dev_write =	devcons_write,
	.dev_close =	devcons_close,
	.dev_stat =	devcons_stat
};

int
iscons(int fdnum)
{
	int r;
	struct Fd *fd;

	if ((r = fd_lookup(fdnum, &fd)) < 0)
		return r;
	return fd->fd_dev_id == devcons.dev_id;
}

int
opencons(void)
{
	int r;
	struct Fd* fd;

	if ((r = fd_alloc(&fd)) < 0)
		return r;
	if ((r = sys_page_alloc(0, fd, PTE_P|PTE_U|PTE_W|PTE_SHARE)) < 0)
		return r;
	fd->fd_dev_id = devcons.dev_id;
	fd->fd_omode = O_RDWR;
	return fd2num(fd);
}

static ssize_t
devcons_read(struct Fd *fd, void *vbuf, size_t n)
{
	int c;

	if (n == 0)
		return 0;

	while ((c = sys_cgetc()) == 0)
		sys_yield();
	if (c < 0)
		return c;
	if (c == 0x04)	// ctl-d is eof
		return 0;
	*(char*)vbuf = c;
	return 1;
}

static ssize_t
devcons_write(struct Fd *fd, const void *vbuf, size_t n)
{
	int tot, m;
	char buf[128];

	// mistake: have to nul-terminate arg to sys_cputs,
	// so we have to copy vbuf into buf in chunks and nul-terminate.
	for (tot = 0; tot < n; tot += m) {
		m = n - tot;
		if (m > sizeof(buf) - 1)
			m = sizeof(buf) - 1;
		memmove(buf, (char*)vbuf + tot, m);
		sys_cputs(buf, m);
	}
	return tot;
}

static int
devcons_close(struct Fd *fd)
{
	USED(fd);

	return 0;
}

static int
devcons_stat(struct Fd *fd, struct Stat *stat)
{
	strcpy(stat->st_name, "<cons>");
	return 0;
}

