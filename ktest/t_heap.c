/* kheap internals.  bin_of() decides which free list every small allocation
 * lands in: an off-by-one there hands back a block the caller then overruns,
 * and nothing complains until the heap is corrupt.  Both functions under
 * test are published through include/mm/mm.h for exactly this reason. */
#include <sys/ktest.h>
#include <console.h>
#include <mm/mm.h>

/* T-011: the documented boundaries 1, 8, 16, 17, 4096, 4097. */
static void test_bin_of(void)
{
    /* bin 0 covers everything up to and including 16 bytes */
    K_EXPECT_EQ(kheap_bin_of(1), 0);
    K_EXPECT_EQ(kheap_bin_of(8), 0);
    K_EXPECT_EQ(kheap_bin_of(16), 0);
    /* one byte past the boundary must move to bin 1 (32 bytes) */
    K_EXPECT_EQ(kheap_bin_of(17), 1);
    K_EXPECT_EQ(kheap_bin_of(32), 1);
    K_EXPECT_EQ(kheap_bin_of(33), 2);
    /* the last bin is KHEAP_NBINS-1 and covers 4096 */
    K_EXPECT_EQ(kheap_bin_of(4095), KHEAP_NBINS - 1);
    K_EXPECT_EQ(kheap_bin_of(4096), KHEAP_NBINS - 1);
    /* 4097 has no small bin.  kmalloc() routes it to the buddy *before*
     * calling bin_of (n + hdr > 4096), so this value is only ever a
     * sentinel -- but it must be out of range, not bin 8 with a lie. */
    K_EXPECT_EQ(kheap_bin_of(4097), KHEAP_NBINS);
    K_EXPECT(kheap_bin_of(4097) >= KHEAP_NBINS);

    /* Sweep every size kmalloc() could hand to bin_of(): the bin must be
     * real, must fit n, and must be the *smallest* bin that does. */
    size_t bad = 0;
    for (size_t n = 1; n <= 4096; n++) {
        int b = kheap_bin_of(n);
        bool fits = b >= 0 && b < KHEAP_NBINS && ((size_t)1 << (b + 4)) >= n;
        bool tightest = b <= 0 || ((size_t)1 << (b + 3)) < n;
        if (!fits || !tightest) bad++;
    }
    K_EXPECT_EQ(bad, 0);
}

/* T-012: the accounting half of the allocator -- everything handed out is
 * handed back, and the small/large paths both settle to zero. */
static void test_kheap_used_pairing(void)
{
    static const size_t sizes[] = {
        1,     8,     16,    17,   /* below / on / past the bin boundary */
        4088,  4089,  4095,  4096, /* 4088+hdr == 4096: last small request */
        65536, 100000,            /* buddy path (order >= 1) */
    };
    enum { N = sizeof(sizes) / sizeof(sizes[0]) };
    void *p[N];
    u64 used0 = kheap_used_bytes();
    u64 free0 = pmm_free_bytes();

    for (int i = 0; i < N; i++) {
        p[i] = kmalloc(sizes[i]);
        K_ASSERT(p[i] != NULL);
        memset(p[i], 0xC0 + i, sizes[i]);
        K_EXPECT(p[i] != NULL);
        for (size_t off = 0; off < sizes[i]; off += (sizes[i] / 3) + 1)
            K_EXPECT_EQ(((u8 *)p[i])[off], (u8)(0xC0 + i));
    }
    /* live blocks must never overlap */
    for (int i = 0; i < N; i++)
        for (int j = i + 1; j < N; j++) K_EXPECT(p[i] != p[j]);

    for (int i = 0; i < N; i++) kfree(p[i]);

    K_EXPECT_EQ(kheap_used_bytes(), used0);
    K_EXPECT_EQ(pmm_free_bytes(), free0); /* the buddy frames came back too */
}

KTEST("kheap", KTEST_EARLY, test_bin_of);
KTEST("kheap", KTEST_MM, test_kheap_used_pairing);
