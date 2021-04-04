#ifndef JOS_INC_ELF_H
#define JOS_INC_ELF_H

#define ELF_MAGIC 0x464C457FU /* "\x7FELF" 的小端模式(低位优先)的十六进制数 */

/**
 * .stab 程序报错时可以提供错误信息
 * .bss 未初始化的全局变量，不会再磁盘有存储空间，全部默认为0，加载时需要初始化这部分空间为0
 * 关键字段
 * entry 是可执行程序的入口地址，从内存的这个地址开始执行(虚拟地址/链接地址)
 * e_phoff, p_phnum 可以用来找到所有的程序头表项
 * e_phoff: 程序头表的第一项相对于 ELF 文件的基址的偏移
 * e_phnum: 表项的个数
 */
struct Elf
{
	uint32_t e_magic;	  // 标识文件是否为 ELF 文件，与 Linux 不同，魔数不在数组中
	uint8_t e_elf[12];	  // 其他相关信息
	uint16_t e_type;	  // 文件类型
	uint16_t e_machine;	  // 针对体系结构
	uint32_t e_version;	  // 版本信息
	uint64_t e_entry;	  // ***entry point 程序入口点
	uint64_t e_phoff;	  // ***程序头表偏移量
	uint64_t e_shoff;	  // ***节头表偏移量
	uint32_t e_flags;	  // 处理器特定标志
	uint16_t e_ehsize;	  // 文件头长度
	uint16_t e_phentsize; // 程序头部长度
	uint16_t e_phnum;	  // ***程序头部个数
	uint16_t e_shentsize; // 节头部长度
	uint16_t e_shnum;	  // ***节头部个数
	uint16_t e_shstrndx;  // 节头部字符索引
};

/**
 * 程序头表实际将文件的内容分成了好几个段，每个表项就代表了一个段(可能包含几个节)
 * 通过 p_offset 可以找到该段在磁盘中的位置，通过 p_va 可知直到应该把这个段放到内存中的哪个位置
 * p_filesz, p_memsz: .bss这种节在硬盘没有存储空间，而在内存中程序需要为其分配空间
 */
struct Proghdr
{
	uint32_t p_type;   // 段类型
	uint32_t p_flags;  // 段标志
	uint64_t p_offset; // ***段位置相对于文件基址的偏移量
	uint64_t p_va;	   // ***段在内存中的地址(虚拟地址)
	uint64_t p_pa;	   // 段的物理地址
	uint64_t p_filesz; // ***段在文件中的长度
	uint64_t p_memsz;  // ***段在内存中的长度
	uint64_t p_align;  // 段在内存中的对齐标志
};

/**
 * 节头表: 让程序能够找到特定的某一节
 */
struct Secthdr
{
	uint32_t sh_name;	   // 节名称
	uint32_t sh_type;	   // 节类型
	uint64_t sh_flags;	   // 节标志
	uint64_t sh_addr;	   // 节在内存中的个虚拟地址
	uint64_t sh_offset;	   // 相对于文件首部的偏移
	uint64_t sh_size;	   // 节大小(字节数)
	uint32_t sh_link;	   // 与其它节的关系
	uint32_t sh_info;	   // 其它信息
	uint64_t sh_addralign; // 字节对齐标志
	uint64_t sh_entsize;   // 表项大小
};

// Proghdr::p_type 的默认值
#define ELF_PROG_LOAD 1

// Proghdr::p_flags 的标志位
#define ELF_PROG_FLAG_EXEC 1
#define ELF_PROG_FLAG_WRITE 2
#define ELF_PROG_FLAG_READ 4

// Secthdr::sh_type 的值
#define ELF_SHT_NULL 0
#define ELF_SHT_PROGBITS 1
#define ELF_SHT_SYMTAB 2
#define ELF_SHT_STRTAB 3

// Secthdr::sh_name 的值
#define ELF_SHN_UNDEF 0

#endif /* !ALVOS_INC_ELF_H */
