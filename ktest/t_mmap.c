/* T-MMAP: anonymous mmap/munmap/mprotect, the demand pager behind them, the
 * copy-on-write fork that rests on the order-0 reference counts, and the
 * stack guard that keeps an overflow out of the mmap window.
 *
 * Everything runs against the idle task's address space (the master PML4,
 * which is `current` at KTEST_LATE) through the SAME code user space
 * reaches: mmap/munmap/mprotect via syscall_probe() -> the real dispatch
 * table, page-in, COW-break and stack growth via page_fault_try_fix() with
 * a caller-built #PF frame (the same entry isr_common() takes), fork via
 * dup_user_aspace() -- no test-only shortcuts into the internals.
 *
 * Invariant every case restores before its last check: current->vmas
 * back to NULL, vmm_count_user_pages() and pmm_free_bytes() back to the
 * values the case started with.  The COW case additionally snapshots
 * every present user leaf, because dup_user_aspace converts ALL of them
 * (not just the ones this case created) and a leaked conversion would
 * leave ring-0 writes to those pages faulting with no fixup below.  The
 * guard case lends idle a ustack_lo (it has none) and puts it back. */
#include <sys/ktest.h>
#include <console.h>
#include <mm/mm.h>
#include <mm/vma.h>
#include <mm/pagefault.h>
#include <sys/sched.h>
#include <sys/cpu.h>
#include <sys/vfs.h>

/* A synthetic user #PF.  page_fault_try_fix() reads the x86 ERROR CODE --
 * bit 0 = present, bit 1 = write, bit 4 = instruction fetch -- plus the
 * address; cs marks it user-mode the way isr_common() gates it.  Pass the
 * real code: the fixers discriminate on these bits (cow_fault claims only
 * a store, an NX fetch must never pass for one), so a frame that lies
 * about them would test nothing at all.  These four are the codes a user
 * access can actually raise. */
#define PF_NP_R 0x00 /* not present, read  -> demand paging / stack growth  */
#define PF_NP_W 0x02 /* not present, write -> those, and the VMA must say W  */
#define PF_P_W  0x03 /* present, write     -> COW break, or a real violation */
#define PF_P_X  0x11 /* present, fetch     -> NX violation, never fixable    */
static void pf_frame(struct intr_frame *f, u64 err)
{
    memset(f, 0, sizeof(*f));
    f->cs = 3;
    f->err = err;
}

/* ================= dispatch: mmap/munmap ================= */
static void test_mmap_syscall_dispatch(void)
{
    K_ASSERT(current->vmas == NULL); /* nobody mapped yet this boot */
    u64 count_before = vmm_count_user_pages(current->pml4);

    long a = syscall_probe(SYS_mmap, 0, 0x8000, LNXRM_PROT_READ | LNXRM_PROT_WRITE);
    K_ASSERT(a > 0);
    K_EXPECT_EQ((u64)a & (PAGE_SIZE - 1), 0);
    K_EXPECT((u64)a >= MMAP_LO && (u64)a + 0x8000 <= MMAP_TOP);

    long b = syscall_probe(SYS_mmap, 0, 0x3000, LNXRM_PROT_READ);
    K_ASSERT(b > 0);
    /* top-down placement: the second region lands below the first, with
     * the gap between them honoured (no overlap, both inside the window) */
    K_EXPECT((u64)b + 0x3000 <= (u64)a);

    /* negatives: a hint the kernel would ignore is refused, not faked */
    K_EXPECT_EQ(syscall_probe(SYS_mmap, (u64)a, 0x1000, 3), LNXRM_EINVAL);
    K_EXPECT_EQ(syscall_probe(SYS_mmap, 0, 0, 3), LNXRM_EINVAL);
    K_EXPECT_EQ(syscall_probe(SYS_munmap, (u64)a + 1, 0x1000, 0), LNXRM_EINVAL);
    K_EXPECT_EQ(syscall_probe(SYS_munmap, (u64)a, 0, 0), LNXRM_EINVAL);
    K_EXPECT_EQ(syscall_probe(SYS_munmap, USER_BASE, 0x1000, 0), LNXRM_EINVAL);
    K_EXPECT_EQ(syscall_probe(SYS_munmap, MMAP_LO, 0x1000, 0), LNXRM_EINVAL); /* in window, no VMA */

    /* punch a page out of the middle of `a`: the range must split into
     * two survivors, and the untouched regions must stay put */
    K_EXPECT_EQ(syscall_probe(SYS_munmap, (u64)a + 0x1000, 0x1000, 0), 0);
    int n = 0;
    u64 prev_end = 0;
    for (struct vma *v = current->vmas; v; v = v->next) {
        K_EXPECT(v->start >= prev_end && v->end > v->start); /* sorted, sane */
        K_EXPECT(v->start >= MMAP_LO && v->end <= MMAP_TOP);
        prev_end = v->end;
        n++;
    }
    K_EXPECT_EQ(n, 3); /* b + a-head + a-tail */
    struct vma *head = vma_find(current, (u64)a);
    K_ASSERT(head);
    K_EXPECT_EQ(head->start, (u64)a);
    K_EXPECT_EQ(head->end, (u64)a + 0x1000);

    /* tear it all down: the list ends exactly NULL, and a repeat of the
     * same munmap is EINVAL (nothing left to intersect) */
    K_EXPECT_EQ(syscall_probe(SYS_munmap, (u64)a, 0x8000, 0), 0);
    K_EXPECT_EQ(syscall_probe(SYS_munmap, (u64)b, 0x3000, 0), 0);
    K_EXPECT(current->vmas == NULL);
    K_EXPECT_EQ(syscall_probe(SYS_munmap, (u64)a, 0x8000, 0), LNXRM_EINVAL);
    /* no page was ever touched, so the account is untouched too */
    K_EXPECT_EQ(vmm_count_user_pages(current->pml4), count_before);
    K_EXPECT_EQ(current->mem_pages, count_before);
}
KTEST("mmap", KTEST_LATE, test_mmap_syscall_dispatch);

/* ================= demand paging ================= */
static void test_mmap_demand_pager(void)
{
    K_ASSERT(current->vmas == NULL);
    u64 count_before = vmm_count_user_pages(current->pml4);
    u64 fb_before = pmm_free_bytes();

    long base = syscall_probe(SYS_mmap, 0, 0x3000, LNXRM_PROT_READ | LNXRM_PROT_WRITE);
    K_ASSERT(base > 0);
    K_EXPECT_EQ(vmm_translate_in(current->pml4, (u64)base), 0); /* recorded, not mapped */

    struct intr_frame f;

    /* (a) quota ceiling: a plain user sitting AT the cap gets no frame --
     * the same cred_mem_quota() brk and stack growth honour, so mmap
     * cannot become the side door around the 64 MiB limit. */
    struct cred saved_cred = current->cred;
    u64 saved_pages = current->mem_pages;
    current->cred.kind = CRED_USER;
    current->cred.uid = CRED_UID_USER;
    current->mem_pages = CRED_USER_MEM_QUOTA_PAGES;
    pf_frame(&f, PF_NP_R);
    K_EXPECT(!page_fault_try_fix(&f, (u64)base));
    K_EXPECT_EQ(vmm_translate_in(current->pml4, (u64)base), 0);
    current->cred = saved_cred;
    current->mem_pages = saved_pages;

    /* (b) an address no VMA claims is not ours to fix */
    pf_frame(&f, PF_NP_R);
    K_EXPECT(!page_fault_try_fix(&f, USER_BASE + 0x9000)); /* t_syscall's kept-unmapped page */

    /* (c) the real thing: first touch maps a zeroed frame */
    pf_frame(&f, PF_NP_R);
    K_EXPECT(page_fault_try_fix(&f, (u64)base));
    u64 pa0 = vmm_translate_in(current->pml4, (u64)base);
    K_ASSERT(pa0 != 0);
    K_EXPECT_EQ(*(u64 *)PHYS_TO_VIRT(pa0), 0); /* anonymous == fresh zeros */

    pf_frame(&f, PF_NP_R);
    K_EXPECT(page_fault_try_fix(&f, (u64)base + 0x2000));

    /* a second fault on an already-present page must claim NOTHING (the
     * hardware will not raise it, but a stale-TLB retry could: without
     * the guard it would allocate a frame and leak it) */
    u64 free_mid = pmm_free_bytes();
    pf_frame(&f, PF_NP_R);
    K_EXPECT(!page_fault_try_fix(&f, (u64)base));
    K_EXPECT_EQ(vmm_translate_in(current->pml4, (u64)base), pa0);
    K_EXPECT_EQ(pmm_free_bytes(), free_mid);

    /* permissions come from the VMA: writable, NX (no PROT_EXEC) */
    u64 pte = vmm_user_pte(current->pml4, (u64)base);
    K_EXPECT(pte & PG_W);
    K_EXPECT(pte & PG_NX);

    /* teardown: every frame back, list NULL, account and buddy exact */
    K_EXPECT_EQ(syscall_probe(SYS_munmap, (u64)base, 0x3000, 0), 0);
    K_EXPECT(current->vmas == NULL);
    K_EXPECT_EQ(vmm_count_user_pages(current->pml4), count_before);
    K_EXPECT_EQ(current->mem_pages, count_before);
    K_EXPECT_EQ(pmm_free_bytes(), fb_before);
}
KTEST("mmap", KTEST_LATE, test_mmap_demand_pager);

/* ================= COW fork ================= */
struct leaf_snap {
    u64 va, pte;
};

/* Every present 4 KiB leaf under root's USER_SLOT -- the same walk
 * dup_user_aspace() performs -- capped at `max`.  Returns the count, or -1
 * when `max` was not enough: a partial snapshot cannot be restored, so
 * the caller must skip instead of converting pages it cannot put back. */
static int snap_user_leaves(u64 root, struct leaf_snap *out, int max)
{
    u64 *l4 = (u64 *)PHYS_TO_VIRT(root);
    if (!(l4[255] & PG_P)) return 0;
    u64 *l3 = (u64 *)PHYS_TO_VIRT(l4[255] & ~0xfffUL);
    int n = 0;
    for (int i3 = 0; i3 < 512; i3++) {
        if (!(l3[i3] & PG_P)) continue;
        u64 *l2 = (u64 *)PHYS_TO_VIRT(l3[i3] & ~0xfffUL);
        for (int i2 = 0; i2 < 512; i2++) {
            if (!(l2[i2] & PG_P)) continue;
            if (l2[i2] & PG_PS) continue; /* user space has no huge pages */
            u64 *l1 = (u64 *)PHYS_TO_VIRT(l2[i2] & ~0xfffUL);
            for (int i1 = 0; i1 < 512; i1++) {
                if (!(l1[i1] & PG_P)) continue;
                if (n >= max) return -1;
                out[n].va = (255UL << 39) | ((u64)i3 << 30) | ((u64)i2 << 21) |
                            ((u64)i1 << 12);
                out[n].pte = l1[i1];
                n++;
            }
        }
    }
    return n;
}

static void test_mmap_cow_fork(void)
{
    K_ASSERT(current->vmas == NULL);
    u64 count_before = vmm_count_user_pages(current->pml4);
    u64 fb_before = pmm_free_bytes();

    static struct leaf_snap snap[512];
    int nsnap = snap_user_leaves(current->pml4, snap, 512);
    if (nsnap < 0) {
        ktest_skip("more present user leaves than the snapshot holds");
        return;
    }

    struct intr_frame f;

    /* a writable mmap, demand-paged in, payload written through the alias */
    long base = syscall_probe(SYS_mmap, 0, 0x2000, LNXRM_PROT_READ | LNXRM_PROT_WRITE);
    K_ASSERT(base > 0);
    pf_frame(&f, PF_NP_R);
    K_EXPECT(page_fault_try_fix(&f, (u64)base));
    u64 pa = vmm_translate_in(current->pml4, (u64)base);
    K_ASSERT(pa != 0);
    *(u64 *)PHYS_TO_VIRT(pa) = 0xC0FFEEULL;

    /* and a read-only one: after fork its write must STAY a real violation */
    long ro = syscall_probe(SYS_mmap, 0, 0x1000, LNXRM_PROT_READ);
    K_ASSERT(ro > 0);
    pf_frame(&f, PF_NP_R);
    K_EXPECT(page_fault_try_fix(&f, (u64)ro));

    /* the fork-shaped half: share every page, copy none */
    u64 dst = vmm_new_user_aspace();
    K_ASSERT(dst != 0);
    K_EXPECT_EQ(dup_user_aspace(current->pml4, dst), 0);

    /* writable page: both sides read-only + PG_COW, one frame, refcount 2 */
    u64 sp = vmm_user_pte(current->pml4, (u64)base);
    u64 dp = vmm_user_pte(dst, (u64)base);
    K_EXPECT((sp & PG_P) && !(sp & PG_W) && (sp & PG_COW));
    K_EXPECT_EQ(dp, sp);
    K_EXPECT_EQ(pmm_refcount(pa), 2);

    /* read-only page: shared as-is, no COW marker (a write stays SIGSEGV) */
    u64 sr = vmm_user_pte(current->pml4, (u64)ro);
    K_EXPECT((sr & PG_P) && !(sr & PG_W) && !(sr & PG_COW));
    K_EXPECT_EQ(pmm_refcount(sr & PTE_PA_MASK), 2);

    /* write-protect fault on the RO page is not ours to fix -> SIGSEGV */
    pf_frame(&f, PF_P_W);
    K_EXPECT(!page_fault_try_fix(&f, (u64)ro));

    /* ...while the COW page's write fault copies and takes it writable */
    pf_frame(&f, PF_P_W);
    K_EXPECT(page_fault_try_fix(&f, (u64)base));
    u64 npa = vmm_translate_in(current->pml4, (u64)base);
    K_ASSERT(npa != 0 && npa != pa);
    u64 sp2 = vmm_user_pte(current->pml4, (u64)base);
    K_EXPECT((sp2 & PG_W) && !(sp2 & PG_COW));
    K_EXPECT_EQ(pmm_refcount(pa), 1);  /* the child still owns the old frame */
    K_EXPECT_EQ(pmm_refcount(npa), 1); /* we own the new one */
    K_EXPECT_EQ(*(u64 *)PHYS_TO_VIRT(npa), 0xC0FFEEULL);  /* payload copied */
    K_EXPECT_EQ(*(u64 *)PHYS_TO_VIRT(pa), 0xC0FFEEULL);   /* child's untouched */

    /* a repeat fault on the now-private page is claimed as already-writable */
    pf_frame(&f, PF_P_W);
    K_EXPECT(page_fault_try_fix(&f, (u64)base));

    /* ---- teardown, ordered so every reference lands on zero ---- */
    vmm_destroy_user_aspace(dst); /* child drops its refs (old frame 2 -> 1 -> 0) */
    for (int i = 0; i < nsnap; i++) {
        /* `base` is skipped: its snapshot entry names the OLD frame, which
         * the break already handed back -- restoring it would resurrect a
         * dangling PTE that the munmap below would then double-free. */
        if (snap[i].va == (u64)base) continue;
        vmm_user_pte_write(current->pml4, snap[i].va, snap[i].pte);
    }
    K_EXPECT_EQ(syscall_probe(SYS_munmap, (u64)base, 0x2000, 0), 0);
    K_EXPECT_EQ(syscall_probe(SYS_munmap, (u64)ro, 0x1000, 0), 0);
    K_EXPECT(current->vmas == NULL);
    K_EXPECT_EQ(vmm_count_user_pages(current->pml4), count_before);
    K_EXPECT_EQ(current->mem_pages, count_before);
    K_EXPECT_EQ(pmm_free_bytes(), fb_before);
}
KTEST("mmap", KTEST_LATE, test_mmap_cow_fork);

/* ================= order-0 reference counts ================= */
static void test_pmm_refcount(void)
{
    u64 fb = pmm_free_bytes();
    u64 pa = pmm_alloc();
    K_ASSERT(pa != 0);
    K_EXPECT_EQ(pmm_refcount(pa), 1);
    K_EXPECT_EQ(pmm_free_bytes(), fb - PAGE_SIZE);

    pmm_ref(pa); /* second address space moves in */
    K_EXPECT_EQ(pmm_refcount(pa), 2);
    pmm_free(pa); /* first one moves out */
    K_EXPECT_EQ(pmm_refcount(pa), 1);
    K_EXPECT_EQ(pmm_free_bytes(), fb - PAGE_SIZE); /* still held: NOT returned */

    pmm_free(pa); /* last owner */
    K_EXPECT_EQ(pmm_refcount(pa), 0);
    K_EXPECT_EQ(pmm_free_bytes(), fb); /* exactly where we started */
}
KTEST("mmap", KTEST_LATE, test_pmm_refcount);

/* ================= VMA list bookkeeping ================= */
static void test_vma_list(void)
{
    struct task fake;
    memset(&fake, 0, sizeof(fake));

    K_EXPECT_EQ(vma_insert(&fake, 0x1000, 0x3000, LNXRM_PROT_READ), 0);
    K_EXPECT_EQ(vma_insert(&fake, 0x8000, 0x9000, LNXRM_PROT_WRITE), 0);
    K_EXPECT_EQ(vma_insert(&fake, 0x4000, 0x7000, LNXRM_PROT_READ), 0); /* out of order */
    u64 prev = 0;
    for (struct vma *v = fake.vmas; v; v = v->next) { /* kept sorted */
        K_EXPECT(v->start >= prev);
        prev = v->end;
    }

    K_EXPECT(vma_overlaps(&fake, 0x2000, 0x2001));
    K_EXPECT(!vma_overlaps(&fake, 0x3000, 0x4000)); /* the gap */
    K_EXPECT_EQ(vma_find(&fake, 0x5000)->end, 0x7000);
    K_EXPECT(vma_find(&fake, 0x3000) == NULL); /* gap is not in any VMA */

    /* punch a hole: [0x1000,0x3000) becomes two pieces */
    K_EXPECT_EQ(vma_erase(&fake, 0x1800, 0x2800), 0);
    K_EXPECT_EQ(vma_find(&fake, 0x1400)->end, 0x1800);
    K_EXPECT_EQ(vma_find(&fake, 0x2900)->start, 0x2800);
    K_EXPECT(vma_find(&fake, 0x2000) == NULL);
    K_EXPECT_EQ(vma_erase(&fake, 0x3100, 0x3200), LNXRM_EINVAL); /* intersects nothing */

    /* deep copy: the copy walks on its own */
    struct task other;
    memset(&other, 0, sizeof(other));
    K_EXPECT_EQ(vma_dup(&fake, &other), 0);
    K_EXPECT_EQ(vma_find(&other, 0x1400)->end, 0x1800);
    other.vmas->start = 0xDEAD000;
    K_EXPECT_EQ(fake.vmas->start, 0x1000); /* original untouched */
    vma_free_all(&other);
    K_EXPECT(other.vmas == NULL);

    vma_free_all(&fake);
    K_ASSERT(fake.vmas == NULL);
    vma_free_all(&fake); /* idempotent */
}
KTEST("mmap", KTEST_LATE, test_vma_list);

/* ================= mprotect ================= */
static void test_mprotect_syscall(void)
{
    K_ASSERT(current->vmas == NULL);
    u64 count_before = vmm_count_user_pages(current->pml4);
    u64 fb_before = pmm_free_bytes();
    struct intr_frame f;

    /* a 3-page RW region with only its first page touched */
    long base = syscall_probe(SYS_mmap, 0, 0x3000, LNXRM_PROT_READ | LNXRM_PROT_WRITE);
    K_ASSERT(base > 0);
    pf_frame(&f, PF_NP_R);
    K_EXPECT(page_fault_try_fix(&f, (u64)base));
    K_ASSERT(vmm_user_pte(current->pml4, (u64)base) & PG_W);

    /* negatives: unaligned / empty / outside user space / a hole (in the
     * window but claimed by nobody and holding no page) */
    K_EXPECT_EQ(syscall_probe(SYS_mprotect, (u64)base + 1, 0x1000, 1), LNXRM_EINVAL);
    K_EXPECT_EQ(syscall_probe(SYS_mprotect, (u64)base, 0, 1), LNXRM_EINVAL);
    K_EXPECT_EQ(syscall_probe(SYS_mprotect, USER_BASE - PAGE_SIZE, PAGE_SIZE, 1),
                LNXRM_EINVAL);
    K_EXPECT_EQ(syscall_probe(SYS_mprotect, MMAP_LO, PAGE_SIZE, 1), LNXRM_ENOMEM);

    /* revoke write over the whole region: the present page flips now ... */
    K_EXPECT_EQ(syscall_probe(SYS_mprotect, (u64)base, 0x3000, LNXRM_PROT_READ), 0);
    u64 pte = vmm_user_pte(current->pml4, (u64)base);
    K_EXPECT(pte && !(pte & PG_W) && (pte & PG_NX));
    /* ... and the VMA follows, so untouched pages inherit it */
    K_EXPECT_EQ(vma_find(current, (u64)base + 0x2000)->prot, (u64)LNXRM_PROT_READ);

    /* a write-protect fault on the revoked page is a real violation */
    pf_frame(&f, PF_P_W);
    K_EXPECT(!page_fault_try_fix(&f, (u64)base));

    /* grant write+exec back over the whole region */
    K_EXPECT_EQ(syscall_probe(SYS_mprotect, (u64)base, 0x3000,
                              LNXRM_PROT_READ | LNXRM_PROT_WRITE | LNXRM_PROT_EXEC),
                0);
    pte = vmm_user_pte(current->pml4, (u64)base);
    K_EXPECT(pte && (pte & PG_W) && !(pte & PG_NX));
    K_EXPECT_EQ(vma_find(current, (u64)base + 0x2000)->prot, (u64)7);

    /* a middle-only mprotect re-cuts the VMA into three: RWX / R / RWX */
    K_EXPECT_EQ(syscall_probe(SYS_mprotect, (u64)base + 0x1000, 0x1000, LNXRM_PROT_READ),
                0);
    struct vma *v = vma_find(current, (u64)base);
    K_ASSERT(v);
    K_EXPECT_EQ(v->start, (u64)base);
    K_EXPECT_EQ(v->end, (u64)base + 0x1000);
    K_EXPECT_EQ(v->prot, (u64)7);
    v = vma_find(current, (u64)base + 0x1000);
    K_ASSERT(v);
    K_EXPECT_EQ(v->end, (u64)base + 0x2000);
    K_EXPECT_EQ(v->prot, (u64)LNXRM_PROT_READ);
    v = vma_find(current, (u64)base + 0x2000);
    K_ASSERT(v);
    K_EXPECT_EQ(v->end, (u64)base + 0x3000);
    K_EXPECT_EQ(v->prot, (u64)7);

    /* the demand pager honours the new prot, per piece */
    pf_frame(&f, PF_NP_R);
    K_EXPECT(page_fault_try_fix(&f, (u64)base + 0x1000)); /* was untouched */
    pte = vmm_user_pte(current->pml4, (u64)base + 0x1000);
    K_EXPECT(pte && !(pte & PG_W) && (pte & PG_NX)); /* came in read-only */
    pf_frame(&f, PF_NP_R);
    K_EXPECT(page_fault_try_fix(&f, (u64)base + 0x2000));
    pte = vmm_user_pte(current->pml4, (u64)base + 0x2000);
    K_EXPECT(pte && (pte & PG_W) && !(pte & PG_NX)); /* ... and this one RWX */

    /* teardown: list, account and buddy all exact */
    K_EXPECT_EQ(syscall_probe(SYS_munmap, (u64)base, 0x3000, 0), 0);
    K_EXPECT(current->vmas == NULL);
    K_EXPECT_EQ(vmm_count_user_pages(current->pml4), count_before);
    K_EXPECT_EQ(current->mem_pages, count_before);
    K_EXPECT_EQ(pmm_free_bytes(), fb_before);
}
KTEST("mmap", KTEST_LATE, test_mprotect_syscall);

/* mprotect's rule table on a frame another owner also holds: granting
 * write must NEVER hand two address spaces the same writable frame, and
 * revoking it must never be undone by the COW fixer. */
static void test_mprotect_sharing(void)
{
    K_ASSERT(current->vmas == NULL);
    u64 count_before = vmm_count_user_pages(current->pml4);
    u64 fb_before = pmm_free_bytes();
    struct intr_frame f;

    long base = syscall_probe(SYS_mmap, 0, PAGE_SIZE, LNXRM_PROT_READ | LNXRM_PROT_WRITE);
    K_ASSERT(base > 0);
    pf_frame(&f, PF_NP_R);
    K_EXPECT(page_fault_try_fix(&f, (u64)base));
    u64 pa = vmm_translate_in(current->pml4, (u64)base);
    K_ASSERT(pa != 0);
    *(u64 *)PHYS_TO_VIRT(pa) = 0xC0FFEE;
    u64 fb_held = pmm_free_bytes(); /* one frame taken, nothing else moves */

    /* private round trip: revoke then grant back costs no copy at all */
    K_EXPECT_EQ(syscall_probe(SYS_mprotect, (u64)base, PAGE_SIZE, LNXRM_PROT_READ), 0);
    K_EXPECT(!(vmm_user_pte(current->pml4, (u64)base) & PG_W));
    K_EXPECT_EQ(syscall_probe(SYS_mprotect, (u64)base, PAGE_SIZE,
                              LNXRM_PROT_READ | LNXRM_PROT_WRITE), 0);
    K_EXPECT(vmm_user_pte(current->pml4, (u64)base) & PG_W);
    K_EXPECT_EQ(vmm_translate_in(current->pml4, (u64)base), pa); /* SAME frame */
    K_EXPECT_EQ(pmm_refcount(pa), 1);
    K_EXPECT_EQ(pmm_free_bytes(), fb_held); /* mprotect allocated nothing */

    /* a second owner moves in -- what fork's dup_user_aspace does */
    pmm_ref(pa);
    K_EXPECT_EQ(pmm_refcount(pa), 2);

    /* revoke -> grant: the grant MARKS copy-on-write, it does not copy */
    K_EXPECT_EQ(syscall_probe(SYS_mprotect, (u64)base, PAGE_SIZE, LNXRM_PROT_READ), 0);
    K_EXPECT_EQ(syscall_probe(SYS_mprotect, (u64)base, PAGE_SIZE,
                              LNXRM_PROT_READ | LNXRM_PROT_WRITE), 0);
    u64 pte = vmm_user_pte(current->pml4, (u64)base);
    K_EXPECT(pte && (pte & PG_COW) && !(pte & PG_W)); /* deferred to the write */
    K_EXPECT_EQ(vmm_translate_in(current->pml4, (u64)base), pa);
    K_EXPECT_EQ(pmm_refcount(pa), 2);
    K_EXPECT_EQ(pmm_free_bytes(), fb_held); /* still no copy */

    /* an NX FETCH is not that write: P|I carries no write bit, so the
     * fixer must decline.  Claiming it would copy the frame for a store
     * that never happened -- and the retried fetch, still NX, would fault
     * again and again: a wild jump that hangs instead of dying. */
    pf_frame(&f, PF_P_X);
    K_EXPECT(!page_fault_try_fix(&f, (u64)base));
    K_EXPECT_EQ(vmm_user_pte(current->pml4, (u64)base), pte); /* still COW */
    K_EXPECT_EQ(vmm_translate_in(current->pml4, (u64)base), pa); /* no copy */
    K_EXPECT_EQ(pmm_refcount(pa), 2);
    K_EXPECT_EQ(pmm_free_bytes(), fb_held);

    /* revoking again drops the COW promise: the next write fault must be
     * REFUSED, not silently break a copy -- otherwise mprotect(READ) would
     * be undone by the very fixer meant to honour it */
    K_EXPECT_EQ(syscall_probe(SYS_mprotect, (u64)base, PAGE_SIZE, LNXRM_PROT_READ), 0);
    pte = vmm_user_pte(current->pml4, (u64)base);
    K_EXPECT(pte && !(pte & PG_COW) && !(pte & PG_W));
    pf_frame(&f, PF_P_W);
    K_EXPECT(!page_fault_try_fix(&f, (u64)base)); /* SIGSEGV */

    /* grant once more: NOW the write fault does the copy */
    K_EXPECT_EQ(syscall_probe(SYS_mprotect, (u64)base, PAGE_SIZE,
                              LNXRM_PROT_READ | LNXRM_PROT_WRITE), 0);
    pf_frame(&f, PF_P_W);
    K_EXPECT(page_fault_try_fix(&f, (u64)base));
    u64 npa = vmm_translate_in(current->pml4, (u64)base);
    K_ASSERT(npa != 0 && npa != pa);
    pte = vmm_user_pte(current->pml4, (u64)base);
    K_EXPECT((pte & PG_W) && !(pte & PG_COW));
    K_EXPECT_EQ(pmm_refcount(pa), 1);  /* the other owner still holds it */
    K_EXPECT_EQ(pmm_refcount(npa), 1); /* we own the fresh one */
    K_EXPECT_EQ(*(u64 *)PHYS_TO_VIRT(npa), 0xC0FFEE); /* payload followed */

    /* the other owner leaves, then we do: buddy exact again */
    pmm_free(pa);
    K_EXPECT_EQ(pmm_refcount(pa), 0);
    K_EXPECT_EQ(syscall_probe(SYS_munmap, (u64)base, PAGE_SIZE, 0), 0);
    K_EXPECT(current->vmas == NULL);
    K_EXPECT_EQ(vmm_count_user_pages(current->pml4), count_before);
    K_EXPECT_EQ(current->mem_pages, count_before);
    K_EXPECT_EQ(pmm_free_bytes(), fb_before);
}
KTEST("mmap", KTEST_LATE, test_mprotect_sharing);

/* ================= fault kinds =================
 *
 * The fixers read the x86 error code and every refusal must cost nothing.
 * The case that matters: an instruction fetch (P|I -- present, no write
 * bit) into a writable NX page used to look like "the COW break already
 * landed", got CLAIMED, changed nothing, and iretq retried the same
 * instruction into the same #PF: a wild jump hung the process forever
 * instead of handing it SIGSEGV.  NXE is on (arch/setup.asm), so that
 * path is live for every stack, heap and mmap'd data page. */
static void test_fault_kinds(void)
{
    K_ASSERT(current->vmas == NULL);
    u64 count_before = vmm_count_user_pages(current->pml4);
    u64 fb_before = pmm_free_bytes();
    struct intr_frame f;

    /* an RW (hence NX) mapping, demand-paged in by a WRITE: the VMA says
     * PROT_WRITE, so the store is entitled to its frame */
    long base = syscall_probe(SYS_mmap, 0, PAGE_SIZE, LNXRM_PROT_READ | LNXRM_PROT_WRITE);
    K_ASSERT(base > 0);
    pf_frame(&f, PF_NP_W);
    K_EXPECT(page_fault_try_fix(&f, (u64)base));
    u64 pte = vmm_user_pte(current->pml4, (u64)base);
    K_ASSERT(pte);
    K_EXPECT((pte & PG_W) && (pte & PG_NX)); /* writable, NOT executable */

    /* ...and a fetch into it is an NX violation, never a COW break:
     * refused, with the PTE and the buddy untouched */
    u64 fb_held = pmm_free_bytes();
    pf_frame(&f, PF_P_X);
    K_EXPECT(!page_fault_try_fix(&f, (u64)base));
    K_EXPECT_EQ(vmm_user_pte(current->pml4, (u64)base), pte); /* identical */
    K_EXPECT_EQ(pmm_free_bytes(), fb_held);

    /* a store into a PROT_READ mapping is refused BEFORE a frame is taken
     * -- the VMA speaks first, the way Linux's access_error() does */
    long ro = syscall_probe(SYS_mmap, 0, PAGE_SIZE, LNXRM_PROT_READ);
    K_ASSERT(ro > 0);
    pf_frame(&f, PF_NP_W);
    K_EXPECT(!page_fault_try_fix(&f, (u64)ro));
    K_EXPECT_EQ(vmm_translate_in(current->pml4, (u64)ro), 0); /* no page */
    K_EXPECT_EQ(pmm_free_bytes(), fb_held);                   /* no frame */

    /* a READ of the same untouched page is fine: x86 cannot refuse reads */
    pf_frame(&f, PF_NP_R);
    K_EXPECT(page_fault_try_fix(&f, (u64)ro));
    pte = vmm_user_pte(current->pml4, (u64)ro);
    K_ASSERT(pte);
    K_EXPECT(!(pte & PG_W)); /* came in read-only, as the VMA asked */

    /* a store to that now-present read-only page is a protection fault:
     * nothing to break, so it dies too */
    pf_frame(&f, PF_P_W);
    K_EXPECT(!page_fault_try_fix(&f, (u64)ro));

    K_EXPECT_EQ(syscall_probe(SYS_munmap, (u64)base, PAGE_SIZE, 0), 0);
    K_EXPECT_EQ(syscall_probe(SYS_munmap, (u64)ro, PAGE_SIZE, 0), 0);
    K_EXPECT(current->vmas == NULL);
    K_EXPECT_EQ(vmm_count_user_pages(current->pml4), count_before);
    K_EXPECT_EQ(current->mem_pages, count_before);
    K_EXPECT_EQ(pmm_free_bytes(), fb_before);
}
KTEST("mmap", KTEST_LATE, test_fault_kinds);

/* ================= the copy gates fault pages in =================
 *
 * user_ptr_ok / user_ptr_writable run BEFORE the copy, so they have to do
 * what Linux's copy_to_user does implicitly: take the #PF.  Otherwise
 * read() into an untouched mmap buffer answers -EFAULT and anonymous mmap
 * is usable only after the program has poked every page itself.  The
 * fixers keep every rule, so what the gate used to refuse it still
 * refuses -- it just no longer refuses a page that was merely lazy. */
static void test_copy_gates_fault_in(void)
{
    K_ASSERT(current->vmas == NULL);
    u64 count_before = vmm_count_user_pages(current->pml4);
    u64 fb_before = pmm_free_bytes();

    /* write gate: uname's 320-byte answer lands in a page nobody touched */
    long buf = syscall_probe(SYS_mmap, 0, PAGE_SIZE, LNXRM_PROT_READ | LNXRM_PROT_WRITE);
    K_ASSERT(buf > 0);
    K_EXPECT_EQ(vmm_translate_in(current->pml4, (u64)buf), 0); /* lazy */
    K_EXPECT_EQ(syscall_probe(SYS_uname, (u64)buf, 0, 0), 0);
    K_EXPECT(vmm_translate_in(current->pml4, (u64)buf) != 0); /* faulted in */
    K_EXPECT(vmm_user_pte(current->pml4, (u64)buf) & PG_W);   /* ...writable */
    K_EXPECT(*(char *)PHYS_TO_VIRT(vmm_translate_in(current->pml4, (u64)buf)) != 0);

    /* read gate: the same page is all zeros, so open() reads an empty path
     * and answers anything but EFAULT -- EFAULT is what the gate said
     * before it learned to fault a page in */
    long buf2 = syscall_probe(SYS_mmap, 0, PAGE_SIZE, LNXRM_PROT_READ | LNXRM_PROT_WRITE);
    K_ASSERT(buf2 > 0);
    K_EXPECT_EQ(vmm_translate_in(current->pml4, (u64)buf2), 0);
    long rc = syscall_probe(SYS_open, (u64)buf2, 0, 0);
    K_EXPECT(rc != LNXRM_EFAULT);
    K_EXPECT(vmm_translate_in(current->pml4, (u64)buf2) != 0); /* faulted in */
    if (rc >= 0) syscall_probe(SYS_close, (u64)rc, 0, 0);

    /* the rules are still enforced: a write into a PROT_READ region is not
     * materialized, it stays -EFAULT with the page still lazy */
    long ro = syscall_probe(SYS_mmap, 0, PAGE_SIZE, LNXRM_PROT_READ);
    K_ASSERT(ro > 0);
    K_EXPECT_EQ(vmm_translate_in(current->pml4, (u64)ro), 0);
    K_EXPECT_EQ(syscall_probe(SYS_uname, (u64)ro, 0, 0), LNXRM_EFAULT);
    K_EXPECT_EQ(vmm_translate_in(current->pml4, (u64)ro), 0); /* still lazy */

    K_EXPECT_EQ(syscall_probe(SYS_munmap, (u64)buf, PAGE_SIZE, 0), 0);
    K_EXPECT_EQ(syscall_probe(SYS_munmap, (u64)buf2, PAGE_SIZE, 0), 0);
    K_EXPECT_EQ(syscall_probe(SYS_munmap, (u64)ro, PAGE_SIZE, 0), 0);
    K_EXPECT(current->vmas == NULL);
    K_EXPECT_EQ(vmm_count_user_pages(current->pml4), count_before);
    K_EXPECT_EQ(current->mem_pages, count_before);
    K_EXPECT_EQ(pmm_free_bytes(), fb_before);
}
KTEST("mmap", KTEST_LATE, test_copy_gates_fault_in);

/* ================= stack guard ================= */
static void test_stack_guard(void)
{
    K_ASSERT(current->vmas == NULL);
    u64 count_before = vmm_count_user_pages(current->pml4);
    u64 fb_before = pmm_free_bytes();
    u64 floor = MMAP_TOP + STACK_GUARD; /* deepest legal stack page */
    u64 saved_lo = current->ustack_lo;
    struct intr_frame f;

    /* idle has no user stack (ustack_lo == 0 -> growth declines).  Lend it
     * one at the usual exec-time watermark so the fixer has something to
     * grow, and put it back before the last check -- same save/restore the
     * quota case does with cred. */
    current->ustack_lo = USER_STACK_TOP - 0x4000;

    /* the guard: unmapped, unmmapable, unprotectable */
    K_EXPECT_EQ(vmm_translate_in(current->pml4, MMAP_TOP), 0);
    K_EXPECT_EQ(vmm_translate_in(current->pml4, floor - PAGE_SIZE), 0);

    long top = syscall_probe(SYS_mmap, 0, PAGE_SIZE, LNXRM_PROT_READ | LNXRM_PROT_WRITE);
    K_ASSERT(top > 0);
    K_EXPECT_EQ((u64)top + PAGE_SIZE, MMAP_TOP); /* window stops at the guard */
    K_EXPECT_EQ(vmm_translate_in(current->pml4, MMAP_TOP), 0);
    K_EXPECT_EQ(syscall_probe(SYS_mprotect, MMAP_TOP, STACK_GUARD, LNXRM_PROT_NONE),
                LNXRM_ENOMEM); /* no VMA, no page: nothing to protect */
    K_EXPECT_EQ(syscall_probe(SYS_munmap, (u64)top, PAGE_SIZE, 0), 0);

    /* a fault INSIDE the guard is nobody's to fix, even when it looks
     * exactly like a stack access (RSP parked at the fault) */
    pf_frame(&f, PF_NP_R);
    f.ussp = floor - PAGE_SIZE;
    K_EXPECT(!page_fault_try_fix(&f, MMAP_TOP));
    K_EXPECT(!page_fault_try_fix(&f, floor - PAGE_SIZE));
    K_EXPECT_EQ(vmm_translate_in(current->pml4, MMAP_TOP), 0);

    /* normal lazy growth is untouched: one page below the watermark */
    pf_frame(&f, PF_NP_R);
    f.ussp = current->ustack_lo - PAGE_SIZE;
    K_EXPECT(page_fault_try_fix(&f, current->ustack_lo - PAGE_SIZE));
    K_EXPECT_EQ(current->ustack_lo, USER_STACK_TOP - 0x5000);
    K_EXPECT(vmm_translate_in(current->pml4, USER_STACK_TOP - 0x5000) != 0);
    u64 fb_grown = pmm_free_bytes();

    /* a fault far below the running RSP is NOT a stack access: growth
     * refuses it even though it sits inside the growth window -- and takes
     * no frame doing so */
    u64 target = current->ustack_lo - PAGE_SIZE;
    pf_frame(&f, PF_NP_R);
    f.ussp = target + 0x20000; /* RSP 128 KiB above the fault */
    K_EXPECT(!page_fault_try_fix(&f, target));
    K_EXPECT_EQ(current->ustack_lo, USER_STACK_TOP - 0x5000); /* unchanged */
    K_EXPECT_EQ(vmm_translate_in(current->pml4, target), 0);  /* no page taken */
    K_EXPECT_EQ(pmm_free_bytes(), fb_grown);                  /* buddy untouched */

    /* the same fault with a plausible RSP is granted */
    pf_frame(&f, PF_NP_R);
    f.ussp = target;
    K_EXPECT(page_fault_try_fix(&f, target));
    K_EXPECT_EQ(current->ustack_lo, target);

    /* hand the borrowed stack pages back, then restore everything */
    for (u64 va = current->ustack_lo; va < USER_STACK_TOP - 0x4000; va += PAGE_SIZE) {
        u64 pa = vmm_unmap_user(current->pml4, va);
        K_EXPECT(pa != 0);
        if (pa) {
            pmm_free(pa);
            vmm_reclaim_empty_tables(current->pml4, va);
        }
    }
    current->ustack_lo = saved_lo;
    current->mem_pages = vmm_count_user_pages(current->pml4);
    K_EXPECT(current->vmas == NULL);
    K_EXPECT_EQ(vmm_count_user_pages(current->pml4), count_before);
    K_EXPECT_EQ(pmm_free_bytes(), fb_before);
}
KTEST("mmap", KTEST_LATE, test_stack_guard);
