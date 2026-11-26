// SPDX-License-Identifier: Apache-2.0

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

#define PT_LOAD    1
#define PT_DYNAMIC 2 /* where the dynamic linking metadata lives */
#define PT_INTERP  3 /* path of the program's interpreter (the ldso) */
#define PT_PHDR    6 /* the program header table itself, if mapped */
#define PF_X       1 /* segment may be executed: mapped without PG_NX */
#define PF_W       2
#define PF_R       4

/* e_type values this loader accepts.  ET_EXEC is linked at absolute
 * addresses and is mapped where it says; ET_DYN is linked at 0 and is
 * mapped at a load bias the kernel picks (elf_place).  Everything else --
 * ET_REL, ET_CORE, a value nobody assigned -- is not an image to run. */
#define ET_EXEC 2
#define ET_DYN  3

/* ---- dynamic linking ------------------------------------------------ *
 * The kernel reads PT_INTERP and nothing else from this half: everything
 * below exists for userland's loader (userland/ldso), which walks the
 * dynamic section, loads DT_NEEDED objects and applies relocations.  It
 * shares this header so the two cannot drift apart on structure layout --
 * a wrong Elf64_Dym size would be a silent stack-smash in ldso, not a
 * compile error.                                              */

typedef struct {
    u64 d_tag; /* DT_* below */
    union {
        u64 d_val;
        u64 d_ptr; /* interpreted as a virtual address, load bias included */
    } d_un;
} Elf64_Dyn;

typedef struct {
    u32 st_name;  /* offset into .dynstr */
    u8 st_info;   /* binding << 4 | type */
    u8 st_other;  /* visibility */
    u16 st_shndx; /* SHN_UNDEF when the definition lives elsewhere */
    u64 st_value;
    u64 st_size;
} Elf64_Sym;

typedef struct {
    u64 r_offset; /* where to write the answer (a virtual address) */
    u64 r_info;   /* symbol index << 32 | relocation type */
    i64 r_addend;
} Elf64_Rela;

/* Dynamic section tags.  Only the ones a loader actually consumes; the
 * numbering is the ELF spec's, not ours. */
#define DT_NULL     0
#define DT_NEEDED   1  /* a shared object this one depends on */
#define DT_PLTRELSZ 2  /* bytes of .rela.plt */
#define DT_PLTGOT   3  /* address of the GOT's PLT part */
#define DT_HASH     4  /* sysv hash: gives a countable symbol table */
#define DT_STRTAB   5
#define DT_SYMTAB   6
#define DT_RELA     7  /* .rela.dyn */
#define DT_RELASZ   8
#define DT_RELAENT  9  /* must be sizeof(Elf64_Rela) */
#define DT_STRSZ    10
#define DT_SYMENT   11 /* must be sizeof(Elf64_Sym) */
#define DT_PLTREL   20 /* DT_RELA or DT_REL, as a DT_* tag */
#define DT_JMPREL   23 /* .rela.plt */
#define DT_GNU_HASH 0x6ffffef5 /* lld's default; not usable for a count */

/* x86-64 relocation types this loader applies.  RELATIVE carries no
 * symbol: it is `*where += load_bias`, which is how an ET_DYN object fixes
 * up its own internal pointers.  GLOB_DAT/JUMP_SLOT look a name up in the
 * global scope -- GOT slots and PLT slots respectively. */
#define R_X86_64_64       1
#define R_X86_64_GLOB_DAT 6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE 8

#define ELF64_R_SYM(i)  ((u32)((i) >> 32))
#define ELF64_R_TYPE(i) ((u32)(i))
#define ELF64_ST_BIND(i) ((i) >> 4)
#define ELF64_ST_TYPE(i) ((i) & 0xf)

#define STB_LOCAL  0
#define STB_GLOBAL 1
#define STT_NOTYPE 0
#define STT_OBJECT 1
#define STT_FUNC   2
#define SHN_UNDEF  0

/* sysv hash header sitting at DT_HASH: nbucket, nchain, buckets[],
 * chains[].  nchain IS the symbol count, which is what makes a linear
 * name scan possible at all when the object carries no GNU hash. */
typedef struct {
    u32 nbucket;
    u32 nchain;
    /* u32 bucket[nbucket]; u32 chain[nchain]; -- flexible tail */
} Elf64_Hash;

/* kernel/sched/elf.c -- load an ET_EXEC image into address space `pml4`
 * at the addresses it is linked at (load bias 0).  Returns the entry point
 * or 0 on failure; sets *brk_end to the first free byte.  ET_DYN images
 * are refused here -- they need a bias, which is elf_load_dyn()'s job. */
u64 elf_load(u64 pml4, const void *img, size_t imgsize, u64 *brk_end);

/* Load an ET_DYN image into `pml4` at load bias `bias`: segment p_vaddr
 * lands at bias + p_vaddr, and the returned entry point has the bias
 * applied.  `bias` comes from elf_place(); an ET_EXEC image is refused
 * (it cannot be relocated) and so is a bias that would push any part of
 * the image outside user space.  Returns 0 on failure; *brk_end is only
 * written on success. */
u64 elf_load_dyn(u64 pml4, const void *img, size_t imgsize, u64 bias, u64 *brk_end);

/* Where `img` will be loaded, so two images can be proved disjoint before
 * either is mapped.  `*cursor` is the first user address not yet claimed;
 * on success it is advanced past this image.  Returns the image's ELF type
 * (ET_EXEC or ET_DYN, which is also which loader to call), and fills *bias
 * with the load bias, plus `lo` and `hi` with the span in LINK addresses --
 * add *bias to either end for the span as it will be mapped (for ET_EXEC the
 * bias is 0, so they already are).  Returns -1 for an image that is not a
 * loadable ELF64, that has no PT_LOAD, or that cannot be placed at *cursor. */
int elf_place(const void *img, size_t imgsize, u64 *cursor, u64 *bias, u64 *lo,
              u64 *hi);

/* Path of PT_INTERP, NUL-terminated into `out` (capacity `outsz`).
 * Returns 1 when the image names an interpreter, 0 for a static image,
 * and -1 when the segment is malformed (truncated, absurdly long): the
 * caller must turn -1 into ENOEXEC rather than silently running the
 * program without the loader it asked for.  `img` is the whole file, so
 * the path is read straight out of it. */
int elf_interp_path(const void *img, size_t imgsize, char *out, size_t outsz);

/* Program-header facts execve publishes in the auxiliary vector.  `vaddr`
 * is the table's virtual address in the image's OWN address space: add the
 * load bias from elf_place() to get AT_PHDR (an ET_EXEC image has bias 0,
 * so for it `vaddr` already is the answer).  It is 0 when no PT_LOAD covers
 * the table -- a `-Ttext` link can put the ELF header below USER_BASE,
 * where elf_load skips it, and a program built that way simply has no
 * AT_PHDR to offer.  Returns 0, or -1 when `img` is not a usable ELF64. */
struct elf_phdrs {
    u64 vaddr;    /* table address in link space; + bias = AT_PHDR */
    u64 entsize;  /* AT_PHENT */
    u64 count;    /* AT_PHNUM */
};
int elf_get_phdrs(const void *img, size_t imgsize, struct elf_phdrs *out);

/* Memory span the image's PT_LOAD segments occupy, in LINK addresses:
 * [*lo, *hi), page aligned, or -1 when it has none.  Add the load bias
 * from elf_place() for the span as mapped (ET_EXEC has bias 0).
 * execve loads a program and its interpreter into ONE address space, and
 * elf_load() reuses a page that is already mapped instead of failing --
 * so two images whose spans overlap would have their bytes silently merged
 * rather than one of them refused.  This is what proves they cannot: a
 * segment is counted when SOME legal bias could map it, so the answer can
 * only ever over-estimate, never certify a real collision as safe. */
int elf_image_range(const void *img, size_t imgsize, u64 *lo, u64 *hi);

#ifdef __cplusplus
}
#endif
