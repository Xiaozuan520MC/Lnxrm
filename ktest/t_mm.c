/* MM self-tests: the buddy allocator, the kernel heap and the page tables.
 *
 * Every case is required to leave the subsystem exactly as it found it, so
 * the allocator tests can (and do) assert pmm_free_bytes() returns to the
 * value it had before the case started -- a leak in any path shows up as a
 * failing count instead of as slow death after a few days of uptime. */
#include <sys/ktest.h>
#include <console.h>
#include <mm/mm.h>

void vmm_heap_region(u64 *phys, u64 *size); /* mm/vmm.c, not in mm.h */

/* Raw leaf entry (4 KiB PTE or 2 MiB PDE) for `va` under page-table root
 * `root`.  Used where the flags matter and vmm_translate_in() only reports
 * the frame -- e.g. the PG_NX policy. */
static u64 raw_leaf(u64 root, u64 va)
{
    u64 i4 = (va >> 39) & 511, i3 = (va >> 30) & 511;
    u64 i2 = (va >> 21) & 511, i1 = (va >> 12) & 511;
    u64 *l4 = (u64 *)PHYS_TO_VIRT(root);
    if (!(l4[i4] & PG_P)) return 0;
    u64 *l3 = (u64 *)PHYS_TO_VIRT(l4[i4] & PTE_PA_MASK);
    if (!(l3[i3] & PG_P)) return 0;
    u64 *l2 = (u64 *)PHYS_TO_VIRT(l3[i3] & PTE_PA_MASK);
    if (!(l2[i2] & PG_P)) return 0;
    if (l2[i2] & PG_PS) return l2[i2];
    u64 *l1 = (u64 *)PHYS_TO_VIRT(l2[i2] & PTE_PA_MASK);
    if (!(l1[i1] & PG_P)) return 0;
    return l1[i1];
}

/* ---- physical frames ---- */

static void test_pmm_buddy_accounting(void)
{
    u64 before = pmm_free_bytes();
    K_ASSERT(before >= 0x1000);

    u64 a = pmm_alloc_order(0);
    K_ASSERT(a != 0);
    K_EXPECT_EQ(a & (PAGE_SIZE - 1), 0);                 /* frames are page aligned */
    K_EXPECT_EQ(pmm_free_bytes(), before - PAGE_SIZE);   /* exactly one page out */
    pmm_free_order(a, 0);
    K_EXPECT_EQ(pmm_free_bytes(), before);               /* and back, no leak */

    /* a high-order block is aligned to its own size */
    u64 b = pmm_alloc_order(3);
    K_ASSERT(b != 0);
    K_EXPECT_EQ(b & ((1UL << 15) - 1), 0);
    K_EXPECT_EQ(pmm_free_bytes(), before - (1UL << 15));
    pmm_free_order(b, 3);
    K_EXPECT_EQ(pmm_free_bytes(), before);

    /* simultaneously live frames must never alias each other */
    u64 frames[16];
    for (int i = 0; i < 16; i++) {
        frames[i] = pmm_alloc_order(0);
        K_ASSERT(frames[i] != 0);
    }
    for (int i = 0; i < 16; i++) {
        K_EXPECT_EQ(frames[i] & (PAGE_SIZE - 1), 0);
        for (int j = i + 1; j < 16; j++) K_EXPECT(frames[i] != frames[j]);
    }
    for (int i = 0; i < 16; i++) pmm_free_order(frames[i], 0);
    K_EXPECT_EQ(pmm_free_bytes(), before);

    /* orders at/above MAX_ORDER must fail cleanly, not corrupt the lists */
    K_EXPECT_EQ(pmm_alloc_order(11), 0);
    K_EXPECT_EQ(pmm_alloc_order(64), 0);
    K_EXPECT_EQ(pmm_free_bytes(), before);
}

static void test_pmm_high_alias(void)
{
    /* vmm_init() promises every managed frame is reachable through the
     * high-half alias -- kmalloc's large path and vmm_map_user both rely
     * on PHYS_TO_VIRT() for the memory they hand out. */
    u64 pa = pmm_alloc();
    K_ASSERT(pa != 0);
    u8 *v = (u8 *)PHYS_TO_VIRT(pa);
    memset(v, 0x3C, PAGE_SIZE);
    K_EXPECT_EQ(v[0], 0x3C);
    K_EXPECT_EQ(v[PAGE_SIZE - 1], 0x3C);
    K_EXPECT_EQ(*(volatile u8 *)PHYS_TO_VIRT(pa), 0x3C);
    pmm_free(pa);
}

/* ---- kernel heap ---- */

static void test_kheap_alloc_free(void)
{
    /* zero-size request still yields a block; NULL free is a no-op */
    void *z = kmalloc(0);
    K_ASSERT(z != NULL);
    kfree(z);
    kfree(NULL);

    void *p = kmalloc(64);
    K_ASSERT(p != NULL);
    K_EXPECT_EQ((uptr)p & 7, 0);
    u8 ref[64];
    memset(ref, 0xAB, sizeof(ref));
    memset(p, 0xAB, 64);
    K_EXPECT(memcmp(p, ref, 64) == 0);
    kfree(p);

    /* every power-of-two size: 1 .. 4096 crosses into the buddy path */
    for (size_t n = 1; n <= 4096; n *= 2) {
        void *q = kmalloc(n);
        K_ASSERT(q != NULL);
        memset(q, (int)(n & 0x7f), n);
        K_EXPECT_EQ(((u8 *)q)[0], (u8)(n & 0x7f));
        K_EXPECT_EQ(((u8 *)q)[n - 1], (u8)(n & 0x7f));
        kfree(q);
    }

    /* the large path bypasses the bins entirely (buddy order >= 1) */
    void *big = kmalloc(100000);
    K_ASSERT(big != NULL);
    memset(big, 0x5A, 100000);
    K_EXPECT_EQ(((u8 *)big)[0], 0x5A);
    K_EXPECT_EQ(((u8 *)big)[4095], 0x5A);
    K_EXPECT_EQ(((u8 *)big)[99999], 0x5A);
    kfree(big);
}

static void test_kheap_distinct_blocks(void)
{
    enum { N = 8 };
    void *p[N];

    for (int i = 0; i < N; i++) {
        p[i] = kmalloc(96);
        K_ASSERT(p[i] != NULL);
        memset(p[i], 0, 96);
    }
    for (int i = 0; i < N; i++)
        for (int j = i + 1; j < N; j++) K_EXPECT(p[i] != p[j]);

    /* distinctness matters only if the contents stay apart too */
    for (int i = 0; i < N; i++) ((u8 *)p[i])[0] = (u8)(0x10 + i);
    for (int i = 0; i < N; i++) K_EXPECT_EQ(((u8 *)p[i])[0], (u8)(0x10 + i));

    for (int i = 0; i < N; i++) kfree(p[i]);
}

/* ---- virtual memory ---- */

static void test_vmm_kernel_windows(void)
{
    u64 root = master_pml4_phys();
    K_ASSERT(root != 0);
    K_EXPECT_EQ(root & (PAGE_SIZE - 1), 0);

    u64 phys = 0, size = 0;
    vmm_heap_region(&phys, &size);
    K_ASSERT(size != 0);

    /* KHEAP_VMA must resolve to the physical heap, byte for byte, because
     * the window is what makes kmalloc's bin blocks writable at all */
    K_EXPECT_EQ(vmm_translate_in(root, KHEAP_VMA), phys);
    K_EXPECT_EQ(vmm_translate_in(root, KHEAP_VMA + 0x1000), phys + 0x1000);
    K_EXPECT_EQ(vmm_translate_in(root, KHEAP_VMA + size - PAGE_SIZE),
                phys + size - PAGE_SIZE);

    /* kernel text: identity through the high-half alias */
    K_EXPECT_EQ(vmm_translate_in(root, (u64)&test_vmm_kernel_windows),
                VIRT_TO_PHYS((uptr)&test_vmm_kernel_windows));

    /* an unmapped user address must read as "not present", not as a frame */
    K_EXPECT_EQ(vmm_translate_in(root, USER_BASE), 0);
}

static void test_vmm_user_range(void)
{
    K_EXPECT(vmm_is_user_range(USER_BASE, USER_BASE + PAGE_SIZE));
    K_EXPECT(vmm_is_user_range(USER_STACK_TOP, USER_MAX_VMA));
    K_EXPECT(!vmm_is_user_range(USER_BASE - 1, USER_BASE));   /* below the base */
    K_EXPECT(!vmm_is_user_range(USER_MAX_VMA, USER_MAX_VMA + 1)); /* above the top */
    K_EXPECT(!vmm_is_user_range(USER_BASE, USER_BASE));       /* empty range */
    K_EXPECT(!vmm_is_user_range(0, PAGE_SIZE));               /* kernel space */
    K_EXPECT(!vmm_is_user_range(USER_BASE, USER_MAX_VMA + 1)); /* one byte too far */
}

static void test_vmm_user_aspace(void)
{
    u64 before = pmm_free_bytes();
    u64 root = vmm_new_user_aspace();
    K_ASSERT(root != 0);
    K_EXPECT(root != master_pml4_phys()); /* a fresh table, not the master */

    u64 pa = pmm_alloc();
    K_ASSERT(pa != 0);
    memset((void *)PHYS_TO_VIRT(pa), 0x77, PAGE_SIZE);

    K_EXPECT_EQ(vmm_map_user(root, USER_BASE, pa, true, true, false), 0);
    K_EXPECT_EQ(vmm_translate_in(root, USER_BASE), pa);
    K_EXPECT_EQ(vmm_translate_in(root, USER_BASE + 0x800), pa + 0x800); /* offset kept */

    /* read the PTE itself: data pages must be non-executable (PG_NX) */
    u64 pte = raw_leaf(root, USER_BASE);
    K_ASSERT(pte != 0);
    K_EXPECT_EQ(pte & PG_NX, PG_NX);
    K_EXPECT_EQ(pte & PG_W, PG_W);
    K_EXPECT_EQ(pte & PG_U, PG_U);
    K_EXPECT_EQ(pte & PTE_PA_MASK, pa);

    /* re-mapping an existing page only widens: exec clears NX, but a
     * non-writable request must NOT strip PG_W that is already there */
    K_EXPECT_EQ(vmm_map_user(root, USER_BASE, 0, false, true, true), 0);
    pte = raw_leaf(root, USER_BASE);
    K_EXPECT_EQ(pte & PG_NX, 0);          /* now executable */
    K_EXPECT_EQ(pte & PG_W, PG_W);        /* still writable */
    K_EXPECT_EQ(pte & PTE_PA_MASK, pa);   /* same frame, not replaced */

    /* rejected before any page table is touched */
    K_EXPECT_EQ(vmm_map_user(root, KHEAP_VMA, pa, true, true, true), (u64)LNXRM_EFAIL);
    K_EXPECT_EQ(vmm_map_user(root, USER_BASE + 1, pa, true, true, true), (u64)LNXRM_EFAIL);
    K_EXPECT_EQ(vmm_map_user(root, USER_MAX_VMA, pa, true, true, true), (u64)LNXRM_EFAIL);

    K_EXPECT_EQ(vmm_unmap_user(root, USER_BASE), pa);
    K_EXPECT_EQ(vmm_translate_in(root, USER_BASE), 0);
    pmm_free(pa);

    /* destroy must reclaim the root plus every intermediate table */
    vmm_destroy_user_aspace(root);
    K_EXPECT_EQ(pmm_free_bytes(), before);
}

/* T-032: what the memory account is counted from.  vmm_count_user_pages()
 * is the number exec, fork and brk refresh a task's account from -- and
 * therefore the number ps prints for "who owns the RAM" -- so it has to
 * agree with the tables themselves, not with a list of pages somebody
 * remembers to write down.  Leaves the allocator where it found it, like
 * every other case in this file. */
static void test_vmm_count_user_pages(void)
{
    u64 before = pmm_free_bytes();
    u64 root = vmm_new_user_aspace();
    K_ASSERT(root != 0);
    K_EXPECT_EQ(vmm_count_user_pages(root), (u64)0); /* a fresh space owns nothing */

    u64 pa1 = pmm_alloc(), pa2 = pmm_alloc();
    K_ASSERT(pa1 != 0 && pa2 != 0);
    K_EXPECT_EQ(vmm_map_user(root, USER_BASE, pa1, true, true, false), 0);
    K_EXPECT_EQ(vmm_count_user_pages(root), (u64)1);

    /* a second VA, in its own 2 MiB table */
    K_EXPECT_EQ(vmm_map_user(root, USER_BASE + 0x400000, pa2, true, true, false), 0);
    K_EXPECT_EQ(vmm_count_user_pages(root), (u64)2);

    /* re-mapping an existing VA only widens it: one leaf, still one page */
    K_EXPECT_EQ(vmm_map_user(root, USER_BASE, 0, false, true, true), 0);
    K_EXPECT_EQ(vmm_count_user_pages(root), (u64)2);

    K_EXPECT_EQ(vmm_unmap_user(root, USER_BASE), pa1);
    K_EXPECT_EQ(vmm_count_user_pages(root), (u64)1);
    /* pa2 must go the same way: vmm_destroy_user_aspace() frees every
     * frame still mapped, so freeing it here too would hand the buddy the
     * same frame twice (a double free poisons its free lists, and the next
     * pmm_alloc() dies on a garbage pointer -- the #GP this test caused
     * before this line existed).  The pre-existing case above unmaps
     * before freeing for exactly this reason. */
    K_EXPECT_EQ(vmm_unmap_user(root, USER_BASE + 0x400000), pa2);
    K_EXPECT_EQ(vmm_count_user_pages(root), (u64)0);

    pmm_free(pa1);
    pmm_free(pa2);
    vmm_destroy_user_aspace(root);
    K_EXPECT_EQ(pmm_free_bytes(), before);
}

KTEST("pmm", KTEST_MM, test_pmm_buddy_accounting);
KTEST("pmm", KTEST_MM, test_pmm_high_alias);
KTEST("kheap", KTEST_MM, test_kheap_alloc_free);
KTEST("kheap", KTEST_MM, test_kheap_distinct_blocks);
KTEST("vmm", KTEST_MM, test_vmm_kernel_windows);
KTEST("vmm", KTEST_MM, test_vmm_user_range);
KTEST("vmm", KTEST_MM, test_vmm_user_aspace);
KTEST("vmm", KTEST_MM, test_vmm_count_user_pages);
