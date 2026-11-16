/* S0 (GAP 5.x): shm_open(51) / shm_unlink(52) -- named shared-memory
 * objects.
 *
 * NOTHING HERE MAY PARK (the LAPIC timer does not run at KTEST_LATE), so
 * every path walked below is one that cannot block: a name lookup, an fd
 * slot, a page that faults in on the spot.
 *
 * The contract these cases make is the one t_mmap.c makes: they start on
 * `current->vmas == NULL` and restore it -- plus vmm_count_user_pages(),
 * pmm_free_bytes() and the filepage cache -- before their last check, so
 * the suite's order cannot matter.
 *
 * The case that matters is test_shm_shared_frame: it opens the SAME name
 * TWICE, through two independent shm_open calls, and asserts the two
 * descriptors are the same struct file and that both ranges resolve to one
 * physical frame.  That is the whole design of S0 in one check -- if the
 * second open had built its own file, mm/filepage.c (which keys its cache
 * on the file POINTER) would have filled a second frame and every
 * cross-process handshake built on this would have silently not worked.
 *
 * The name is written into a user page the way ring 3 would leave it there,
 * because the dispatcher reads it with copy_user_str() and refuses anything
 * ring 3 could not have named -- a string literal in this file's .rodata
 * included.  That page is mapped here, faulted in, and UNMAPPED before each
 * case's last check so it leaves no VMA behind. */
#include <sys/ktest.h>
#include <sys/shm.h>
#include <sys/vfs.h>
#include <sys/sched.h>
#include <console.h> /* memset / memcpy / memcmp / strlen */
#include <mm/mm.h>
#include <mm/vma.h>
#include <mm/filepage.h>
#include <mm/pagefault.h>

/* A synthetic user #PF, the same shape t_mmap.c builds: bit0 clear (not
 * present) so the fixers run, cs = 3 so it reads as a ring-3 access.  The
 * shared mapping below is answered by demand_page() either way -- what the
 * error code decides is WHICH fixer claims it, and a lying code would let a
 * check test nothing. */
#define PF_NP_R 0x00
static void pf_frame(struct intr_frame *f, u64 err)
{
    memset(f, 0, sizeof(*f));
    f->cs = 3;
    f->err = err;
}

/* ---- the page the object's name is written into ---- */

static u64 name_va;

/* Map one anonymous page and fault it in.  On ANY failure it unmaps what it
 * made and answers false, so a caller that returns on `false` leaves the
 * address space exactly as it found it. */
static bool shm_name_page(void)
{
    if (name_va) return true;

    long p = syscall_probe6(SYS_mmap, 0, PAGE_SIZE, LNXRM_PROT_READ | LNXRM_PROT_WRITE,
                            0, (u64)-1, 0);
    name_va = p > 0 ? (u64)p : 0;
    if (name_va) {
        struct intr_frame f;
        pf_frame(&f, PF_NP_R);
        if (!page_fault_try_fix(&f, name_va)) name_va = 0;
    }
    if (!name_va) {
        if (p > 0) syscall_probe(SYS_munmap, (u64)p, PAGE_SIZE, 0);
        K_EXPECT(name_va != 0);
        return false;
    }
    return true;
}

/* Hand the page back.  Every case calls this before its last check. */
static void shm_name_done(void)
{
    if (!name_va) return;
    K_EXPECT_EQ(syscall_probe(SYS_munmap, name_va, PAGE_SIZE, 0), 0);
    name_va = 0;
}

/* Write a C string into that page through the frame's high-half alias --
 * a kernel self-test has no user-mode store of its own. */
static void set_name(const char *s)
{
    u64 pa = vmm_translate_in(current->pml4, name_va);
    K_EXPECT(pa != 0);
    if (pa) memcpy((void *)PHYS_TO_VIRT(pa), s, strlen(s) + 1);
}

/* Fault a shared range in and answer the frame it landed on (0 on failure). */
static u64 map_in(long va)
{
    struct intr_frame f;
    pf_frame(&f, PF_NP_R);
    if (!page_fault_try_fix(&f, (u64)va)) return 0;
    return vmm_translate_in(current->pml4, (u64)va);
}

/* ================= the names themselves =================
 *
 * Every refusal must cost nothing: no entry, no descriptor, no VMA.  The
 * live-object counter is what proves that, so it is checked after each one
 * rather than only at the end. */
static void test_shm_names(void)
{
    K_EXPECT(current->vmas == NULL);
    u32 live0 = shm_live_objects();

    if (!shm_name_page()) return;

    /* ---- a name nobody has ---- */
    set_name("/shm_none");
    K_EXPECT_EQ(syscall_probe(SYS_shm_open, (u64)name_va, 0, 0), LNXRM_ENOENT);
    K_EXPECT_EQ(shm_live_objects(), live0);

    /* ---- names that are not names ---- */
    set_name("");
    K_EXPECT_EQ(syscall_probe(SYS_shm_open, (u64)name_va, LNXRM_O_CREAT, 4096), LNXRM_EINVAL);
    set_name("////");
    K_EXPECT_EQ(syscall_probe(SYS_shm_open, (u64)name_va, LNXRM_O_CREAT, 4096), LNXRM_EINVAL);
    set_name("a/b"); /* an interior slash: this is not a path */
    K_EXPECT_EQ(syscall_probe(SYS_shm_open, (u64)name_va, LNXRM_O_CREAT, 4096), LNXRM_EINVAL);
    K_EXPECT_EQ(shm_live_objects(), live0);

    /* ---- the length bound, at the byte where it turns ---- */
    set_name("01234567890123456789012345678901"); /* 32 chars + NUL */
    K_EXPECT_EQ(syscall_probe(SYS_shm_open, (u64)name_va, LNXRM_O_CREAT | LNXRM_O_EXCL, 4096),
                LNXRM_ENAMETOOLONG);
    K_EXPECT_EQ(shm_live_objects(), live0);
    set_name("0123456789012345678901234567890"); /* 31 chars + NUL: fits */
    long fit = syscall_probe(SYS_shm_open, (u64)name_va, LNXRM_O_CREAT | LNXRM_O_EXCL, 4096);
    K_EXPECT(fit >= 0);
    if (fit >= 0) {
        K_EXPECT_EQ(shm_live_objects(), live0 + 1);
        K_EXPECT_EQ(syscall_probe(SYS_close, (u64)fit, 0, 0), 0);
        K_EXPECT_EQ(syscall_probe(SYS_shm_unlink, (u64)name_va, 0, 0), 0);
        K_EXPECT_EQ(shm_live_objects(), live0); /* last reference went with unlink */
    }

    /* ---- a flag this call has no meaning for ---- */
    set_name("/shm_nm");
    K_EXPECT_EQ(syscall_probe(SYS_shm_open, (u64)name_va,
                              LNXRM_O_CREAT | LNXRM_O_EXCL | 01000 /* O_TRUNC */, 4096),
                LNXRM_EINVAL);
    K_EXPECT_EQ(shm_live_objects(), live0);

    /* ---- size: 0 is not an object, and an object nobody could back is
     * refused BEFORE anything is allocated ---- */
    K_EXPECT_EQ(syscall_probe(SYS_shm_open, (u64)name_va, LNXRM_O_CREAT, 0), LNXRM_EINVAL);
    K_EXPECT_EQ(syscall_probe(SYS_shm_open, (u64)name_va, LNXRM_O_CREAT,
                              LNXRM_SHM_MAX_SIZE + 1),
                LNXRM_ENOMEM);
    K_EXPECT_EQ(shm_live_objects(), live0);

    /* ---- create, then the exclusive door ---- */
    long a = syscall_probe(SYS_shm_open, (u64)name_va, LNXRM_O_CREAT | LNXRM_O_EXCL, 4096);
    K_EXPECT(a >= 0);
    if (a >= 0) {
        K_EXPECT_EQ(shm_live_objects(), live0 + 1);
        K_EXPECT_EQ(syscall_probe(SYS_shm_open, (u64)name_va,
                                  LNXRM_O_CREAT | LNXRM_O_EXCL, 4096),
                    LNXRM_EEXIST);
        K_EXPECT_EQ(shm_live_objects(), live0 + 1); /* EEXIST allocated nothing */

        /* ---- a second, independent open: the SAME file ---- */
        long b = syscall_probe(SYS_shm_open, (u64)name_va, 0, 0);
        K_EXPECT(b >= 0);
        if (b >= 0) {
            K_EXPECT(b != a);
            K_EXPECT(current->fds[a] == current->fds[b]); /* one object, one file */
            K_EXPECT_EQ(shm_live_objects(), live0 + 1);
            K_EXPECT_EQ(syscall_probe(SYS_close, (u64)b, 0, 0), 0);
        }
        K_EXPECT_EQ(syscall_probe(SYS_close, (u64)a, 0, 0), 0);

        /* Closed by everybody, yet still there: the entry holds a reference
         * no descriptor owns, which is what lets the two ends of a
         * rendezvous miss each other in time. */
        K_EXPECT_EQ(shm_live_objects(), live0 + 1);

        /* ---- unlink drops the NAME and, with nothing else open, the
         * object with it ---- */
        K_EXPECT_EQ(syscall_probe(SYS_shm_unlink, (u64)name_va, 0, 0), 0);
        K_EXPECT_EQ(shm_live_objects(), live0);
        K_EXPECT_EQ(syscall_probe(SYS_shm_unlink, (u64)name_va, 0, 0), LNXRM_ENOENT);
        K_EXPECT_EQ(syscall_probe(SYS_shm_open, (u64)name_va, 0, 0), LNXRM_ENOENT);
        K_EXPECT_EQ(shm_live_objects(), live0);
    }

    shm_name_done();
    K_EXPECT(current->vmas == NULL);
}
KTEST("shm", KTEST_LATE, test_shm_names);

/* ================= one frame for two descriptors ================= */
static void test_shm_shared_frame(void)
{
    K_EXPECT(current->vmas == NULL);
    u64 fp0 = filepage_resident();
    u64 pages0 = vmm_count_user_pages(current->pml4);
    u64 free0 = pmm_free_bytes();
    u32 live0 = shm_live_objects();

    if (!shm_name_page()) return;
    set_name("/shm_sf");

    long fd1 = syscall_probe(SYS_shm_open, (u64)name_va, LNXRM_O_CREAT | LNXRM_O_EXCL,
                             PAGE_SIZE);
    K_EXPECT(fd1 >= 0);
    long fd2 = fd1 >= 0 ? syscall_probe(SYS_shm_open, (u64)name_va, 0, 0) : -1;
    K_EXPECT(fd2 >= 0);

    long m1 = -1, m2 = -1;
    if (fd1 >= 0 && fd2 >= 0) {
        /* The design, in one line: two INDEPENDENT opens of one name share
         * one struct file, so mm/filepage.c cannot tell them apart. */
        K_EXPECT(current->fds[fd1] == current->fds[fd2]);
        K_EXPECT_EQ(shm_live_objects(), live0 + 1);

        m1 = syscall_probe6(SYS_mmap, 0, PAGE_SIZE, LNXRM_PROT_READ | LNXRM_PROT_WRITE,
                            LNXRM_MAP_SHARED, (u64)fd1, 0);
        m2 = syscall_probe6(SYS_mmap, 0, PAGE_SIZE, LNXRM_PROT_READ | LNXRM_PROT_WRITE,
                            LNXRM_MAP_SHARED, (u64)fd2, 0);
        K_EXPECT(m1 > 0 && m2 > 0 && m1 != m2);
    }

    if (m1 > 0 && m2 > 0) {
        u64 pa1 = map_in(m1);
        u64 pa2 = map_in(m2);
        K_EXPECT(pa1 != 0);
        /* THE assertion: two ranges, one physical frame. */
        K_EXPECT_EQ(pa2, pa1);
        K_EXPECT_EQ(filepage_resident(), fp0 + 1); /* one cache entry, not two */

        /* Writable at once and never copy-on-write: MAP_SHARED shares the
         * frame's ownership, it does not fork it. */
        K_EXPECT(vmm_user_pte(current->pml4, (u64)m1) & PG_W);
        K_EXPECT(!(vmm_user_pte(current->pml4, (u64)m1) & PG_COW));

        /* A store into the frame is every mapping of it, immediately. */
        memcpy((void *)PHYS_TO_VIRT(pa1), "SHM!", 5);
        K_EXPECT_EQ(memcmp((void *)PHYS_TO_VIRT(pa2), "SHM!", 5), 0);
        K_EXPECT_EQ(memcmp((void *)PHYS_TO_VIRT(vmm_translate_in(current->pml4, (u64)m2)),
                           "SHM!", 5),
                    0);
    }

    /* ---- teardown on every path, in one place ---- */
    if (m1 > 0) K_EXPECT_EQ(syscall_probe(SYS_munmap, (u64)m1, PAGE_SIZE, 0), 0);
    if (m2 > 0) K_EXPECT_EQ(syscall_probe(SYS_munmap, (u64)m2, PAGE_SIZE, 0), 0);
    if (fd1 >= 0) K_EXPECT_EQ(syscall_probe(SYS_close, (u64)fd1, 0, 0), 0);
    if (fd2 >= 0) K_EXPECT_EQ(syscall_probe(SYS_close, (u64)fd2, 0, 0), 0);
    if (fd1 >= 0)
        K_EXPECT_EQ(syscall_probe(SYS_shm_unlink, (u64)name_va, 0, 0), 0);

    /* Nothing held anything any more, so close_node ran and the frames went
     * with the file -- the cache outlives the mappings but not the file. */
    K_EXPECT_EQ(filepage_resident(), fp0);
    K_EXPECT_EQ(shm_live_objects(), live0);

    shm_name_done();
    K_EXPECT(current->vmas == NULL);
    K_EXPECT_EQ(vmm_count_user_pages(current->pml4), pages0);
    K_EXPECT_EQ(current->mem_pages, pages0);
    K_EXPECT_EQ(pmm_free_bytes(), free0);
}
KTEST("shm", KTEST_LATE, test_shm_shared_frame);

/* ================= lifetime: name, bytes, and the door between them ====== */
static void test_shm_lifecycle(void)
{
    K_EXPECT(current->vmas == NULL);
    u64 fp0 = filepage_resident();
    u64 pages0 = vmm_count_user_pages(current->pml4);
    u64 free0 = pmm_free_bytes();
    u32 live0 = shm_live_objects();

    if (!shm_name_page()) return;
    set_name("/shm_lc");

    long fd1 = syscall_probe(SYS_shm_open, (u64)name_va, LNXRM_O_CREAT | LNXRM_O_EXCL,
                             PAGE_SIZE);
    K_EXPECT(fd1 >= 0);
    long m1 = fd1 >= 0
                  ? syscall_probe6(SYS_mmap, 0, PAGE_SIZE, LNXRM_PROT_READ | LNXRM_PROT_WRITE,
                                   LNXRM_MAP_SHARED, (u64)fd1, 0)
                  : -1;
    K_EXPECT(m1 > 0);
    if (m1 > 0) {
        u64 pa = map_in(m1);
        K_EXPECT(pa != 0);
        if (pa) memcpy((void *)PHYS_TO_VIRT(pa), "LIVE", 5);
    }

    /* ---- drop the mapping AND the descriptor: the object must survive ---- */
    if (m1 > 0) K_EXPECT_EQ(syscall_probe(SYS_munmap, (u64)m1, PAGE_SIZE, 0), 0);
    if (fd1 >= 0) K_EXPECT_EQ(syscall_probe(SYS_close, (u64)fd1, 0, 0), 0);
    m1 = -1;
    fd1 = -1;
    K_EXPECT_EQ(shm_live_objects(), live0 + 1);  /* the entry holds it */
    K_EXPECT_EQ(filepage_resident(), fp0 + 1);   /* ...and so the frames stay */

    /* ---- re-attach by name alone: the half of a rendezvous that arrives
     * later finds the same object, with the bytes still in it ---- */
    long fd2 = syscall_probe(SYS_shm_open, (u64)name_va, 0, 0);
    K_EXPECT(fd2 >= 0);
    long m2 = fd2 >= 0 ? syscall_probe6(SYS_mmap, 0, PAGE_SIZE,
                                        LNXRM_PROT_READ | LNXRM_PROT_WRITE,
                                        LNXRM_MAP_SHARED, (u64)fd2, 0)
                       : -1;
    K_EXPECT(m2 > 0);
    if (m2 > 0) {
        u64 pa = map_in(m2);
        K_EXPECT(pa != 0);
        if (pa) K_EXPECT_EQ(memcmp((void *)PHYS_TO_VIRT(pa), "LIVE", 5), 0);
        K_EXPECT_EQ(filepage_resident(), fp0 + 1); /* the SAME frame, still one */

        /* ---- write-back reports the page clean and stores nothing: the
         * frames ARE the object, so a flush has nowhere to put bytes ---- */
        u64 pte = vmm_user_pte(current->pml4, (u64)m2);
        vmm_user_pte_write(current->pml4, (u64)m2, pte | PG_D);
        K_EXPECT_EQ(vma_writeback(current, current->pml4, 0, MMAP_TOP), 1);
        K_EXPECT(!(vmm_user_pte(current->pml4, (u64)m2) & PG_D));
        K_EXPECT_EQ(memcmp((void *)PHYS_TO_VIRT(vmm_translate_in(current->pml4, (u64)m2)),
                           "LIVE", 5),
                    0); /* still there -- nothing overwrote it */
        K_EXPECT_EQ(vfs_file_size_now(current->fds[fd2]), PAGE_SIZE); /* fixed */
    }

    /* ---- unlink while a descriptor is open: the NAME goes, the object
     * does not, and nobody can reach it by name again ---- */
    K_EXPECT_EQ(syscall_probe(SYS_shm_unlink, (u64)name_va, 0, 0), 0);
    K_EXPECT_EQ(shm_live_objects(), live0 + 1); /* still held open */
    K_EXPECT_EQ(syscall_probe(SYS_shm_unlink, (u64)name_va, 0, 0), LNXRM_ENOENT);
    K_EXPECT_EQ(syscall_probe(SYS_shm_open, (u64)name_va, 0, 0), LNXRM_ENOENT);

    /* ...and the descriptor that outlived its name still maps the object,
     * onto the very frame it mapped before. */
    long m3 = fd2 >= 0 ? syscall_probe6(SYS_mmap, 0, PAGE_SIZE,
                                        LNXRM_PROT_READ | LNXRM_PROT_WRITE,
                                        LNXRM_MAP_SHARED, (u64)fd2, 0)
                       : -1;
    K_EXPECT(m3 > 0);
    if (m3 > 0 && m2 > 0) {
        u64 pa3 = map_in(m3);
        K_EXPECT(pa3 != 0);
        K_EXPECT_EQ(pa3, vmm_translate_in(current->pml4, (u64)m2));
        K_EXPECT_EQ(filepage_resident(), fp0 + 1);
    }

    /* ---- teardown: the last descriptor is the last reference, and with
     * the name already gone close_node frees the entry ---- */
    if (m3 > 0) K_EXPECT_EQ(syscall_probe(SYS_munmap, (u64)m3, PAGE_SIZE, 0), 0);
    if (m2 > 0) K_EXPECT_EQ(syscall_probe(SYS_munmap, (u64)m2, PAGE_SIZE, 0), 0);
    if (fd2 >= 0) K_EXPECT_EQ(syscall_probe(SYS_close, (u64)fd2, 0, 0), 0);
    K_EXPECT_EQ(shm_live_objects(), live0);
    K_EXPECT_EQ(filepage_resident(), fp0);

    shm_name_done();
    K_EXPECT(current->vmas == NULL);
    K_EXPECT_EQ(vmm_count_user_pages(current->pml4), pages0);
    K_EXPECT_EQ(current->mem_pages, pages0);
    K_EXPECT_EQ(pmm_free_bytes(), free0);
}
KTEST("shm", KTEST_LATE, test_shm_lifecycle);
