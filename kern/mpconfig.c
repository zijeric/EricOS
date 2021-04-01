// 读取(搜索)并解析多处理器配置表的代码
// See http://developer.intel.com/design/pentium/datashts/24201606.pdf

#include "inc/types.h"
#include "inc/string.h"
#include "inc/memlayout.h"
#include "inc/x86.h"
#include "inc/mmu.h"
#include "inc/env.h"
#include "AlvOS/cpu.h"
#include "kern/pmap.h"

struct CpuInfo cpus[NCPU];
struct CpuInfo *bootcpu;
int ismp;
int ncpu;

/** 
 * Per-CPU(NCPU) 的内核栈(size:KSTKSIZE)
 * 多个 cpu 可以同时 trap 到内核中，为了防止干扰彼此的运行，需要为每个处理器设置单独的栈空间
 * BSP 的内核栈地址是 bootstack，映射到虚拟地址 KSTACKTOP 之下的 KSTKSIZE 大小的空间
 * 紧随着，跳过保护页(KSTKGAP-bit)之后，映射 CPU1 内核栈空间，以此类推
 * 
 * 如果共享的内核栈，中断时，硬件会先自动将相关寄存器进栈，然后才执行锁的检查，共享内核栈可能会导致系统崩溃
 * 支持多个 CPU 的时候，只有一份内核4级页表，所有CPU都会使用这个4级页表映射CPU栈
 * 不同的 CPU 栈映射到不同的虚拟地址上
 * 注意：不同的用户环境是可以同时将用户栈物理地址映射到 UXSTACKTOP 上的
 * 因为每一个用户环境都有一份独立的4级页表，创建用户环境的时候会分配和映射一页用户栈物理页到UXSTACKTOP上
 * 多个 CPU 同时运行多个用户环境的时候，实际上都是使用各自的4级页表进行寻址和存储数据到各自的用户栈物理页上的
 */
unsigned char percpu_kstacks[NCPU][KSTKSIZE]
	__attribute__((aligned(PGSIZE)));

// 参照 MultiProcessor Specification Version 1.[14]

struct mp
{						  // 浮动指针 [MP 4.1]
	uint8_t signature[4]; // "_MP_"
	physaddr_t physaddr;  // phys addr of MP config table
	uint8_t length;		  // 1
	uint8_t specrev;	  // [14]
	uint8_t checksum;	  // all bytes must add up to 0
	uint8_t type;		  // MP system config type
	uint8_t imcrp;
	uint8_t reserved[3];
} __attribute__((__packed__));

struct mpconf
{						  // configuration table header [MP 4.2]
	uint8_t signature[4]; // "PCMP"
	uint16_t length;	  // total table length
	uint8_t version;	  // [14]
	uint8_t checksum;	  // all bytes must add up to 0
	uint8_t product[20];  // product id
	uint32_t oemtable;	  // OEM table pointer
	uint16_t oemlength;	  // OEM table length
	uint16_t entry;		  // entry count
	uint32_t lapicaddr;	  // address of local APIC
	uint16_t xlength;	  // extended table length
	uint8_t xchecksum;	  // extended table checksum
	uint8_t reserved;
	uint8_t entries[0]; // table entries
} __attribute__((__packed__));

struct mpproc
{						  // processor table entry [MP 4.3.1]
	uint8_t type;		  // entry type (0)
	uint8_t apicid;		  // local APIC id
	uint8_t version;	  // local APIC version
	uint8_t flags;		  // CPU flags
	uint8_t signature[4]; // CPU signature
	uint32_t feature;	  // feature flags from CPUID instruction
	uint8_t reserved[8];
} __attribute__((__packed__));

// mpproc flags
#define MPPROC_BOOT 0x02 // This mpproc is the bootstrap processor

// Table entry types
#define MPPROC 0x00	  // One per processor
#define MPBUS 0x01	  // One per bus
#define MPIOAPIC 0x02 // One per I/O APIC
#define MPIOINTR 0x03 // One per bus interrupt source
#define MPLINTR 0x04  // One per system interrupt source

static uint8_t
sum(void *addr, int len)
{
	int i, sum;

	sum = 0;
	for (i = 0; i < len; i++)
		sum += ((uint8_t *)addr)[i];
	return sum;
}

// Look for an MP structure in the len bytes at physical address addr.
static struct mp *
mpsearch1(physaddr_t a, int len)
{
	struct mp *mp = KADDR(a), *end = KADDR(a + len);

	for (; mp < end; mp++)
		if (memcmp(mp->signature, "_MP_", 4) == 0 &&
			sum(mp, sizeof(*mp)) == 0)
			return mp;
	return NULL;
}

/**
 * 搜索 MP 指针结构，根据[MP 4]，它位于以下三个位置之一:
 *   1) 在 EBDA 的首个千字节
 *   2) 如果没有E BDA，则在系统基本内存的最后一千字节
 *   3) 在 [0xE0000, 0xFFFFF] 的 BIOS ROM
 */ 
static struct mp *
mpsearch(void)
{
	uint8_t *bda;
	uint32_t p;
	struct mp *mp;

	//static_assert(sizeof(*mp) == 32);

	// The BIOS data area lives in 16-bit segment 0x40.
	bda = (uint8_t *)KADDR(0x40 << 4);

	// [MP 4] The 16-bit segment of the EBDA is in the two bytes
	// starting at byte 0x0E of the BDA.  0 if not present.
	if ((p = *(uint16_t *)(bda + 0x0E)))
	{
		p <<= 4; // Translate from segment to PA
		if ((mp = mpsearch1(p, 1024)))
			return mp;
	}
	else
	{
		// The size of base memory, in KB is in the two bytes
		// starting at 0x13 of the BDA.
		p = *(uint16_t *)(bda + 0x13) * 1024;
		if ((mp = mpsearch1(p - 1024, 1024)))
			return mp;
	}
	return mpsearch1(0xF0000, 0x10000);
}

// 搜索MP配置表，目前还不能接受默认配置(physaddr == 0)
// 检查正确的签名、校验和以及版本
static struct mpconf *
mpconfig(struct mp **pmp)
{
	struct mpconf *conf;
	struct mp *mp;

	if ((mp = mpsearch()) == 0)
		return NULL;
	if (mp->physaddr == 0 || mp->type != 0)
	{
		cprintf("SMP: Default configurations not implemented\n");
		return NULL;
	}
	conf = (struct mpconf *)KADDR(mp->physaddr);
	if (memcmp(conf, "PCMP", 4) != 0)
	{
		cprintf("SMP: Incorrect MP configuration table signature\n");
		return NULL;
	}
	if (sum(conf, conf->length) != 0)
	{
		cprintf("SMP: Bad MP configuration checksum\n");
		return NULL;
	}
	if (conf->version != 1 && conf->version != 4)
	{
		cprintf("SMP: Unsupported MP version %d\n", conf->version);
		return NULL;
	}
	if (sum((uint8_t *)conf + conf->length, conf->xlength) != conf->xchecksum)
	{
		cprintf("SMP: Bad MP configuration extended checksum\n");
		return NULL;
	}
	*pmp = mp;
	return conf;
}

/**
 * 在启动APs之前，BSP 从 BIOS 的内存区域中读取 MP 配置表[mp_init()的作用]，收集多处理器系统的信息，
 * 比如CPUs数目、APIC IDs、LAPIC单元的MMIO地址
 * 通过读取驻留在BIOS内存区域中的MP配置表来检索此信息，也就是说在出厂时，厂家就将此计算机的处理器信息写入了BIOS中，
 * 其有一定的规范，也就是kern/mpconfig.c中struct mp定义的
 * 
 * 细节：
 * 通过调用 mpconfig() 从BIOS中读取浮动指针mp，从mp中找到struct mpconf多处理器配置表，
 * 然后根据这个结构体内的entries信息(processor table entry)对各个cpu结构体进行配置(主要是cpu_id)
 * 如果proc->flag是MPPROC_BOOT，说明这个入口对应的处理器是用于启动的处理器，我们把结构体数组cpus[ncpu]地址赋值给bootcpu指针
 * 注意这里ncpu是个全局变量，那么这里实质上就是把cpus数组的第一个元素的地址给了bootcpu
 * 如果出现任何entries匹配错误，则认为处理器的初始化失败了，不能用多核处理器进行机器的运行
 */
void mp_init(void)
{
	struct mp *mp;
	struct mpconf *conf;
	struct mpproc *proc;
	uint8_t *p;
	unsigned int i;

	bootcpu = &cpus[0];
	// 调用 mpconfig() 从BIOS中读取浮动指针 mp
	if ((conf = mpconfig(&mp)) == 0)
		return;
	ismp = 1;
	lapicaddr = conf->lapicaddr;

	// 从 mp 中找到 struct mpconf 多处理器配置表，然后根据这个结构体内的 entries 信息(processor table entry)
	// 对各个cpu结构体进行配置(主要是cpu_id)
	for (p = conf->entries, i = 0; i < conf->entry; i++)
	{
		switch (*p)
		{
		case MPPROC:
			proc = (struct mpproc *)p;
			// 如果proc->flag是MPPROC_BOOT，说明这个入口对应的处理器是用于启动的处理器，我们把结构体数组cpus[ncpu]地址赋值给bootcpu指针
			// 注意这里ncpu是个全局变量，那么这里实质上就是把cpus数组的第一个元素的地址给了bootcpu
			if (proc->flags & MPPROC_BOOT)
				bootcpu = &cpus[ncpu];
			if (ncpu < NCPU)
			{
				cpus[ncpu].cpu_id = ncpu;
				ncpu++;
			}
			else
			{
				cprintf("SMP: too many CPUs, CPU %d disabled\n",
						proc->apicid);
			}
			p += sizeof(struct mpproc);
			continue;
		case MPBUS:
		case MPIOAPIC:
		case MPIOINTR:
		case MPLINTR:
			p += 8;
			continue;
		default:
			cprintf("mpinit: unknown config type %x\n", *p);
			ismp = 0;
			i = conf->entry;
		}
	}

	bootcpu->cpu_status = CPU_STARTED;
	// 如果出现任何entries匹配错误，则认为处理器的初始化失败了，不能用多核处理器进行机器的运行
	if (!ismp)
	{
		// 没有找到其他 CPU; 返回到单核.
		ncpu = 1;
		lapicaddr = 0;
		cprintf("SMP: configuration not found, SMP disabled\n");
		return;
	}
	cprintf("SMP: CPU %d found %d CPU(s)\n", bootcpu->cpu_id, ncpu);

	if (mp->imcrp)
	{
		// [MP 3.2.6.1] If the hardware implements PIC mode,
		// switch to getting interrupts from the LAPIC.
		cprintf("SMP: Setting IMCR to switch from PIC mode to symmetric I/O mode\n");
		outb(0x22, 0x70);		   // Select IMCR
		outb(0x23, inb(0x23) | 1); // Mask external interrupts.
	}
}
