/* T-004: running out of memory is an error return, never a panic.
 *
 * The branches under test are the ones a user program could reach by
 * looping brk()/fork() until the buddy was dry -- before this change each
 * of them stopped the machine instead of answering the call.  The buddy is
 * not actually emptied to prove it: pmm_inject_oom() makes the next frame
 * allocation behave as if the free lists were empty, so an OOM path can be
 * driven on a box that has plenty of memory.  (Exhausting RAM for real is
 * covered end to end by /bin/systest's brk.oom block, which gives every
 * frame straight back afterwards.)
 *
 * Every case must leave both allocators exactly as it found them. */
#include <sys/ktest.h>
#include <console.h>
#include <mm/mm.h>

/* kmalloc()'s buddy path: anything past a page skips the free lists, so one
 * injected failure lands in exactly the branch that used to panic. */
static void test_kmalloc_buddy_oom_returns_null(void)
{
    u64 used0 = kheap_used_bytes();
    u64 free0 = pmm_free_bytes();

    pmm_inject_oom(1);
    void *p = kmalloc(1 << 20);
    pmm_inject_oom(0);

    K_EXPECT(p == NULL);                    /* refused, not fatal */
    K_EXPECT_EQ(kheap_used_bytes(), used0); /* and not charged for it */
    K_EXPECT_EQ(pmm_free_bytes(), free0);

    p = kmalloc(1 << 20); /* the heap still hands memory out afterwards */
    K_ASSERT(p != NULL);
    kfree(p);
    K_EXPECT_EQ(kheap_used_bytes(), used0);
    K_EXPECT_EQ(pmm_free_bytes(), free0);
}

/* kmalloc()'s refill path: the small free list serves what is on it, then
 * carves a fresh page out of the buddy.  Draining a bin that nothing else
 * uses (bin 8: one 4096-byte chunk per page) until the refill is due makes
 * that branch deterministic, and the injected failure has to come back as
 * NULL instead of a panic. */
static void test_kmalloc_refill_oom_returns_null(void)
{
    enum { CAP = 64, CHUNK = 2048 }; /* CHUNK + header -> bin 8 */
    static void *stash[CAP];
    u64 used0 = kheap_used_bytes();
    u64 free0 = pmm_free_bytes();
    int n = 0;

    pmm_inject_oom(1);
    for (; n < CAP; n++) {
        void *q = kmalloc(CHUNK);
        if (!q) break;
        stash[n] = q;
    }
    pmm_inject_oom(0);

    K_EXPECT(n < CAP); /* the bin dried up and the refill said no */
    for (int i = 0; i < n; i++) kfree(stash[i]);
    K_EXPECT_EQ(kheap_used_bytes(), used0);
    K_EXPECT_EQ(pmm_free_bytes(), free0);
}

/* A request larger than the buddy can ever satisfy needs no injection at
 * all: MAX_ORDER caps it out and kmalloc must answer NULL. */
static void test_kmalloc_oversize_returns_null(void)
{
    u64 used0 = kheap_used_bytes();
    u64 free0 = pmm_free_bytes();

    K_EXPECT(kmalloc(1UL << 40) == NULL);
    K_EXPECT_EQ(kheap_used_bytes(), used0);
    K_EXPECT_EQ(pmm_free_bytes(), free0);
}

/* vmm had two panics of its own on the same paths: the frame for a new
 * aspace root (fork/exec) and the page tables vmm_map_user builds on the
 * way to a page (brk grows one PT every 2 MiB).  Both must report failure
 * and leave nothing half-built behind. */
static void test_vmm_oom_is_an_error_not_a_panic(void)
{
    u64 free0 = pmm_free_bytes();
    u64 root = vmm_new_user_aspace();
    K_ASSERT(root != 0);

    /* (a) the aspace root */
    pmm_inject_oom(1);
    u64 none = vmm_new_user_aspace();
    pmm_inject_oom(0);
    K_EXPECT_EQ(none, 0);

    /* (b) the first page table on the way to USER_BASE */
    u64 pa = pmm_alloc();
    K_ASSERT(pa != 0);
    pmm_inject_oom(1);
    int r = vmm_map_user(root, USER_BASE, pa, true, true, false);
    pmm_inject_oom(0);
    K_EXPECT_EQ((u64)r, (u64)LNXRM_ENOMEM);
    K_EXPECT_EQ(vmm_translate_in(root, USER_BASE), 0); /* nothing half-mapped */

    /* the same call succeeds once frames are available again */
    K_EXPECT_EQ(vmm_map_user(root, USER_BASE, pa, true, true, false), 0);
    K_EXPECT_EQ(vmm_translate_in(root, USER_BASE), pa);
    K_EXPECT_EQ(vmm_unmap_user(root, USER_BASE), pa);
    pmm_free(pa);

    vmm_destroy_user_aspace(root);
    K_EXPECT_EQ(pmm_free_bytes(), free0);
}

KTEST("oom", KTEST_MM, test_kmalloc_buddy_oom_returns_null);
KTEST("oom", KTEST_MM, test_kmalloc_refill_oom_returns_null);
KTEST("oom", KTEST_MM, test_kmalloc_oversize_returns_null);
KTEST("oom", KTEST_MM, test_vmm_oom_is_an_error_not_a_panic);
