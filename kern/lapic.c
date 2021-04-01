// 在每个处理器中驱动 Local APIC 单元的内核代码
// Local APIC 管理内部(非I/O)中断
// See Chapter 8 & Appendix C of Intel processor manual volume 3.

#include "inc/types.h"
#include "inc/memlayout.h"
#include "inc/trap.h"
#include "inc/mmu.h"
#include "inc/stdio.h"
#include "inc/x86.h"
#include "kern/pmap.h"
#include "AlvOS/cpu.h"

// Local APIC registers, divided by 4 for use as uint32_t[] indices.
#define ID (0x0020 / 4)	   // ID
#define VER (0x0030 / 4)   // Version
#define TPR (0x0080 / 4)   // Task Priority
#define EOI (0x00B0 / 4)   // EOI
#define SVR (0x00F0 / 4)   // Spurious Interrupt Vector
#define ENABLE 0x00000100  // Unit Enable
#define ESR (0x0280 / 4)   // Error Status
#define ICRLO (0x0300 / 4) // Interrupt Command
#define INIT 0x00000500	   // INIT/RESET
#define STARTUP 0x00000600 // Startup IPI
#define DELIVS 0x00001000  // Delivery status
#define ASSERT 0x00004000  // Assert interrupt (vs deassert)
#define DEASSERT 0x00000000
#define LEVEL 0x00008000  // Level triggered
#define BCAST 0x00080000  // Send to all APICs, including self.
#define OTHERS 0x000C0000 // Send to all APICs, excluding self.
#define BUSY 0x00001000
#define FIXED 0x00000000
#define ICRHI (0x0310 / 4)	// Interrupt Command [63:32]
#define TIMER (0x0320 / 4)	// Local Vector Table 0 (TIMER)
#define X1 0x0000000B		// divide counts by 1
#define PERIODIC 0x00020000 // Periodic
#define PCINT (0x0340 / 4)	// Performance Counter LVT
#define LINT0 (0x0350 / 4)	// Local Vector Table 1 (LINT0)
#define LINT1 (0x0360 / 4)	// Local Vector Table 2 (LINT1)
#define ERROR (0x0370 / 4)	// Local Vector Table 3 (ERROR)
#define MASKED 0x00010000	// Interrupt masked
#define TICR (0x0380 / 4)	// Timer Initial Count
#define TCCR (0x0390 / 4)	// Timer Current Count
#define TDCR (0x03E0 / 4)	// Timer Divide Configuration

physaddr_t lapicaddr; // Initialized in mpconfig.c
volatile uint32_t *lapic;

static void
lapicw(int index, int value)
{
	lapic[index] = value;
	lapic[ID]; // wait for write to finish, by reading
}

/**
 * 将编程LAPIC的内置计时器，触发时钟中断以支持抢占式多任务调度
 */ 
void lapic_init(void)
{
	if (!lapicaddr)
		return;

	// lapicaddr is the physical address of the LAPIC's 4K MMIO
	// region.  Map it in to virtual memory so we can access it.
	lapic = mmio_map_region(lapicaddr, 4096);

	// Enable local APIC; set spurious interrupt vector.
	lapicw(SVR, ENABLE | (IRQ_OFFSET + IRQ_SPURIOUS));

	// The timer repeatedly counts down at bus frequency
	// from lapic[TICR] and then issues an interrupt.
	// If we cared more about precise timekeeping,
	// TICR would be calibrated using an external time source.
	lapicw(TDCR, X1);
	lapicw(TIMER, PERIODIC | (IRQ_OFFSET + IRQ_TIMER));
	lapicw(TICR, 10000000);

	// Leave LINT0 of the BSP enabled so that it can get
	// interrupts from the 8259A chip.
	//
	// According to Intel MP Specification, the BIOS should initialize
	// BSP's local APIC in Virtual Wire Mode, in which 8259A's
	// INTR is virtually connected to BSP's LINTIN0. In this mode,
	// we do not need to program the IOAPIC.
	if (thiscpu != bootcpu)
		lapicw(LINT0, MASKED);

	// Disable NMI (LINT1) on all CPUs
	lapicw(LINT1, MASKED);

	// Disable performance counter overflow interrupts
	// on machines that provide that interrupt entry.
	if (((lapic[VER] >> 16) & 0xFF) >= 4)
		lapicw(PCINT, MASKED);

	// Map error interrupt to IRQ_ERROR.
	lapicw(ERROR, IRQ_OFFSET + IRQ_ERROR);

	// Clear error status register (requires back-to-back writes).
	lapicw(ESR, 0);
	lapicw(ESR, 0);

	// Ack any outstanding interrupts.
	lapicw(EOI, 0);

	// Send an Init Level De-Assert to synchronize arbitration ID's.
	lapicw(ICRHI, 0);
	lapicw(ICRLO, BCAST | INIT | LEVEL);
	while (lapic[ICRLO] & DELIVS)
		;

	// Enable interrupts on the APIC (but not on the processor).
	lapicw(TPR, 0);
}

/**
 * 根据LAPIC识别码(APIC ID)识别我们的代码运行在哪个CPU上
 */ 
int cpunum(void)
{
	if (lapic)
		return lapic[ID] >> 24;
	return 0;
}

// Acknowledge interrupt.
void lapic_eoi(void)
{
	if (lapic)
		lapicw(EOI, 0);
}

// Spin for a given number of microseconds.
// On real hardware would want to tune this dynamically.
static void
microdelay(int us)
{
}

#define IO_RTC 0x70

// 参考 MultiProcessor Specification 附录B.
// 从 BSP 发送 STARTUP 处理器间中断 IPI(interprocessor interrupt)到 APs，启动其他 CPU
// 在参数 addr 启动运行其他处理器运行的入口代码.
void lapic_startap(uint8_t apicid, uint32_t addr)
{
	int i;
	uint16_t *wrv;

	/**
	 * "BSP 必须在[通用启动算法(universal startup algorithm)]之前将 CMOS 关机代码初始化为0AH
	 * 并将热重置向量指向 AP 启动代码"
	 */
	// 偏移 0xF 是关机代码
	outb(IO_RTC, 0xF);
	outb(IO_RTC + 1, 0x0A);

	// 热重置向量
	wrv = (uint16_t *)KADDR((0x40 << 4 | 0x67));
	wrv[0] = 0;
	wrv[1] = addr >> 4;

	// "通用启动算法."
	// 为了重置其他 CPU 发送INIT(电平触发)中断.
	lapicw(ICRHI, apicid << 24);
	lapicw(ICRLO, INIT | LEVEL | ASSERT);
	microdelay(200);
	lapicw(ICRLO, INIT | LEVEL);
	microdelay(100);

	/**
	 * 为了输入代码，发送启动IPI(两次)
	 * 一般的硬件只有在由于 INIT 而处于暂停状态时才会接受 STARTUP
	 * 所以第二种应该被忽略，但它是 Intel 官方算法的一部分
	 * 第二种对Bochs来说太糟糕了
	 */
	for (i = 0; i < 2; i++)
	{
		lapicw(ICRHI, apicid << 24);
		lapicw(ICRLO, STARTUP | (addr >> 12));
		microdelay(200);
	}
}

void lapic_ipi(int vector)
{
	lapicw(ICRLO, OTHERS | FIXED | vector);
	while (lapic[ICRLO] & DELIVS)
		;
}
