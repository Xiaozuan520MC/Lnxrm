/* The user/kernel copy boundary: user_ptr_ok() decides whether the kernel
 * may touch an address a user space handed it, and copy_from/to_user() are
 * the only sanctioned way across.  These are the assertions that start
 * failing the moment somebody "optimises" the per-page present check away
 * (a TOCTOU hole: checked here, unmapped before the copy runs).
 *
 * Every failure path is checked for two things: the call reports failure,
 * and the destination is byte-for-byte what it was before the call. */
#include <sys/ktest.h>
#include <console.h>
#include <mm/mm.h>
#include <sys/sched.h>
#include <sys/cpu.h>

/* the page this case works in; a different page from the ELF loader's so
 * the two cases cannot see each other's mappings */
#define TEST_VA (USER_BASE + 0x2000)

/* SMAP: copy_from/to_user carry their own AC window, but these tests also
 * probe the mapping with raw dereferences to see what actually landed --
 * those need a window of their own, or the first one #PFs the kernel. */
static u8 uread8(u64 va)
{
    u64 ac = smap_enter();
    u8 v = *(volatile u8 *)va;
    smap_leave(ac);
    return v;
}
static void umemset(u64 va, int v, size_t n)
{
    u64 ac = smap_enter();
    memset((void *)va, v, n);
    smap_leave(ac);
}

static void test_user_ptr_ok_bounds(void)
{
    u8 local[16];

    /* zero length is never a valid copy */
    K_EXPECT(!user_ptr_ok(TEST_VA, 0));

    /* kernel addresses: the stack we are running on, the heap window, the
     * high-half alias of the first megabyte */
    K_EXPECT(!user_ptr_ok((u64)local, sizeof(local)));
    K_EXPECT(!user_ptr_ok(KHEAP_VMA, 16));
    K_EXPECT(!user_ptr_ok(PHYS_TO_VIRT(0x100000), 0x1000));

    /* the window edges: one byte below the base, one byte past the top,
     * and ranges that straddle either edge */
    K_EXPECT(!user_ptr_ok(USER_BASE - 1, 2));
    K_EXPECT(!user_ptr_ok(USER_BASE - PAGE_SIZE, PAGE_SIZE));
    K_EXPECT(!user_ptr_ok(USER_MAX_VMA, 1));
    K_EXPECT(!user_ptr_ok(USER_MAX_VMA - 8, 16));
    K_EXPECT(!user_ptr_ok(USER_BASE - 8, 8));

    /* p + n wrapping around the top of the address space must not be
     * mistaken for an in-range interval (vmm_is_user_range requires lo<hi) */
    K_EXPECT(!user_ptr_ok(~0ULL - 4, 16));
    K_EXPECT(!user_ptr_ok(0xFFFFFFFFFFFFF000ULL, 0x1000));
    K_EXPECT(!user_ptr_ok(0x8000000000000000ULL, 8)); /* non-canonical */

    /* in range but unmapped: nothing in user space exists yet */
    K_EXPECT(!user_ptr_ok(USER_BASE, 1));
    K_EXPECT(!user_ptr_ok(TEST_VA, PAGE_SIZE));
}

static void test_copy_user_boundaries(void)
{
    u8 kbuf[16];
    u8 ref[8];
    u64 back;

    memset(kbuf, 0x5A, sizeof(kbuf));
    memset(ref, 0x44, sizeof(ref));

    /* ---- failures must leave the kernel buffer alone ---- */
    K_EXPECT(copy_from_user(kbuf, (const void *)TEST_VA, 8) != 0); /* unmapped */
    K_EXPECT_EQ(kbuf[0], 0x5A);
    K_EXPECT_EQ(kbuf[15], 0x5A);
    K_EXPECT(copy_from_user(kbuf, (const void *)(TEST_VA - 8), 8) != 0);
    K_EXPECT_EQ(kbuf[0], 0x5A);
    K_EXPECT(copy_from_user(kbuf, &ref[0], 8) != 0); /* kernel source */
    K_EXPECT(copy_from_user(kbuf, (const void *)(~0ULL - 4), 8) != 0); /* wrap */
    K_EXPECT_EQ(kbuf[0], 0x5A);
    K_EXPECT(copy_from_user(kbuf, (const void *)0, 8) != 0);
    K_EXPECT_EQ(kbuf[0], 0x5A);

    /* ---- warm up the intermediate page tables, then take the snapshot:
     * the first map into this address space's user half allocates PML4 ->
     * PDPT -> PD -> PT, and the invariant under test is "what you took is
     * what you gave back", not "the boot allocator never spent a frame". */
    u64 w = pmm_alloc();
    K_ASSERT(w != 0);
    K_EXPECT_EQ(vmm_map_user(current->pml4, TEST_VA, w, true, true, false), 0);
    back = vmm_unmap_user(current->pml4, TEST_VA);
    K_EXPECT_EQ(back, w);
    pmm_free(w);
    K_EXPECT_EQ(vmm_translate_in(current->pml4, TEST_VA), 0);

    u64 snap = pmm_free_bytes();
    u64 pa = pmm_alloc();
    K_ASSERT(pa != 0);
    K_EXPECT_EQ(vmm_map_user(current->pml4, TEST_VA, pa, true, true, false), 0);
    umemset(TEST_VA, 0x33, PAGE_SIZE);

    /* ---- one mapped page: positive cases ---- */
    K_EXPECT(user_ptr_ok(TEST_VA, 1));
    K_EXPECT(user_ptr_ok(TEST_VA, PAGE_SIZE));
    K_EXPECT(user_ptr_ok(TEST_VA + 0x800, 0x800));
    K_EXPECT(user_ptr_ok(TEST_VA + PAGE_SIZE - 8, 8)); /* ends on the edge */
    K_EXPECT(!user_ptr_ok(TEST_VA - 1, 2));            /* starts before it */
    K_EXPECT(!user_ptr_ok(TEST_VA + PAGE_SIZE - 1, 2)); /* crosses out */
    K_EXPECT(!user_ptr_ok(TEST_VA, PAGE_SIZE + 1));

    /* monotonicity: every sub-range of the unmapped neighbour page fails,
     * not just page-aligned addresses */
    for (u64 off = 0; off < PAGE_SIZE; off += 509)
        K_EXPECT(!user_ptr_ok(TEST_VA + PAGE_SIZE + off, 1));
    K_EXPECT(!user_ptr_ok(TEST_VA + PAGE_SIZE, PAGE_SIZE));

    /* ---- cross-page copy: the second page is not there, so the whole
     * call fails and the first page keeps its contents ---- */
    K_EXPECT(copy_from_user(kbuf, (const void *)(TEST_VA + PAGE_SIZE - 4), 8) != 0);
    K_EXPECT_EQ(kbuf[0], 0x5A);
    K_EXPECT(copy_to_user((void *)(TEST_VA + PAGE_SIZE - 4), ref, 8) != 0);
    K_EXPECT_EQ(uread8(TEST_VA + PAGE_SIZE - 4), 0x33);
    K_EXPECT_EQ(uread8(TEST_VA + PAGE_SIZE - 1), 0x33);

    /* kernel destination for a user copy must be refused too */
    K_EXPECT(copy_to_user((void *)kbuf, ref, 8) != 0);
    K_EXPECT_EQ(kbuf[0], 0x5A);

    /* ---- the happy path ---- */
    K_EXPECT_EQ(copy_from_user(kbuf, (const void *)TEST_VA, 16), 0);
    K_EXPECT_EQ(kbuf[0], 0x33);
    K_EXPECT_EQ(kbuf[15], 0x33);
    K_EXPECT_EQ(copy_to_user((void *)TEST_VA, ref, sizeof(ref)), 0);
    K_EXPECT_EQ(uread8(TEST_VA), 0x44);
    K_EXPECT_EQ(uread8(TEST_VA + 7), 0x44);
    K_EXPECT_EQ(uread8(TEST_VA + 8), 0x33); /* beyond n: untouched */

    /* ---- teardown: the frame comes back and the mapping goes away ---- */
    back = vmm_unmap_user(current->pml4, TEST_VA);
    K_EXPECT_EQ(back, pa);
    pmm_free(pa);
    K_EXPECT_EQ(vmm_translate_in(current->pml4, TEST_VA), 0);
    K_EXPECT_EQ(pmm_free_bytes(), snap);
}

/* A read-only user page: the kernel may READ it (copy_from_user) but must
 * REFUSE to store into it (copy_to_user).  With CR0.WP on, the store would
 * #PF in ring 0 and this kernel has no fixup table; before WP it landed and
 * silently corrupted the process's text/.rodata.  The frame is filled via
 * its high-half alias, because a store through TEST_VA would fault too. */
static void test_copy_to_user_readonly(void)
{
    u8 kbuf[8];
    u8 ref[8];
    u64 back;

    memset(ref, 0x44, sizeof(ref));
    u64 w = pmm_alloc();
    K_ASSERT(w != 0);
    K_EXPECT_EQ(vmm_map_user(current->pml4, TEST_VA, w, false, true, false), 0);
    memset((void *)PHYS_TO_VIRT(w), 0x33, PAGE_SIZE);

    K_EXPECT(user_ptr_ok(TEST_VA, 1));
    K_EXPECT(!user_ptr_writable(TEST_VA, 1));
    K_EXPECT(!user_ptr_writable(TEST_VA, PAGE_SIZE + 1)); /* runs off the page */

    /* reads ignore the R/W bit entirely */
    K_EXPECT_EQ(copy_from_user(kbuf, (const void *)TEST_VA, sizeof(kbuf)), 0);
    K_EXPECT_EQ(kbuf[0], 0x33);
    K_EXPECT_EQ(kbuf[7], 0x33);

    /* stores must be refused and leave the page byte-for-byte intact */
    K_EXPECT(copy_to_user((void *)TEST_VA, ref, sizeof(ref)) != 0);
    K_EXPECT_EQ(uread8(TEST_VA), 0x33);
    K_EXPECT_EQ(uread8(TEST_VA + 7), 0x33);
    /* half on the RO page, half on the unmapped one: still refused */
    K_EXPECT(copy_to_user((void *)(TEST_VA + PAGE_SIZE - 4), ref, 8) != 0);
    K_EXPECT_EQ(uread8(TEST_VA + PAGE_SIZE - 4), 0x33);

    back = vmm_unmap_user(current->pml4, TEST_VA);
    K_EXPECT_EQ(back, w);
    pmm_free(w);
    K_EXPECT_EQ(vmm_translate_in(current->pml4, TEST_VA), 0);
}

KTEST("usercopy", KTEST_LATE, test_user_ptr_ok_bounds);
KTEST("usercopy", KTEST_LATE, test_copy_user_boundaries);
KTEST("usercopy", KTEST_LATE, test_copy_to_user_readonly);
