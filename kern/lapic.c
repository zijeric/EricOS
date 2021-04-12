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
#include "kern/cpu.h"

// Local APIC 寄存器集, divided by 4 for use as uint32_t[] indices.
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

physaddr_t lapicaddr; // local APIC 的地址，在 mpconfig.c 中初始化
volatile uint32_t *lapic;

static void
lapicw(int index, int value)
{
	lapic[index] = value;
	lapic[ID]; // 通过读取，等待写操作完成
}

/**
 * 
 * 
 */
void lapic_init(void)
{
	// 尚未初始化 local APIC 的地址，BSP 尚未获取多处理器系统的信息，不能继续
	if (!lapicaddr)
		return;

	// lapicaddr 是 LAPIC 的 4KB MMIO 区域的物理地址
	// 为了可以访问它，把它映射到虚拟内存中.
	lapic = mmio_map_region(lapicaddr, 4096);

	// 启用本地APIC；设置虚假中断向量.
	lapicw(SVR, ENABLE | (IRQ_OFFSET + IRQ_SPURIOUS));

	// 计时器以总线频率从lapic[TICR]重复计数，然后发出一个中断
	// 如果更关心精确计时，TICR将使用外部时间源进行校准.
	lapicw(TDCR, X1);
	lapicw(TIMER, PERIODIC | (IRQ_OFFSET + IRQ_TIMER));
	lapicw(TICR, 10000000);

	// 保持 BSP 的 LINT0 置位，以便它可以从 8259A 芯片获得中断.
	//
	// 根据 Intel MP 规范[MP Specification]，BIOS 应该在 Virtual Wire 模式初始化 BSP 的 Local APIC
	// 其中 8259A 的 INTR 实际上连接到 BSP 的 LINTIN0
	// 在这种模式下，我们不需要对 IOAPIC 进行编程.
	if (thiscpu != bootcpu)
		lapicw(LINT0, MASKED);

	// 在所有 CPU 上禁用 NMI (LINT1)
	lapicw(LINT1, MASKED);

	// 在提供该中断入口的机器上禁用性能计数器溢出中断.
	if (((lapic[VER] >> 16) & 0xFF) >= 4)
		lapicw(PCINT, MASKED);

	// 将错误中断映射到 IRQ_ERROR.
	lapicw(ERROR, IRQ_OFFSET + IRQ_ERROR);

	// 清除错误状态寄存器(需要反向写入).
	lapicw(ESR, 0);
	lapicw(ESR, 0);

	// 确认所有未完成的中断.
	lapicw(EOI, 0);

	// 发送Init Level De-Assert 同步仲裁 ID.
	lapicw(ICRHI, 0);
	lapicw(ICRLO, BCAST | INIT | LEVEL);
	while (lapic[ICRLO] & DELIVS)
		;

	// 在 APIC 上启用中断(但不在处理器上).
	lapicw(TPR, 0);
}

int cpunum(void)
{
	if (lapic)
		return lapic[ID] >> 24;
	return 0;
}

// 中断应答.
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

// Start additional processor running entry code at addr.
// See Appendix B of MultiProcessor Specification.
void lapic_startap(uint8_t apicid, uint32_t addr)
{
	int i;
	uint16_t *wrv;

	// "The BSP must initialize CMOS shutdown code to 0AH
	// and the warm reset vector (DWORD based at 40:67) to point at
	// the AP startup code prior to the [universal startup algorithm]."
	outb(IO_RTC, 0xF); // offset 0xF is shutdown code
	outb(IO_RTC + 1, 0x0A);
	wrv = (uint16_t *)KADDR((0x40 << 4 | 0x67)); // Warm reset vector
	wrv[0] = 0;
	wrv[1] = addr >> 4;

	// "Universal startup algorithm."
	// Send INIT (level-triggered) interrupt to reset other CPU.
	lapicw(ICRHI, apicid << 24);
	lapicw(ICRLO, INIT | LEVEL | ASSERT);
	microdelay(200);
	lapicw(ICRLO, INIT | LEVEL);
	microdelay(100); // should be 10ms, but too slow in Bochs!

	// Send startup IPI (twice!) to enter code.
	// Regular hardware is supposed to only accept a STARTUP
	// when it is in the halted state due to an INIT.  So the second
	// should be ignored, but it is part of the official Intel algorithm.
	// Bochs complains about the second one.  Too bad for Bochs.
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
