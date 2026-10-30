//extern sem_t sem_load_elf;
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    void *img;
} program_img;

typedef struct {
    void *start;
    void *end;
    void *entry;
    uint32_t offset;
    uint32_t size;
} exec_img;

#define E_NOT_ELF_X       1
#define E_NOT_COMPATIBLE  2

typedef uint32_t elf32_addr;  // unsigned address
typedef uint32_t elf32_off;   // unsigned 4-byte integer
typedef uint16_t elf32_half;  // unsigned 2-byte integer
typedef uint32_t elf32_word;  // unsigned 4-byte integer
typedef int32_t  elf32_sword; // signed 4-byte integer
typedef uint8_t  elf32_byte;  // unsigned byte

typedef struct {
    elf32_byte e_ident[16]; // machine-independent identification info
    elf32_half e_type;      // object file type
    elf32_half e_machine;   // architecture
    elf32_word e_version;   // object file standard version
    elf32_addr e_entry;     // entry point
    elf32_off  e_phoff;     // program header table's offset
    elf32_off  e_shoff;     // section header table's offset
    elf32_word e_flags;     // processor-specific flags
    elf32_half e_ehsize;    // ELF header size
    elf32_half e_phentsize; // program header entry size (all program headers are the same size)
    elf32_half e_phnum;     // number of entries in the program header table
    elf32_half e_shentsize; // section header entry size (all section headers are the same size)
    elf32_half e_shnum;     // number of entries in the section header table
    elf32_half e_shstrndx;  // string table section index
} ELF32_hdr;
#define EID0        0x7f // ELF format magic number
#define EV_CURRENT  1    // ELF format version
#define EC_32       1    // 32-bit arch
#define EM_ARM      0x28 // for ARM CPUs
#define EM_RISCV    0xF3 // for RISC-V CPUs
#define ET_EXEC     2    // type: normal executable
#define ET_DYN      3    // type: PIE (or a shared library, but shared libraries are useless now)

typedef struct {
    elf32_word sh_name;      // index into the section name string table
    elf32_word sh_type;
    elf32_word sh_flags;
    elf32_addr sh_addr;      // address in the memory image that this should appear at
    elf32_off  sh_off;       // offset from the file start to the section
    elf32_word sh_size;      // section size (in file, unless it is type SH_NOBITS - like .bss)
    elf32_word sh_link;      // type-dependent
    elf32_word sh_info;      // type-dependent
    elf32_word sh_addralign; // alignment constraints (sh_addr needs to be a multiple of this)
    elf32_word sh_entsize;   // if it is a table of fixed-size entries, then this is that size
} ELF32_shdr;

enum sh_type {
    SHT_NULL     =  0, // UNDEF
    SHT_PROGBITS =  1, // program info
    SHT_SYMTAB   =  2, // symbol table
    SHT_STRTAB   =  3, // string table
    SHT_RELA     =  4, // relocation (with addend)
    SHT_HASH     =  5, // symbol hash table
    SHT_DYNAMIC  =  6, // info for dynamic linking
    SHT_NOTE     =  7,
    SHT_NOBITS   =  8, // not present in the file
    SHT_REL      =  9, // relocation (no addend)
    SHT_SHLIB    = 10, // no specified semantics
    SHT_DYNSYM   = 11, // dynamic symbols table
};
enum sh_flags {
    SHF_WRITE = 0x1,
    SHF_ALLOC = 0x2,
    SHF_EXEC  = 0x4,
};

#define ALIGN(to, x)  ((x+(to)-1) & ~((to)-1))
extern bool is_elf_x(ELF32_hdr *hdr);
extern  bool is_compatible(ELF32_hdr *hdr);
//extern int load_elf( program_img *prg, exec_img *res);

extern uint32 elf_execve(const char *file, exec_img *res);
#define EI_NIDENT 16

#define PT_NULL     0
#define PT_LOAD     1
#define PT_DYNAMIC  2
#define PT_INTERP   3
#define PT_NOTE     4
#define PT_SHLIB    5
#define PT_PHDR     6
#define PT_RESERVED 0x70000000

#define ELF32_ST_BIND(i)    ((i) >> 4)
#define ELF32_ST_TYPE(i)    ((i) & 0xF)
#define ELF32_ST_INFO(b, t) (((b) << 4) + ((t) & 0xF))

#define ELF32_R_SYM(i)     ((i) >> 8)
#define ELF32_R_TYPE(i)    ((i) & 0xFF)
#define ELF32_R_INFO(s, t) (((s) << 8) + ((t) & 0xFF))


typedef struct {
    elf32_word    st_name;
    elf32_addr    st_value;
    elf32_word    st_size;
    unsigned char st_info;
    unsigned char st_other;
    elf32_half    st_shndx;
} __attribute__((packed)) Elf32_Sym;

typedef struct {
    elf32_addr r_offset;
    elf32_word r_info;
} __attribute__((packed)) Elf32_Rel;

typedef struct {
    elf32_addr  r_offset;
    elf32_word  r_info;
    elf32_sword r_addend;
} __attribute__((packed)) Elf32_Rela;

typedef struct {
    elf32_word p_type;
    elf32_off  p_offset;
    elf32_addr p_vaddr;
    elf32_addr p_paddr;
    elf32_word p_filesz;
    elf32_word p_memsz;
    elf32_word p_flags;
    elf32_word p_align;
} __attribute__((packed)) Elf32_Phdr;