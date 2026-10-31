/* Minimal static ELF64 loader for user programs. */
#include <elf.h>
#include <mm/mm.h>
#include <console.h>

u64 elf_load(u64 pml4, const void *img, size_t imgsize, u64 *brk_end)
{
    if (!img || imgsize < sizeof(Elf64_Ehdr)) return 0; /* NULL is not an image */
    const Elf64_Ehdr *eh = img;
    if (memcmp(eh->e_ident,
               "\x7f"
               "ELF",
               4) ||
        eh->e_ident[4] != 2) {
        kprintf("[elf] not an ELF64 image\n");
        return 0;
    }
    if (eh->e_type != 2) { /* ET_EXEC only */
        kprintf("[elf] e_type %d unsupported (need ET_EXEC)\n", eh->e_type);
        return 0;
    }
    /* The entry point is jumped to in ring 3 the moment this returns, so it
     * has to be a user address before anything is mapped.  (Self-test:
     * ktest/t_elf.c feeds a wild e_entry and expects a rejection.) */
    if (eh->e_entry < USER_BASE || eh->e_entry >= USER_MAX_VMA) {
        kprintf("[elf] e_entry %#lx outside user space\n", (u64)eh->e_entry);
        return 0;
    }

    u64 max_end = 0;
    /* program header table must lie entirely inside the file image */
    if (eh->e_phoff > imgsize || eh->e_phnum > 128 ||
        (u64)eh->e_phnum * sizeof(Elf64_Phdr) > imgsize - eh->e_phoff) {
        kprintf("[elf] malformed program header table\n");
        return 0;
    }
    const Elf64_Phdr *ph = (const Elf64_Phdr *)((const u8 *)img + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_vaddr + ph[i].p_memsz <= ph[i].p_vaddr || ph[i].p_vaddr < USER_BASE ||
            ph[i].p_vaddr >= USER_MAX_VMA)
            continue; /* skip non-user segments (.note etc.) */
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
        u64 va = ALIGN_DOWN(ph[i].p_vaddr, PAGE_SIZE);
        u64 end = ALIGN_UP(ph[i].p_vaddr + ph[i].p_memsz, PAGE_SIZE);

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

        /* Segment bytes land exactly at p_vaddr; earlier bytes of its first
         * page stay zero (BSS-style padding).  The copy goes through each
         * frame's high-half alias, never through p_vaddr: text/.rodata pages
         * are mapped read-only, and with CR0.WP on a ring-0 store through
         * the user alias would #PF (there is no fixup table to recover).
         * The alias is always mapped writable, and this works for any
         * `root` -- not just the address space that happens to be in CR3. */
        {
            u64 done = 0;
            while (done < ph[i].p_filesz) {
                u64 va_cur = ph[i].p_vaddr + done;
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
    return eh->e_entry;
}
