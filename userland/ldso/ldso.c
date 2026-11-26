// SPDX-License-Identifier: Apache-2.0

/* ld.so -- this system's dynamic linker (userland/ldso).
 *
 * The kernel maps a PT_INTERP image alongside the program, fills in an
 * auxiliary vector describing both, and jumps here.  Everything after that
 * is this file's job: read the program's PT_DYNAMIC, load every DT_NEEDED
 * object at a base of our choosing, apply every relocation, and return the
 * program's entry point for entry.S to jump to.
 *
 * Two deliberate choices keep it small:
 *
 *   - It is position-independent but needs no relocation of its own.
 *     Linked as an ET_DYN image with nothing to link against, it carries
 *     no PT_DYNAMIC and no dynamic relocation, so there is no
 *     self-referential bootstrap to solve: every address in here is
 *     already correct the moment the kernel jumps to it.  AT_BASE reports
 *     where the kernel put it, and is not needed -- a loader that must
 *     relocate itself before it can relocate anything else is one broken
 *     instruction away from not running at all.
 *
 *   - Binding is eager.  Every JUMP_SLOT is filled in while loading, so the
 *     PLT's lazy-resolver path is never entered and no link_map state
 *     exists to maintain.  The PLT still ships -- it is what the program's
 *     calls go through -- it simply never falls back.
 *
 * A shared object is mapped straight from its file: mmap supplies the pages
 * on demand, so the only bytes ever read into memory here are the ELF
 * header and the program header table.
 *
 * Nothing in this file may link against libulib.so -- it is what ld.so
 * exists to load.  It uses ulib's syscall wrappers (static inline in the
 * header) and xstrlen from the static ulib.o, and nothing else.
 */
#include "ulib.h"
#include <types.h> /* PAGE_SIZE and the alignment macros: the same values
                    * the kernel maps with, so a segment cannot be placed
                    * one way here and rounded another way there. */
#include <elf.h>

/* The program plus its DT_NEEDED closure.  Eight is far more than any link
 * in this tree needs, and it is a hard stop rather than an overrun. */
#define MAX_OBJS 8

/* Where unqualified sonames are found.  There is no search path and no
 * LD_LIBRARY_PATH: a DT_NEEDED name resolves here or fails. */
#define LIB_DIR "/lib/"

struct object {
    char path[48];
    u64 base; /* load bias: link address 0 lands here, so every address
               * in the object is base + its link address */
    const Elf64_Dyn *dyn;
    const Elf64_Sym *syms;
    const char *strtab;
    u64 nsyms; /* DT_HASH's nchain -- the only count this ABI can offer */
    const Elf64_Rela *rela;
    u64 nrela;
    const Elf64_Rela *plt;
    u64 nplt;
};

static struct object g_obj[MAX_OBJS];
static int g_nobj;

/* Program header table of the object currently being loaded.  128 entries
 * is 7 KiB -- far more than the 16 KiB stack can spare -- and only one
 * object is ever read at a time. */
static Elf64_Phdr g_ph[128];

/* ---- small helpers --------------------------------------------------- *
 * No libc exists to reach for, and the compiler may lower memcmp/memcpy
 * into calls that would then be undefined, so everything here is written
 * out rather than borrowed. */

__attribute__((noreturn)) static void fail(const char *what, const char *arg)
{
    kwrite(2, "ld.so: ", 7);
    kwrite(2, what, xstrlen(what));
    if (arg) {
        kwrite(2, " ", 1);
        kwrite(2, arg, xstrlen(arg));
    }
    kwrite(2, "\n", 1);
    kexit(127);
    for (;;) { } /* SYS_exit does not return; this keeps noreturn honest */
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    while (src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

/* Decimal rendering for diagnostics: xprintf() writes to stdout on its own,
 * and a failed load has to reach stderr as one line. */
static void dec(char *dst, u64 v)
{
    char tmp[24];
    int n = 0;
    if (!v) tmp[n++] = '0';
    while (v) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    int i = 0;
    while (n) dst[i++] = tmp[--n];
    dst[i] = 0;
}

/* Resolve a DT_NEEDED soname: an absolute name is taken as-is, anything
 * else is looked for beside this loader. */
static void child_path(char *dst, size_t cap, const char *name)
{
    size_t i = 0;
    if (name[0] != '/') {
        const char *p = LIB_DIR;
        while (*p && i + 1 < cap) dst[i++] = *p++;
    }
    while (*name && i + 1 < cap) dst[i++] = *name++;
    dst[i] = 0;
}

/* ---- symbol resolution ----------------------------------------------- */

/* First definition of `name` anywhere in the link, searching the program
 * first and then the objects it pulled in, which is the scope a static
 * linker would have resolved against.  Returns 0 when nobody defines it --
 * the caller turns that into a named error rather than a call through a
 * zero slot. */
static u64 lookup(const char *name)
{
    for (int i = 0; i < g_nobj; i++) {
        const struct object *o = &g_obj[i];
        if (!o->syms || !o->strtab || !o->nsyms) continue;
        /* Index 0 is the ELF-mandated undefined placeholder, never a
         * definition. */
        for (u64 j = 1; j < o->nsyms; j++) {
            const Elf64_Sym *s = &o->syms[j];
            if (s->st_shndx == SHN_UNDEF) continue;
            if (streq(name, o->strtab + s->st_name)) return o->base + s->st_value;
        }
    }
    return 0;
}

static void apply(struct object *o, const Elf64_Rela *r)
{
    u32 type = ELF64_R_TYPE(r->r_info);
    u64 *where = (u64 *)(o->base + r->r_offset);

    switch (type) {
    case R_X86_64_RELATIVE:
        /* No symbol involved: the object fixing up its own pointers, which
         * is the only relocation an ET_DYN image needs for its own data. */
        *where = o->base + (u64)r->r_addend;
        break;
    case R_X86_64_GLOB_DAT:
    case R_X86_64_JUMP_SLOT:
    case R_X86_64_64: {
        u32 idx = ELF64_R_SYM(r->r_info);
        /* nsyms comes from DT_HASH, so an object that carries relocations
         * but no hash table cannot be checked -- and cannot be searched
         * either, which is why every object in this link is built with
         * --hash-style=sysv. */
        if (!o->syms || !o->strtab || idx >= o->nsyms)
            fail("relocation names a symbol outside the table", o->path);
        const Elf64_Sym *s = &o->syms[idx];
        const char *name = o->strtab + s->st_name;
        u64 v = lookup(name);
        if (!v) fail("undefined symbol", name);
        *where = (type == R_X86_64_64) ? v + (u64)r->r_addend : v;
        break;
    }
    default: {
        char nb[24];
        dec(nb, type);
        fail("unsupported relocation type", nb);
    }
    }
}

static void relocate(struct object *o)
{
    for (u64 i = 0; i < o->nrela; i++) apply(o, &o->rela[i]);
    for (u64 i = 0; i < o->nplt; i++) apply(o, &o->plt[i]);
}

/* ---- dynamic section -------------------------------------------------- */

static struct object *load_object(const char *path); /* defined below */

static void parse_dynamic(struct object *o)
{
    u64 hash = 0, strtab = 0, symtab = 0;
    u64 rela = 0, relasz = 0, jmprel = 0, pltrelsz = 0;
    u64 needed[4];
    int nneeded = 0;

    for (const Elf64_Dyn *d = o->dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_NEEDED:
            /* d_val is an offset into .dynstr, not an address. */
            if (nneeded < (int)(sizeof needed / sizeof needed[0]))
                needed[nneeded++] = d->d_un.d_val;
            break;
        /* Every pointer tag is relative to the object's link base, which
         * is what `base` is: 0 for a program linked at absolute addresses,
         * the load bias for anything linked at 0. */
        case DT_HASH:     hash = o->base + d->d_un.d_ptr; break;
        case DT_STRTAB:   strtab = o->base + d->d_un.d_ptr; break;
        case DT_SYMTAB:   symtab = o->base + d->d_un.d_ptr; break;
        case DT_RELA:     rela = o->base + d->d_un.d_ptr; break;
        case DT_RELASZ:   relasz = d->d_un.d_val; break;
        case DT_JMPREL:   jmprel = o->base + d->d_un.d_ptr; break;
        case DT_PLTRELSZ: pltrelsz = d->d_un.d_val; break;
        default: break; /* DT_DEBUG, DT_GNU_HASH, flags: nothing to do */
        }
    }

    o->strtab = (const char *)strtab;
    o->syms = (const Elf64_Sym *)symtab;
    /* The sysv hash header's nchain IS the symbol count.  DT_GNU_HASH
     * cannot be counted this way, which is the whole reason the link uses
     * --hash-style=sysv: without a count there is no way to search an
     * object, and a missing symbol would surface as a call through a zero
     * slot instead of a name. */
    o->nsyms = hash ? ((const Elf64_Hash *)hash)->nchain : 0;
    o->rela = (const Elf64_Rela *)rela;
    o->nrela = relasz / sizeof(Elf64_Rela);
    o->plt = (const Elf64_Rela *)jmprel;
    o->nplt = pltrelsz / sizeof(Elf64_Rela);

    /* Dependencies first: every object has to be mapped before any
     * relocation can name a symbol living in one.  load_object() records
     * the object before recursing, so a cycle terminates. */
    for (int i = 0; i < nneeded; i++) {
        char child[64];
        child_path(child, sizeof child, o->strtab + needed[i]);
        load_object(child);
    }
}

/* ---- loading ---------------------------------------------------------- */

static struct object *load_object(const char *path)
{
    for (int i = 0; i < g_nobj; i++)
        if (streq(g_obj[i].path, path)) return &g_obj[i];
    if (g_nobj == MAX_OBJS) fail("too many shared objects", path);

    int fd = kopen(path, 0); /* O_RDONLY: this kernel's open takes no mode */
    if (fd < 0) fail("cannot open", path);

    Elf64_Ehdr eh;
    if (kread(fd, &eh, sizeof eh) != (long)sizeof eh) fail("truncated ELF header", path);
    if (eh.e_ident[0] != 0x7f || eh.e_ident[1] != 'E' || eh.e_ident[2] != 'L' ||
        eh.e_ident[3] != 'F' || eh.e_ident[4] != 2)
        fail("not an ELF64 object", path);
    if (eh.e_type != 3) /* ET_DYN: a shared object, not a program */
        fail("not a shared object", path);
    if (eh.e_phnum == 0 || eh.e_phnum > 128) fail("implausible header count", path);

    /* The whole table in one read: a short read IS the bounds check -- it
     * means the file ends inside the table. */
    size_t phbytes = (size_t)eh.e_phnum * sizeof(Elf64_Phdr);
    if (klseek(fd, (long)eh.e_phoff, LNXRM_SEEK_SET) < 0 ||
        kread(fd, g_ph, phbytes) != (long)phbytes)
        fail("truncated program headers", path);

    /* Span the object occupies.  ET_DYN is linked at 0, so this is also
     * the size of the hole that has to be found free. */
    u64 lo = 0, hi = 0;
    int seen = 0;
    for (int i = 0; i < eh.e_phnum; i++) {
        const Elf64_Phdr *p = &g_ph[i];
        if (p->p_type != PT_LOAD) continue;
        if (p->p_memsz == 0 || p->p_vaddr + p->p_memsz <= p->p_vaddr) continue;
        u64 s = p->p_vaddr & ~(PAGE_SIZE - 1);
        u64 e = (p->p_vaddr + p->p_memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        if (!seen || s < lo) lo = s;
        if (e > hi) hi = e;
        seen = 1;
    }
    if (!seen || hi <= lo) fail("no loadable segments", path);

    /* Reserve the span in one call -- the kernel guarantees that range is
     * free -- and hand it straight back.  MAP_FIXED refuses to map over
     * anything, so every segment below has to land in genuinely empty
     * space: a collision fails loudly instead of overwriting whatever was
     * already there.  Nothing else runs between the two calls, so the hole
     * cannot be taken in between. */
    long res = kmmap((long)(hi - lo), LNXRM_PROT_NONE);
    if (res < 0) fail("cannot reserve address space", path);
    if (kmunmap(res, (long)(hi - lo)) < 0) fail("cannot release reservation", path);
    u64 base = (u64)res - lo; /* link address 0 lands at `base` */

    for (int i = 0; i < eh.e_phnum; i++) {
        const Elf64_Phdr *p = &g_ph[i];
        if (p->p_type != PT_LOAD) continue;
        if (p->p_memsz == 0 || p->p_vaddr + p->p_memsz <= p->p_vaddr) continue;

        u64 s = p->p_vaddr & ~(PAGE_SIZE - 1);
        u64 e = (p->p_vaddr + p->p_memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        int prot = 0;
        if (p->p_flags & PF_R) prot |= LNXRM_PROT_READ;
        if (p->p_flags & PF_W) prot |= LNXRM_PROT_WRITE;
        if (p->p_flags & PF_X) prot |= LNXRM_PROT_EXEC;

        /* offset is the file's page containing p_offset; because ELF keeps
         * p_vaddr ≡ p_offset (mod page), the byte at p_offset still lands
         * at base + p_vaddr.  Bytes past EOF read as zero, which is how
         * the BSS tail of a segment gets filled. */
        if (kmmap_fixed((long)(base + s), (long)(e - s), prot,
                        LNXRM_MAP_PRIVATE | LNXRM_MAP_FIXED, fd,
                        (long)(p->p_offset & ~(PAGE_SIZE - 1))) < 0)
            fail("cannot map segment", path);
    }
    kclose(fd);

    struct object *o = &g_obj[g_nobj++];
    copy_str(o->path, sizeof o->path, path);
    o->base = base;
    o->dyn = 0;
    o->syms = 0;
    o->strtab = 0;
    o->nsyms = 0;
    o->rela = 0;
    o->nrela = 0;
    o->plt = 0;
    o->nplt = 0;

    /* PT_DYNAMIC sits inside a segment that is now mapped, so the object's
     * own metadata is read from memory rather than from the file. */
    for (int i = 0; i < eh.e_phnum; i++)
        if (g_ph[i].p_type == PT_DYNAMIC) {
            o->dyn = (const Elf64_Dyn *)(base + g_ph[i].p_vaddr);
            break;
        }
    if (!o->dyn) fail("shared object has no PT_DYNAMIC", path);

    parse_dynamic(o);
    return o;
}

/* ---- entry ------------------------------------------------------------- */

/* Runs with the kernel-built stack below us (see entry.S).  Returns the
 * program's entry point; entry.S restores the original stack pointer and
 * jumps to it, so the program reads argc from [rsp] exactly as execve
 * laid it out. */
long ldso_main(long argc, char **argv, char **envp, const u64 *auxv)
{
    (void)argc;
    (void)argv;
    (void)envp;

    /* The auxiliary vector is a (type, value) list ending at AT_NULL.
     * Six entries carry everything needed: where the program's header
     * table was mapped, how wide each entry is, how many there are, the
     * page size every segment is aligned to, where execution resumes once
     * the link is done, and where the kernel put this loader. */
    u64 phdr = 0, phent = 0, phnum = 0, pagesz = 0, entry = 0, base_self = 0;
    for (const u64 *a = auxv; a[0] != LNXRM_AT_NULL; a += 2) {
        switch (a[0]) {
        case LNXRM_AT_PHDR:  phdr = a[1]; break;
        case LNXRM_AT_PHENT: phent = a[1]; break;
        case LNXRM_AT_PHNUM: phnum = a[1]; break;
        case LNXRM_AT_PAGESZ: pagesz = a[1]; break;
        case LNXRM_AT_ENTRY: entry = a[1]; break;
        case LNXRM_AT_BASE:  base_self = a[1]; break;
        default: break;
        }
    }
    (void)base_self; /* see the header: this loader relocates nothing */
    if (!phdr || !phnum || !entry)
        fail("no usable auxiliary vector (need AT_PHDR/AT_PHNUM/AT_ENTRY)", 0);
    if (phent != sizeof(Elf64_Phdr)) fail("unexpected program header size", 0);
    /* Every segment below is placed and page-rounded with this loader's own
     * PAGE_SIZE, so a kernel that rounded differently would have to be told
     * rather than quietly produce misaligned mappings. */
    if (pagesz != PAGE_SIZE) fail("unsupported page size", 0);

    /* Object zero is the program: the kernel mapped it, but at a bias only
     * the kernel chose.  AT_PHDR is where the header table LANDS and
     * PT_PHDR.p_vaddr is where it is LINKED, so their difference is the
     * program's load bias -- the one number the program's own bytes cannot
     * yield, and the reason AT_PHDR exists.  Every address read out of the
     * program below is a link address and has to have this added. */
    const Elf64_Phdr *pph = (const Elf64_Phdr *)phdr;
    u64 pbias = 0;
    int have_phdr_seg = 0;
    for (u64 i = 0; i < phnum; i++)
        if (pph[i].p_type == PT_PHDR) {
            if (phdr < pph[i].p_vaddr) fail("program header table below its link address", 0);
            pbias = phdr - pph[i].p_vaddr;
            have_phdr_seg = 1;
            break;
        }
    if (!have_phdr_seg)
        fail("program has no PT_PHDR (cannot recover its load bias)", 0);

    struct object *prog = &g_obj[g_nobj++];
    copy_str(prog->path, sizeof prog->path, "(program)");
    prog->base = pbias;
    prog->dyn = 0;
    prog->syms = 0;
    prog->strtab = 0;
    prog->nsyms = 0;
    prog->rela = 0;
    prog->nrela = 0;
    prog->plt = 0;
    prog->nplt = 0;

    for (u64 i = 0; i < phnum; i++)
        if (pph[i].p_type == PT_DYNAMIC) {
            prog->dyn = (const Elf64_Dyn *)(pbias + pph[i].p_vaddr);
            break;
        }
    if (!prog->dyn) fail("program has no PT_DYNAMIC", 0);

    parse_dynamic(prog); /* maps DT_NEEDED, which appends the shared objects */

    /* Eager binding: everything is resolved before the program runs, so no
     * relocation can be reached half-applied. */
    for (int i = 0; i < g_nobj; i++) relocate(&g_obj[i]);

    return (long)entry;
}
