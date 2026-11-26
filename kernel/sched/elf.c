// SPDX-License-Identifier: Apache-2.0

/* ELF64 loader for user images, in two flavours: an ET_EXEC image is
 * mapped at the addresses it is linked at (load bias 0), an ET_DYN image
 * at a load bias the caller picked with elf_place().  Everything here
 * parses bytes that came off disk, so the acceptance rules live in as few
 * places as possible -- elf_load(), elf_load_dyn() and elf_place() all
 * ask phdr_table() whether the header table is real, and all three apply
 * the same segment skip rules through load_at()/elf_image_range(). */
#include <elf.h>
#include <mm/mm.h>
#include <console.h>

/* Program header table of `img`, or NULL when it is not there.  Every
 * consumer below parses bytes that came off disk, so "is this table real?"
 * is asked in exactly one place: magic and class first, then the table's
 * own bounds (offset, count, and the multiplication that could wrap).
 * `*count` is only written on success. */
static const Elf64_Phdr *phdr_table(const void *img, size_t imgsize, int *count)
{
    if (!img || imgsize < sizeof(Elf64_Ehdr)) return NULL;
    const Elf64_Ehdr *eh = img;
    if (memcmp(eh->e_ident,
               "\x7f"
               "ELF",
               4) ||
        eh->e_ident[4] != 2)
        return NULL;
    if (eh->e_phoff > imgsize || eh->e_phnum > 128 ||
        (u64)eh->e_phnum * sizeof(Elf64_Phdr) > imgsize - eh->e_phoff)
        return NULL;
    *count = eh->e_phnum;
    return (const Elf64_Phdr *)((const u8 *)img + eh->e_phoff);
}

/* ELF64 header of `img`, or NULL when there is not one there.  A buffer
 * too short to hold a header is silently not an image (there is nothing
 * to diagnose); one that is long enough but wrong says so, because that
 * is a caller handing us something that claims to be an ELF and is not. */
static const Elf64_Ehdr *elf_header(const void *img, size_t imgsize)
{
    if (!img || imgsize < sizeof(Elf64_Ehdr)) return NULL;
    const Elf64_Ehdr *eh = img;
    if (memcmp(eh->e_ident,
               "\x7f"
               "ELF",
               4) ||
        eh->e_ident[4] != 2) {
        kprintf("[elf] not an ELF64 image\n");
        return NULL;
    }
    return eh;
}

/* Map `img` into `pml4` at load bias `bias` -- 0 for an ET_EXEC image, so
 * every address below is then exactly what the file says.  Returns the
 * entry point WITH the bias applied, or 0 on failure; *brk_end is only
 * written on success. */
static u64 load_at(u64 pml4, const void *img, size_t imgsize, u64 bias, u64 *brk_end)
{
    const Elf64_Ehdr *eh = elf_header(img, imgsize);
    if (!eh) return 0; /* NULL is not an image */

    /* The entry point is jumped to in ring 3 the moment this returns, so it
     * has to be a user address before anything is mapped -- and with a bias
     * the link address alone says nothing, it is bias + e_entry that has to
     * be in range.  (Self-test: ktest/t_elf.c feeds a wild e_entry and
     * expects a rejection.) */
    if (bias + eh->e_entry < bias) { /* the addition wrapped */
        kprintf("[elf] e_entry %#lx wraps past the load bias\n", (u64)eh->e_entry);
        return 0;
    }
    u64 entry = bias + eh->e_entry;
    if (entry < USER_BASE || entry >= USER_MAX_VMA) {
        kprintf("[elf] e_entry %#lx outside user space\n", entry);
        return 0;
    }

    u64 max_end = 0;
    /* program header table must lie entirely inside the file image.
     * phdr_table() is the one place that decides this, so elf_load,
     * elf_interp_path() and elf_get_phdrs() cannot disagree about which
     * untrusted byte ranges are real. */
    int phnum = 0;
    const Elf64_Phdr *ph = phdr_table(img, imgsize, &phnum);
    if (!ph) {
        kprintf("[elf] malformed program header table\n");
        return 0;
    }
    for (int i = 0; i < phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        /* Loaded address of this segment.  A bias that wraps it means the
         * segment can never be placed, and one that lands it outside user
         * space means it is not placed HERE -- the same "skip non-user
         * segments (.note etc.)" rule as ever, only evaluated after the
         * bias instead of on the bare link address, which for an ET_DYN
         * image linked at 0 would otherwise skip everything. */
        if (bias + ph[i].p_vaddr < bias) continue;
        u64 vaddr = bias + ph[i].p_vaddr;
        if (vaddr + ph[i].p_memsz <= vaddr || vaddr < USER_BASE || vaddr >= USER_MAX_VMA)
            continue;
        /* Reject malformed segments: p_filesz bytes are copied straight
         * out of the image, so they must fit in it, and p_filesz may never
         * exceed p_memsz (the mapped window). */
        if (ph[i].p_offset > imgsize || ph[i].p_filesz > imgsize - ph[i].p_offset ||
            ph[i].p_filesz > ph[i].p_memsz) {
            kprintf("[elf] malformed segment %d (off=%#lx filesz=%#lx "
                    "imgsize=%#lx)\n",
                    i, (u64)ph[i].p_offset, (u64)ph[i].p_filesz, (u64)imgsize);
            return 0;
        }
        u64 va = ALIGN_DOWN(vaddr, PAGE_SIZE);
        u64 end = ALIGN_UP(vaddr + ph[i].p_memsz, PAGE_SIZE);

        for (u64 page = va; page < end; page += PAGE_SIZE) {
            /* PF_X decides whether the page may be executed: everything
             * else (.rodata, .data, .bss) is mapped NX.  A page shared
             * with an already-mapped segment gets its permissions merged
             * instead (pa == 0 tells vmm_map_user there is no new frame). */
            bool w = !!(ph[i].p_flags & PF_W);
            bool x = !!(ph[i].p_flags & PF_X);
            u64 pa = 0;
            if (!vmm_translate_in(pml4, page)) {
                pa = pmm_alloc();
                if (!pa) return 0;
                memset((void *)PHYS_TO_VIRT(pa), 0, PAGE_SIZE);
            }
            if (vmm_map_user(pml4, page, pa, w, true, x) < 0) {
                if (pa) pmm_free(pa);
                return 0; /* no room for the page tables: load failed */
            }
        }

        /* Segment bytes land exactly at the loaded p_vaddr; earlier bytes
         * of its first page stay zero (BSS-style padding).  The copy goes
         * through each frame's high-half alias, never through the user
         * address: text/.rodata pages are mapped read-only, and with
         * CR0.WP on a ring-0 store through the user alias would #PF (there
         * is no fixup table to recover).  The alias is always mapped
         * writable, and this works for any `root` -- not just the address
         * space that happens to be in CR3. */
        {
            u64 done = 0;
            while (done < ph[i].p_filesz) {
                u64 va_cur = vaddr + done;
                u64 pa = vmm_translate_in(pml4, va_cur);
                if (!pa) return 0; /* the loop above mapped every page */
                size_t chunk = PAGE_SIZE - (va_cur & (PAGE_SIZE - 1));
                if (chunk > ph[i].p_filesz - done) chunk = ph[i].p_filesz - done;
                memcpy((void *)PHYS_TO_VIRT(pa), (const u8 *)img + ph[i].p_offset + done,
                       chunk);
                done += chunk;
            }
        }

        if (end > max_end) max_end = end;
    }
    *brk_end = ALIGN_UP(max_end, PAGE_SIZE);
    return entry;
}

u64 elf_load(u64 pml4, const void *img, size_t imgsize, u64 *brk_end)
{
    const Elf64_Ehdr *eh = elf_header(img, imgsize);
    if (!eh) return 0;
    if (eh->e_type != ET_EXEC) { /* ET_DYN needs a bias: elf_load_dyn() */
        kprintf("[elf] e_type %d unsupported (need ET_EXEC)\n", eh->e_type);
        return 0;
    }
    return load_at(pml4, img, imgsize, 0, brk_end);
}

u64 elf_load_dyn(u64 pml4, const void *img, size_t imgsize, u64 bias, u64 *brk_end)
{
    const Elf64_Ehdr *eh = elf_header(img, imgsize);
    if (!eh) return 0;
    if (eh->e_type != ET_DYN) { /* ET_EXEC is linked absolutely: no bias to give */
        kprintf("[elf] e_type %d unsupported (need ET_DYN)\n", eh->e_type);
        return 0;
    }
    /* elf_place() never hands out 0 (its cursor starts at USER_BASE), and a
     * bias of 0 on a link-at-0 image would ask for kernel space.  Refusing
     * here says which of the two went wrong instead of the entry-point
     * check below reporting "outside user space" for a perfectly good file. */
    if (!bias) {
        kprintf("[elf] ET_DYN image loaded with no bias\n");
        return 0;
    }
    return load_at(pml4, img, imgsize, bias, brk_end);
}

int elf_place(const void *img, size_t imgsize, u64 *cursor, u64 *bias, u64 *lo, u64 *hi)
{
    if (!cursor || !bias || !lo || !hi) return -1;
    /* The cursor is the next free USER address, so it has to be one: a
     * caller that has walked it off the end of user space has nothing left
     * to place anything in. */
    if (*cursor < USER_BASE || *cursor >= USER_MAX_VMA) return -1;

    const Elf64_Ehdr *eh = elf_header(img, imgsize);
    if (!eh) return -1;
    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) {
        kprintf("[elf] e_type %d cannot be placed\n", eh->e_type);
        return -1;
    }

    u64 slo = 0, shi = 0;
    if (elf_image_range(img, imgsize, &slo, &shi) < 0) {
        kprintf("[elf] image has no loadable segment\n");
        return -1;
    }

    if (eh->e_type == ET_EXEC) {
        /* Linked at absolute addresses: it goes where it says, and the
         * cursor steps past it so a later image cannot be placed on top. */
        *bias = 0;
        if (shi > *cursor) *cursor = shi;
    } else {
        /* Linked at 0: the whole span is laid down starting at the cursor,
         * so two images placed one after the other cannot overlap -- which
         * is what makes the overlap check below belt and braces rather than
         * the only thing standing between two images and merged bytes. */
        if (shi > USER_MAX_VMA - *cursor) {
            kprintf("[elf] image of %#lx bytes does not fit in user space\n", shi);
            return -1;
        }
        *bias = *cursor;
        *cursor += shi;
    }
    *lo = slo;
    *hi = shi;
    return eh->e_type;
}

int elf_interp_path(const void *img, size_t imgsize, char *out, size_t outsz)
{
    if (!out || !outsz) return -1;
    int phnum = 0;
    const Elf64_Phdr *ph = phdr_table(img, imgsize, &phnum);
    if (!ph) return -1;

    for (int i = 0; i < phnum; i++) {
        if (ph[i].p_type != PT_INTERP) continue;
        /* The path is p_filesz bytes at p_offset and must fit whole: a
         * truncated or oversized one is a corrupt image, not something to
         * silently cut down to a name that names nothing. */
        if (ph[i].p_offset > imgsize || ph[i].p_filesz > imgsize - ph[i].p_offset)
            return -1;
        if (ph[i].p_filesz >= outsz) return -1;
        memcpy(out, (const u8 *)img + ph[i].p_offset, ph[i].p_filesz);
        out[ph[i].p_filesz] = 0; /* in bounds: p_filesz < outsz */
        return 1;
    }
    return 0; /* no PT_INTERP: a static image */
}

int elf_get_phdrs(const void *img, size_t imgsize, struct elf_phdrs *out)
{
    if (!out) return -1;
    out->vaddr = out->entsize = out->count = 0;

    int phnum = 0;
    const Elf64_Phdr *ph = phdr_table(img, imgsize, &phnum);
    if (!ph) return -1;
    const Elf64_Ehdr *eh = img;
    out->entsize = eh->e_phentsize;
    out->count = (u64)phnum;

    /* AT_PHDR has to point at memory the loader actually mapped.  The
     * table normally sits inside the first PT_LOAD, but a segment the
     * loader skips is never mapped and one whose file range does not reach
     * e_phoff holds no table at all -- either way the honest answer is 0.
     * The skip rule is the one load_at() applies, asked in LINK addresses:
     * an ET_EXEC image is mapped at them and an ET_DYN image at link+bias,
     * and elf_place() only ever hands out a bias that keeps every segment
     * it counted inside user space. */
    bool dyn = eh->e_type == ET_DYN;
    for (int i = 0; i < phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (dyn ? ph[i].p_vaddr >= USER_MAX_VMA
                : (ph[i].p_vaddr < USER_BASE || ph[i].p_vaddr >= USER_MAX_VMA))
            continue;
        if (ph[i].p_offset > eh->e_phoff) continue;
        u64 delta = eh->e_phoff - ph[i].p_offset;
        if (delta >= ph[i].p_filesz) continue;
        if (ph[i].p_vaddr + delta < ph[i].p_vaddr) continue; /* wraps */
        out->vaddr = ph[i].p_vaddr + delta;
        return 0;
    }
    return 0; /* well-formed image, just no mapped header table */
}

int elf_image_range(const void *img, size_t imgsize, u64 *lo, u64 *hi)
{
    if (!lo || !hi) return -1;
    int phnum = 0;
    const Elf64_Phdr *ph = phdr_table(img, imgsize, &phnum);
    if (!ph) return -1;
    const Elf64_Ehdr *eh = img;

    /* The skip rules load_at() applies, asked in LINK addresses.  A bias
     * can only move a segment up, so for an ET_DYN image a segment that is
     * already past USER_MAX_VMA here can never be mapped by any bias
     * elf_place() would choose, while one that fits fits just as well at
     * the bias it will actually get -- the answer can over-state what the
     * loader occupies (a refused pair that was really fine) but can never
     * under-state it, which is the direction that would certify a
     * collision as safe.  An ET_EXEC image has bias 0, so the two rules
     * are the same rule. */
    bool dyn = eh->e_type == ET_DYN;
    u64 low = 0, high = 0;
    int seen = 0;
    for (int i = 0; i < phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_vaddr + ph[i].p_memsz <= ph[i].p_vaddr) continue;
        if (dyn ? ph[i].p_vaddr >= USER_MAX_VMA
                : (ph[i].p_vaddr < USER_BASE || ph[i].p_vaddr >= USER_MAX_VMA))
            continue;

        u64 s = ALIGN_DOWN(ph[i].p_vaddr, PAGE_SIZE);
        u64 e = ALIGN_UP(ph[i].p_vaddr + ph[i].p_memsz, PAGE_SIZE);
        /* `seen`, not `!low`: an ET_DYN image linked at 0 has low == 0 as
         * its NORMAL answer, and testing the value would let every later
         * segment overwrite it. */
        if (!seen || s < low) low = s;
        if (!seen || e > high) high = e;
        seen = 1;
    }
    if (!seen) return -1; /* nothing loadable: no span to compare */
    *lo = low;
    *hi = high;
    return 0;
}
