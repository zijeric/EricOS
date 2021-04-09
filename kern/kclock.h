#ifndef ALVOS_KERN_KCLOCK_H
#define ALVOS_KERN_KCLOCK_H
#ifndef ALVOS_KERNEL
#error "This is a AlvOS kernel header; user programs should not #include it"
#endif

#define IO_RTC 0x070 /* RTC 端口 */

#define MC_NVRAM_START 0xe /* start of NVRAM: offset 14 */
#define MC_NVRAM_SIZE 50   /* 50 字节 of NVRAM */

/* NVRAM 字节7 & 8:基本内存大小 */
#define NVRAM_BASELO (MC_NVRAM_START + 7) /* 低字节; RTC off. 0x15 */
#define NVRAM_BASEHI (MC_NVRAM_START + 8) /* 高字节; RTC off. 0x16 */

/* NVRAM 字节 9 & 10: 扩展内存大小 */
#define NVRAM_EXTLO (MC_NVRAM_START + 9)  /* 低字节; RTC off. 0x17 */
#define NVRAM_EXTHI (MC_NVRAM_START + 10) /* 高字节; RTC off. 0x18 */

/* NVRAM 字节 34 and 35: 扩展内存 POSTed 大小 */
#define NVRAM_PEXTLO (MC_NVRAM_START + 34) /* 低字节; RTC off. 0x30 */
#define NVRAM_PEXTHI (MC_NVRAM_START + 35) /* 高字节; RTC off. 0x31 */

/* NVRAM 字节 38 39: extended memory > 16M in blocks of 64K */
#define NVRAM_EXTGT16LO (MC_NVRAM_START + 38) /* 低字节. 0x34 */
#define NVRAM_EXTGT16HI (MC_NVRAM_START + 39) /* 高字节. 0x35 */

/* NVRAM 字节 36: current century.  (please increment in Dec99!) */
#define NVRAM_CENTURY (MC_NVRAM_START + 36) /* RTC offset 0x32 */

unsigned mc146818_read(unsigned reg);
void mc146818_write(unsigned reg, unsigned datum);

#endif
