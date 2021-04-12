/* multiboot.h - Multiboot头部 */

#define APPEND_HILO(hi, lo) (((uint64_t)hi << 32) + lo)

/* Types. */

#define MB_TYPE_USABLE 1
#define MB_TYPE_RESERVED 2
#define MB_TYPE_ACPI_RECLM 3
#define MB_TYPE_ACPI_NVS 4
#define MB_TYPE_BAD 5

#define MB_FLAG_MMAP 0x40

/* The Multiboot header. */
typedef struct multiboot_header
{
    uint32_t magic;
    uint32_t flags;
    uint32_t checksum;
    uint32_t header_addr;
    uint32_t load_addr;
    uint32_t load_end_addr;
    uint32_t bss_end_addr;
    uint32_t entry_addr;
} multiboot_header_t;

/* The symbol table for a.out. */
typedef struct aout_symbol_table
{
    uint32_t tabsize;
    uint32_t strsize;
    uint32_t addr;
    uint32_t reserved;
} aout_symbol_table_t;

/* The section header table for ELF. */
typedef struct elf_section_header_table
{
    uint32_t num;
    uint32_t size;
    uint32_t addr;
    uint32_t shndx;
} elf_section_header_table_t;

/* The Multiboot information. */
typedef struct multiboot_info
{
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    union
    {
        aout_symbol_table_t aout_sym;
        elf_section_header_table_t elf_sec;
    } u;
    uint32_t mmap_length;
    uint32_t mmap_addr;
} multiboot_info_t;

/* The module structure. */
typedef struct module
{
    uint32_t mod_start;
    uint32_t mod_end;
    uint32_t string;
    uint32_t reserved;
} module_t;

/* 内存映射. 请注意，偏移量0是base_addr_low，但没有大小. */
typedef struct memory_map
{
    uint32_t size;
    uint32_t base_addr_low;
    uint32_t base_addr_high;
    uint32_t length_low;
    uint32_t length_high;
    uint32_t type;
} memory_map_t;

uint32_t restrictive_type(uint32_t t1, uint32_t t2)
{
    if (t1 == MB_TYPE_BAD || t2 == MB_TYPE_BAD)
        return MB_TYPE_BAD;
    else if (t1 == MB_TYPE_ACPI_NVS || t2 == MB_TYPE_ACPI_NVS)
        return MB_TYPE_ACPI_NVS;
    else if (t1 == MB_TYPE_RESERVED || t2 == MB_TYPE_RESERVED)
        return MB_TYPE_RESERVED;
    else if (t1 == MB_TYPE_ACPI_RECLM || t2 == MB_TYPE_ACPI_RECLM)
        return MB_TYPE_ACPI_RECLM;

    return MB_TYPE_USABLE;
}
