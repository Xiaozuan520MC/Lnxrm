#pragma once
#include <types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ELF64 (usr/include flavor kept minimal) */
typedef struct {
    u8 e_ident[16];
    u16 e_type, e_machine;
    u32 e_version;
    u64 e_entry;
    u64 e_phoff;
    u64 e_shoff;
    u32 e_flags;
    u16 e_ehsize, e_phentsize, e_phnum;
    u16 e_shentsize, e_shnum, e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    u32 p_type;
    u32 p_flags;
    u64 p_offset;
    u64 p_vaddr;
    u64 p_paddr;
    u64 p_filesz;
    u64 p_memsz;
    u64 p_align;
} Elf64_Phdr;

#define PT_LOAD 1
#define PF_X    1 /* segment may be executed: mapped without PG_NX */
#define PF_W    2
#define PF_R    4

/* kernel/elf.c -- load a static ELF64 into address space `pml4`.
 * Returns entry point or 0 on failure; sets *brk_end to first free byte. */
u64 elf_load(u64 pml4, const void *img, size_t imgsize, u64 *brk_end);

#ifdef __cplusplus
}
#endif
