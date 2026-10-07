/* T-013: property tests -- randomised inputs, fixed seed, no panic, no overrun.
 *
 * T-011/T-012 pin *hand-picked* inputs.  The defects this file exists for are
 * the ones a hand-written list does not reach: C16 (nclus truncation), C30
 * (overflow), C31 (buffer), C32 (fat_entries overflow), C33 (interval
 * semantics) are all "wrong at an offset nobody thought to type", and those
 * are exactly what random input walks into by accident.
 *
 * So the five surfaces the kernel parses -- the user/kernel copy boundary,
 * page tables, path lengths, ELF fields and FAT 8.3 directory entries -- are
 * fed a deterministic pseudo-random stream, and every one of the 10 000 inputs
 * must satisfy:
 *
 *   1. NOTHING PANICS.  A panic shows up as the smoke test's "[PANIC]" line or
 *      a boot timeout, so this one needs no assertion of its own: it is the
 *      absence of one.
 *   2. NOTHING OVERRUNS.  Every buffer a callee may write into is bracketed by
 *      canary bytes which are scanned afterwards.  The GAP_ANALYSIS asks for a
 *      "canary page" around the buffer; these buffers live on the stack or in
 *      a local struct rather than on a page of their own, so the guard is the
 *      same idea expressed as surrounding fill bytes.
 *   3. THE ANSWER MATCHES AN ORACLE.  Each case carries a second
 *      implementation of the contract, written from the header comments
 *      (types.h, mm/mm.h, fs/vfs.c, fat32_priv.h, elf.h) and answered by
 *      walking the page tables itself -- never by calling the function under
 *      test.
 *
 * REPRODUCIBILITY.  PROP_SEED is a constant and every case derives its own
 * stream from it (prop_mix(case_id)), so a case's inputs do not depend on the
 * order the linker happened to place the `.ktest` entries in.  A violation
 * prints case, seed and iteration number; replaying that case from that seed
 * regenerates the same inputs byte for byte.
 *
 * A violation is counted, not raised per iteration: 10 000 iterations must
 * never be able to flood the serial log, so only the first offender of a case
 * prints its details and every case ends with K_EXPECT_EQ(prop_bad, 0). */
#include <sys/ktest.h>
#include <console.h>
#include <mm/mm.h>
#include <sys/sched.h>
#include <sys/vfs.h>
#include <fat32_priv.h>
#include <elf.h>

#define PROP_SEED 0x9E3779B97F4A7C15ULL

enum {
    ITER_USERCOPY = 3000, /* user_ptr_ok / copy_*_user against an oracle */
    ITER_PAGETBL = 2000,  /* map -> translate -> unmap round trips */
    ITER_PATH = 2000,     /* vfs_abs_path: length rule + no overrun */
    ITER_FATNAME = 1500,  /* fat_short_to_name / fat_name_eq_short */
    ITER_ELF = 1500,      /* elf_load on mutated images */
    ITER_TOTAL = 10000
};

/* Frames one mutated ELF image may map before the case stops treating it as a
 * parse test and starts treating it as a resource test (see test_prop_elf). */
#define PROP_MAX_ELF_PAGES 8

static u64 rng_state;
static const char *prop_name;
static u64 prop_seed_val;
static u64 prop_iter, prop_iters;
static u64 prop_bad, prop_bad_total;
static u64 prop_done;
static bool prop_banner_shown, prop_bad_shown;

/* ---- the deterministic stream ---- */

static u64 prop_mix(u64 id)
{
    u64 x = PROP_SEED ^ (id * 0xD1B54A32D192ED03ULL);
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDULL;
    x ^= x >> 33;
    x *= 0xC4CEB9FE1A85EC53ULL;
    x ^= x >> 33;
    return x ? x : 1; /* a zero state would lock the generator at zero */
}

static u64 prop_rand(void)
{
    u64 x = rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    rng_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

static u32 prop_below(u32 n) { return n ? (u32)(prop_rand() % n) : 0; }

/* ---- reporting ---- */

static void prop_begin(const char *name, u64 case_id, u64 iters)
{
    prop_name = name;
    prop_seed_val = prop_mix(case_id);
    rng_state = prop_seed_val;
    prop_iter = 0;
    prop_iters = iters;
    prop_bad = 0;
    prop_bad_shown = false;
    if (!prop_banner_shown) {
        prop_banner_shown = true;
        kprintf("[prop] T-013 property tests, seed=0x%lx, %d iterations\n", PROP_SEED,
                ITER_TOTAL);
    }
}

/* Only the first offender of a case prints its details: everything after it
 * is the same bug, and 10 000 lines of it would drown the serial log. */
static void prop_check(bool ok, const char *what, u64 got, u64 want)
{
    if (ok) return;
    prop_bad++;
    prop_bad_total++;
    if (!prop_bad_shown) {
        prop_bad_shown = true;
        kprintf("[prop] T-013 FAIL %s seed=0x%lx iter=%lu: %s (got=0x%lx want=0x%lx)\n",
                prop_name, prop_seed_val, prop_iter, what, got, want);
    }
}

static void prop_end(void)
{
    prop_done += prop_iter;
    K_EXPECT_EQ(prop_iter, prop_iters); /* the budget was actually spent */
    K_EXPECT_EQ(prop_bad, 0);
    kprintf("[prop] T-013 %-9s %lu/%lu iterations seed=0x%lx violations=%lu\n", prop_name,
            prop_iter, prop_iters, prop_seed_val, prop_bad);
    if (prop_done == ITER_TOTAL)
        kprintf("[prop] T-013 total: %lu/%d iterations, %lu violations, seed=0x%lx\n", prop_done,
                ITER_TOTAL, prop_bad_total, PROP_SEED);
}

/* ===================================================================== *
 * 1. user_ptr_ok / copy_from_user / copy_to_user
 *
 * The oracle restates the contract instead of reusing the code under test:
 * "inside the window, not wrapping, every page present (and writable)" is
 * what types.h and mm/vmm.c promise, and the page answers come from walking
 * the tables with vmm_translate_in/vmm_writable_in.  Two probe pages are
 * mapped -- one writable, one read-only -- so the writable oracle has
 * something to disagree with, and the holes around them make a range that
 * runs off a page edge fail.
 * ===================================================================== */

#define COPY_RW_VA (USER_BASE + 0x4000)
#define COPY_RO_VA (USER_BASE + 0x5000)

static bool range_in_window(u64 p, u64 n)
{
    if (!n) return false;
    u64 hi = p + n;
    if (hi < p) return false; /* wrapped: "lo <= hi" would be a lie */
    return p >= USER_BASE && hi <= USER_MAX_VMA;
}

static bool range_present(u64 p, u64 n)
{
    if (!range_in_window(p, n)) return false;
    u64 lo = p & ~(PAGE_SIZE - 1), hi = (p + n - 1) & ~(PAGE_SIZE - 1);
    for (u64 pg = lo;; pg += PAGE_SIZE) {
        if (!vmm_translate_in(current->pml4, pg)) return false;
        if (pg == hi) break;
    }
    return true;
}

static bool range_writable(u64 p, u64 n)
{
    if (!range_in_window(p, n)) return false;
    u64 lo = p & ~(PAGE_SIZE - 1), hi = (p + n - 1) & ~(PAGE_SIZE - 1);
    for (u64 pg = lo;; pg += PAGE_SIZE) {
        if (!vmm_writable_in(current->pml4, pg)) return false;
        if (pg == hi) break;
    }
    return true;
}

/* Which of this case's pages, if any, holds all of [p, p+n):
 * 0 = someone else's page (contents not checkable, return code still is),
 * 1 = the writable probe, 2 = the read-only probe. */
static int owner_page(u64 p, u64 n)
{
    if (!n) return 0;
    /* Containment without an overflowing sum: pick_ptr() deliberately offers
     * pointers near ~0ULL, and "p + n <= base + PAGE_SIZE" wraps for those,
     * admitting a range whose snapshot index lands in the kernel half (#PF). */
    if (p >= COPY_RW_VA && p <= COPY_RW_VA + PAGE_SIZE &&
        n <= COPY_RW_VA + PAGE_SIZE - p)
        return 1;
    if (p >= COPY_RO_VA && p <= COPY_RO_VA + PAGE_SIZE &&
        n <= COPY_RO_VA + PAGE_SIZE - p)
        return 2;
    return 0;
}

static const u64 ptr_knobs[] = {
    USER_BASE,
    USER_BASE - 1,
    USER_BASE + 1,
    USER_BASE + PAGE_SIZE,
    COPY_RW_VA,
    COPY_RW_VA + PAGE_SIZE - 1,
    COPY_RW_VA + PAGE_SIZE, /* one page past the mapping */
    COPY_RO_VA,
    COPY_RO_VA + 0x800,
    USER_MAX_VMA - 1,
    USER_MAX_VMA,
    USER_MAX_VMA - PAGE_SIZE,
    0,
    1,
    8,
    KHEAP_VMA,
    PHYS_TO_VIRT(0x100000),
    ~0ULL - 4,
    0xFFFFFFFFFFFFF000ULL,
    0x8000000000000000ULL, /* non-canonical */
};

static u64 pick_ptr(void)
{
    if (prop_below(2) == 0) {
        /* half the inputs sit in the hole/mapping neighbourhood, or the
         * oracle would only ever see the cheap "not even in range" answer */
        return COPY_RW_VA - PAGE_SIZE + (u64)prop_below(3 * (u32)PAGE_SIZE);
    }
    u64 k = ptr_knobs[prop_below((u32)(sizeof(ptr_knobs) / sizeof(ptr_knobs[0])))];
    i64 jitter = (i64)prop_below(65) - 32;
    return k + (u64)jitter;
}

static u64 pick_len(void)
{
    if (prop_below(4) != 0) return prop_below(65); /* the copyable sizes */
    static const u64 knobs[] = {
        0,    1,     8,      16,     47,     48,      49,      64,   65,   4095,
        4096, 4097,  8192,   1ULL << 32, 1ULL << 40, 0xFFFFFFFFULL, ~0ULL, ~0ULL - 4,
        0x8000000000000000ULL, USER_MAX_VMA - USER_BASE,
    };
    return knobs[prop_below((u32)(sizeof(knobs) / sizeof(knobs[0])))];
}

struct kbuf {
    u8 pre[16];
    u8 buf[64];
    u8 post[16];
};

/* bytes of the three regions that no longer hold their fill pattern */
static u64 canary_damage(const u8 *pre, size_t pre_n, const u8 *buf, size_t buf_n, u8 buf_pat,
                         const u8 *post, size_t post_n)
{
    u64 bad = 0;
    for (size_t i = 0; i < pre_n; i++)
        if (pre[i] != 0xA1) bad++;
    for (size_t i = 0; i < buf_n; i++)
        if (buf[i] != buf_pat) bad++;
    for (size_t i = 0; i < post_n; i++)
        if (post[i] != 0xC5) bad++;
    return bad;
}

static void test_prop_usercopy(void)
{
    prop_begin("usercopy", 1, ITER_USERCOPY);

    /* Warm the intermediate page tables up first, then take the snapshot: the
     * first map into this address space's user half spends PML4 -> PDPT -> PD
     * -> PT frames, and the invariant under test is "everything this case
     * allocated came back", not "the boot allocator never spent a frame"
     * (same reasoning as t_usercopy.c). */
    u64 warm = pmm_alloc();
    if (!warm || vmm_map_user(current->pml4, COPY_RW_VA, warm, true, true, false) != 0) {
        prop_check(0, "warming up the page tables", 0, 1);
        if (warm) pmm_free(warm);
        prop_end();
        return;
    }
    vmm_unmap_user(current->pml4, COPY_RW_VA);
    pmm_free(warm);

    /* The probe frames are allocated *after* the snapshot: teardown frees
     * them again, so free bytes must come back to exactly this figure. */
    u64 snap = pmm_free_bytes();
    u64 rw = pmm_alloc(), ro = pmm_alloc();
    u8 *rw_page = rw ? (u8 *)PHYS_TO_VIRT(rw) : NULL;
    u8 *ro_page = ro ? (u8 *)PHYS_TO_VIRT(ro) : NULL;

    if (!rw || !ro) {
        prop_check(0, "pmm_alloc for the probe pages", 0, 1);
        if (rw) pmm_free(rw);
        if (ro) pmm_free(ro);
        prop_end();
        return;
    }
    /* position-dependent patterns: a copy that lands at the wrong offset, or
     * comes from the wrong page, cannot pass by matching a constant */
    for (u64 i = 0; i < PAGE_SIZE; i++) {
        rw_page[i] = (u8)(i * 7 + 0x11);
        ro_page[i] = (u8)(0x80 | (i & 0x7F));
    }

    if (vmm_map_user(current->pml4, COPY_RW_VA, rw, true, true, false) != 0 ||
        vmm_map_user(current->pml4, COPY_RO_VA, ro, false, true, false) != 0) {
        prop_check(0, "vmm_map_user probe pages", 0, 1);
        if (vmm_translate_in(current->pml4, COPY_RW_VA)) vmm_unmap_user(current->pml4, COPY_RW_VA);
        if (vmm_translate_in(current->pml4, COPY_RO_VA)) vmm_unmap_user(current->pml4, COPY_RO_VA);
        pmm_free(rw);
        pmm_free(ro);
        prop_end();
        return;
    }

    for (; prop_iter < prop_iters; prop_iter++) {
        u64 p = pick_ptr();
        u64 n = pick_len();

        /* ---- the range predicates ---- */
        bool want_ok = range_present(p, n);
        bool got_ok = user_ptr_ok(p, n);
        prop_check(got_ok == want_ok, "user_ptr_ok disagrees with the page-table walk", got_ok,
                   want_ok);

        bool want_w = range_writable(p, n);
        bool got_w = user_ptr_writable(p, n);
        prop_check(got_w == want_w, "user_ptr_writable disagrees with the PTE", got_w, want_w);

        if (want_ok) {
            /* monotonicity: every suffix of a valid range is valid too */
            u64 off = prop_below((u32)(n > 0x1000 ? 0x1000 : n));
            prop_check(user_ptr_ok(p + off, n - off), "suffix of a valid range refused", 0, 1);
            prop_check(user_ptr_ok(p, 1), "first byte of a valid range refused", 0, 1);
            prop_check(user_ptr_ok(p + n - 1, 1), "last byte of a valid range refused", 0, 1);
        }

        /* ---- copy_from_user: return code, canaries, contents ---- */
        u64 cn = (n <= sizeof(((struct kbuf *)0)->buf)) ? n : (u64)prop_below(65);
        struct kbuf kb;
        memset(kb.pre, 0xA1, sizeof(kb.pre));
        memset(kb.buf, 0x5C, sizeof(kb.buf));
        memset(kb.post, 0xC5, sizeof(kb.post));

        int own = owner_page(p, cn);
        u8 before[64];
        memset(before, 0, sizeof(before));
        if (own) {
            u8 *page = own == 1 ? rw_page : ro_page;
            u64 base = own == 1 ? COPY_RW_VA : COPY_RO_VA;
            for (u64 k = 0; k < cn; k++) before[k] = page[(p + k) - base];
        }

        int rc = copy_from_user(kb.buf, (const void *)p, cn);
        bool want_c = range_present(p, cn);
        prop_check((rc == 0) == want_c, "copy_from_user return code vs oracle", (u64)(rc == 0),
                   (u64)want_c);
        /* the guard regions must survive either way -- but on success the
         * buffer's first cn bytes are *supposed* to change, so only the
         * guards are checked here (the tail against the fill pattern below) */
        prop_check(canary_damage(kb.pre, sizeof(kb.pre), kb.buf, 0, 0, kb.post,
                                 sizeof(kb.post)) == 0,
                   "copy_from_user overran into the canaries", 1, 0);
        if (rc == 0) {
            if (own)
                for (u64 k = 0; k < cn; k++)
                    prop_check(kb.buf[k] == before[k], "copy_from_user returned wrong bytes",
                               kb.buf[k], before[k]);
            for (size_t k = (size_t)cn; k < sizeof(kb.buf); k++)
                prop_check(kb.buf[k] == 0x5C, "copy_from_user wrote past its own length",
                           kb.buf[k], 0x5C);
        } else {
            for (size_t k = 0; k < sizeof(kb.buf); k++)
                prop_check(kb.buf[k] == 0x5C, "a failed copy_from_user touched the buffer",
                           kb.buf[k], 0x5C);
        }

        /* ---- copy_to_user: the page is the proof, the source is the caller's ---- */
        struct kbuf src;
        memset(src.pre, 0x3C, sizeof(src.pre));
        memset(src.post, 0x9B, sizeof(src.post));
        for (size_t k = 0; k < sizeof(src.buf); k++) src.buf[k] = (u8)(k * 3 + 1);

        int rc2 = copy_to_user((void *)p, src.buf, cn);
        bool want_w2 = range_writable(p, cn);
        prop_check((rc2 == 0) == want_w2, "copy_to_user return code vs oracle", (u64)(rc2 == 0),
                   (u64)want_w2);
        prop_check(src.pre[0] == 0x3C && src.pre[15] == 0x3C && src.post[0] == 0x9B &&
                       src.post[15] == 0x9B,
                   "copy_to_user touched its own source canary", 1, 0);
        if (own) {
            u8 *page = own == 1 ? rw_page : ro_page;
            u64 base = own == 1 ? COPY_RW_VA : COPY_RO_VA;
            for (u64 k = 0; k < cn; k++) {
                u8 got = page[(p + k) - base];
                if (rc2 == 0)
                    prop_check(got == src.buf[k], "copy_to_user stored the wrong bytes", got,
                               src.buf[k]);
                else
                    prop_check(got == before[k], "a refused copy_to_user still wrote the page",
                               got, before[k]);
            }
        }
    }

    /* ---- teardown: both mappings go, both frames come back ---- */
    u64 got = vmm_unmap_user(current->pml4, COPY_RW_VA);
    prop_check(got == rw, "unmap of the writable probe page", got, rw);
    got = vmm_unmap_user(current->pml4, COPY_RO_VA);
    prop_check(got == ro, "unmap of the read-only probe page", got, ro);
    pmm_free(rw);
    pmm_free(ro);
    prop_check(vmm_translate_in(current->pml4, COPY_RW_VA) == 0, "probe page still mapped", 1, 0);
    prop_check(vmm_translate_in(current->pml4, COPY_RO_VA) == 0, "probe page still mapped", 1, 0);
    prop_check(pmm_free_bytes() == snap, "probe frames leaked", pmm_free_bytes(), snap);

    prop_end();
}

/* ===================================================================== *
 * 2. page tables: map -> translate -> unmap, one random slot per step
 *
 * Eight user pages toggled by a random walk.  After every transition the
 * whole pool is re-read: the frame that was mapped, the offset inside it, the
 * leaf PTE's flags, every *other* slot (a map must not disturb its
 * neighbours) and vmm_count_user_pages() -- the number exec/fork/brk refresh
 * a task's account from (T-032).  A map that must be refused has to cost
 * exactly nothing.
 * ===================================================================== */

#define PT_SLOTS 8

/* Raw leaf PTE for `va` under `root` (0 if any level is missing).  The flags
 * are the property: a data page that lost PG_NX, or never got PG_U, still
 * translates correctly, so translate-only assertions would wave it through. */
static u64 prop_raw_leaf(u64 root, u64 va)
{
    u64 i4 = (va >> 39) & 511, i3 = (va >> 30) & 511;
    u64 i2 = (va >> 21) & 511, i1 = (va >> 12) & 511;
    u64 *l4 = (u64 *)PHYS_TO_VIRT(root);
    if (!(l4[i4] & PG_P)) return 0;
    u64 *l3 = (u64 *)PHYS_TO_VIRT(l4[i4] & PTE_PA_MASK);
    if (!(l3[i3] & PG_P)) return 0;
    u64 *l2 = (u64 *)PHYS_TO_VIRT(l3[i3] & PTE_PA_MASK);
    if (!(l2[i2] & PG_P) || (l2[i2] & PG_PS)) return 0;
    u64 *l1 = (u64 *)PHYS_TO_VIRT(l2[i2] & PTE_PA_MASK);
    if (!(l1[i1] & PG_P)) return 0;
    return l1[i1];
}

static void test_prop_page_table(void)
{
    prop_begin("page_table", 2, ITER_PAGETBL);

    u64 snap = pmm_free_bytes();
    u64 root = vmm_new_user_aspace();
    if (!root) {
        prop_check(0, "vmm_new_user_aspace", 0, 1);
        prop_end();
        return;
    }

    u64 frame[PT_SLOTS];
    for (int i = 0; i < PT_SLOTS; i++) frame[i] = 0;
    u64 mapped = 0; /* vmm_count_user_pages(root) must equal this, always */

    static const u64 bad_va[] = {
        USER_BASE + 1,   USER_MAX_VMA, USER_BASE - PAGE_SIZE, KHEAP_VMA, 0x1234,
    };

    for (; prop_iter < prop_iters; prop_iter++) {
        u32 slot = prop_below(PT_SLOTS);
        u64 va = USER_BASE + (u64)slot * PAGE_SIZE;

        if (!frame[slot]) {
            u64 pa = pmm_alloc();
            if (!pa) {
                prop_check(0, "pmm_alloc for a probe page", 0, 1);
                break;
            }
            u8 *p = (u8 *)PHYS_TO_VIRT(pa);
            for (u64 i = 0; i < PAGE_SIZE; i++) p[i] = (u8)(slot * 31 + i);
            int rc = vmm_map_user(root, va, pa, true, true, false);
            prop_check(rc == 0, "vmm_map_user refused a legal page", (u64)rc, 0);
            if (rc) {
                pmm_free(pa);
                continue;
            }
            frame[slot] = pa;
            mapped++;

            prop_check(vmm_translate_in(root, va) == pa, "translate != the frame mapped", va, pa);
            u64 off = prop_below((u32)PAGE_SIZE);
            prop_check(vmm_translate_in(root, va + off) == pa + off, "translate lost the offset",
                       va + off, pa + off);

            u64 pte = prop_raw_leaf(root, va);
            prop_check(pte != 0, "no leaf PTE after a successful map", va, 0);
            prop_check((pte & PG_P) != 0, "leaf PTE not present", pte, PG_P);
            prop_check((pte & PG_U) != 0, "user page without PG_U", pte, PG_U);
            prop_check((pte & PG_W) != 0, "writable page without PG_W", pte, PG_W);
            prop_check((pte & PG_NX) == PG_NX, "data page lost PG_NX", pte & PG_NX, PG_NX);
            prop_check((pte & PTE_PA_MASK) == pa, "leaf PTE points at the wrong frame",
                       pte & PTE_PA_MASK, pa);
        } else {
            u64 pa = vmm_unmap_user(root, va);
            prop_check(pa == frame[slot], "unmap returned the wrong frame", pa, frame[slot]);
            prop_check(vmm_translate_in(root, va) == 0, "still mapped after unmap", va, 0);
            prop_check(prop_raw_leaf(root, va) == 0, "leaf PTE survived unmap", 1, 0);
            pmm_free(pa);
            frame[slot] = 0;
            mapped--;
        }

        /* every other slot must still read exactly what it did */
        for (u32 j = 0; j < PT_SLOTS; j++) {
            u64 want = frame[j];
            u64 got = vmm_translate_in(root, USER_BASE + (u64)j * PAGE_SIZE);
            prop_check(got == want, "a neighbouring slot moved", got, want);
        }
        prop_check(vmm_count_user_pages(root) == mapped, "page count drifted",
                   vmm_count_user_pages(root), mapped);

        /* an illegal address (unaligned / out of window / kernel) must cost
         * exactly nothing: no frame, no leaf, no counter tick */
        if ((prop_iter & 15) == 0) {
            u32 idx = prop_below(6);
            u64 bva = idx < sizeof(bad_va) / sizeof(bad_va[0]) ? bad_va[idx]
                                                               : (u64)prop_below(0x1000);
            u64 before = pmm_free_bytes();
            int rc = vmm_map_user(root, bva, 0x2000, true, true, false);
            prop_check(rc != 0, "illegal address accepted by vmm_map_user", bva, 0);
            prop_check(pmm_free_bytes() == before, "a refused map spent a frame",
                       pmm_free_bytes(), before);
            prop_check(vmm_count_user_pages(root) == mapped, "a refused map changed the count",
                       vmm_count_user_pages(root), mapped);
        }
    }

    for (int i = 0; i < PT_SLOTS; i++) {
        if (!frame[i]) continue;
        u64 va = USER_BASE + (u64)i * PAGE_SIZE;
        u64 pa = vmm_unmap_user(root, va);
        prop_check(pa == frame[i], "teardown unmap returned the wrong frame", pa, frame[i]);
        pmm_free(pa);
        frame[i] = 0;
    }
    prop_check(vmm_count_user_pages(root) == 0, "pages left after teardown",
               vmm_count_user_pages(root), 0);
    vmm_destroy_user_aspace(root);
    prop_check(pmm_free_bytes() == snap, "page-table case leaked frames", pmm_free_bytes(), snap);

    prop_end();
}

/* ===================================================================== *
 * 3. vfs_abs_path: the length rule, and never one byte outside [0, bufsz)
 *
 * From the header comment in fs/vfs.c the whole answer is a function of the
 * path length L and the buffer size: an absolute path always passes through
 * (same pointer, buffer untouched), a relative one fits exactly when
 * L <= bufsz - 3 -- "/" + L + NUL -- and anything longer is refused rather
 * than cut short.  The buffer is bracketed by canaries on both sides, so
 * "refused" must still not mean "written past the end on the way to
 * refusing".
 * ===================================================================== */

#define PATH_CAP 64

struct path_slot {
    u8 pre[16];
    char buf[PATH_CAP];
    u8 post[16];
};

static size_t gen_path(char *in, size_t cap)
{
    static const char alphabet[] = "abcxyzABCXYZ0189._-/";
    size_t body;
    switch (prop_below(6)) {
    case 0:
        body = prop_below(30);
        break; /* comfortably small */
    case 1:
        body = 56 + prop_below(24);
        break; /* straddles a 64-byte buffer */
    case 2:
        body = prop_below(8);
        break; /* degenerate */
    case 3:
        body = PATH_CAP + prop_below(40);
        break; /* must be refused */
    default:
        body = prop_below(150);
        break;
    }
    if (body > cap - 2) body = cap - 2;

    size_t i = 0;
    if (prop_below(4) == 0) in[i++] = '/'; /* absolute half of the inputs */
    for (size_t k = 0; k < body && i + 1 < cap; k++)
        in[i++] = alphabet[prop_below((u32)(sizeof(alphabet) - 1))];
    in[i] = 0;
    return i;
}

static u64 canary_damage_ee(const u8 *region, size_t n)
{
    u64 bad = 0;
    for (size_t i = 0; i < n; i++)
        if (region[i] != 0xEE) bad++;
    return bad;
}

static u64 path_damage(const struct path_slot *s)
{
    return canary_damage_ee(s->pre, sizeof(s->pre)) + canary_damage_ee(s->post, sizeof(s->post));
}

static void test_prop_path(void)
{
    prop_begin("path", 3, ITER_PATH);

    char in[160];
    static const u8 bufsz_tab[] = {0, 1, 2, 3, 4, 8, 16, 32, 48, 63, 64};

    for (; prop_iter < prop_iters; prop_iter++) {
        size_t len = gen_path(in, sizeof(in));
        size_t bufsz = bufsz_tab[prop_below((u32)(sizeof(bufsz_tab) / sizeof(bufsz_tab[0])))];

        struct path_slot s;
        memset(&s, 0xEE, sizeof(s));

        const char *r = vfs_abs_path(in, s.buf, bufsz);

        /* invariant: not one byte outside the window, whatever happened */
        prop_check(path_damage(&s) == 0, "abs_path wrote outside its buffer", 1, 0);

        if (in[0] == '/') {
            /* absolute paths pass through untouched: same pointer, no write */
            prop_check(r == in, "absolute path was not returned as given", (u64)r, (u64)in);
            prop_check(canary_damage_ee((const u8 *)s.buf, sizeof(s.buf)) == 0,
                       "an absolute path still wrote the buffer", 1, 0);
            continue;
        }

        /* invariant: the answer is exactly what the length rule predicts.
         * bufsz < 2 is refused outright, which is why that case is folded in
         * rather than left to the (size_t) underflow of bufsz - 3. */
        bool fits =
            bufsz >= 2 && (len == 0 || (bufsz >= 3 && len <= bufsz - 3));
        prop_check((r != NULL) == fits, "abs_path length rule disagrees with the oracle",
                   (u64)(r != NULL), (u64)fits);

        if (r) {
            prop_check(r == s.buf, "relative path returned something other than the buffer",
                       (u64)r, (u64)s.buf);
            prop_check(s.buf[0] == '/', "normalised path does not start with '/'", s.buf[0], '/');
            prop_check(strlen(s.buf) == len + 1, "normalised length != 1 + path length",
                       strlen(s.buf), len + 1);
            prop_check(strcmp(s.buf + 1, in) == 0, "abs_path rewrote the path", 1, 0);
            prop_check(strlen(s.buf) < bufsz, "result does not fit the buffer", strlen(s.buf),
                       bufsz);
            if (bufsz >= 3) /* the loop can only reach index bufsz-2 */
                prop_check((u8)s.buf[bufsz - 1] == 0xEE, "the window's last byte was written",
                           (u8)s.buf[bufsz - 1], 0xEE);
            for (size_t i = strlen(s.buf) + 1; i < sizeof(s.buf); i++)
                prop_check((u8)s.buf[i] == 0xEE, "wrote past the terminator", (u8)s.buf[i], 0xEE);

            /* idempotence: normalising an already absolute path is a no-op */
            struct path_slot s2;
            memset(&s2, 0xEE, sizeof(s2));
            const char *r2 = vfs_abs_path(s.buf, s2.buf, sizeof(s2.buf));
            prop_check(r2 == s.buf, "re-normalising a normalised path moved it", (u64)r2,
                       (u64)s.buf);
            prop_check(path_damage(&s2) == 0, "re-normalising wrote the buffer", 1, 0);
        } else {
            /* refused: buf needs nothing written to be safe, but the window
             * must end where the caller said it did */
            if (bufsz <= 1)
                prop_check(canary_damage_ee((const u8 *)s.buf, sizeof(s.buf)) == 0,
                           "a refusal with no room still wrote the buffer", 1, 0);
            else if (bufsz >= 3)
                prop_check((u8)s.buf[bufsz - 1] == 0xEE,
                           "a refused path wrote the window's last byte", (u8)s.buf[bufsz - 1],
                           0xEE);
        }
    }

    prop_end();
}

/* ===================================================================== *
 * 4. the FAT 8.3 codec: fat_short_to_name / fat_name_eq_short
 *
 * fat_short_to_name() promises 13 bytes; what each input must produce is
 * worked out here from the contract (base stops at the first space, an
 * extension exists only if byte 8 is not a space, the separator dot lands
 * exactly between them, the NT case bits act per half, nothing follows the
 * terminator).  The match half: when the base is one contiguous run with no
 * embedded dot -- the only shape that survives name_to_short()'s split -- the
 * display name must match the entry it came from, and neither case nor an
 * extension longer than the three characters 8.3 stores may change an answer.
 * ===================================================================== */

/* A canonical 8.3 entry: each field is one contiguous run of non-space
 * bytes followed by space padding -- the only shape the codec ever writes.
 * (An embedded space in a random field is where the renderer stops while
 * the raw entry keeps the bytes after it, so no display name can match.) */
static void gen_short(u8 sn[11])
{
    static const u8 base_chars[] = {'A', 'Z', 'a', 'z', '0', '9', '-', '_', '+',
                                    0x05, 0xE5, 0x80, 0xFF, 0x7F, '"', '.'};
    static const u8 ext_chars[] = {'A', 'Z', 'a', 'z', '0', '9', '-', '_', '+'};
    int bl = (int)prop_below(9); /* base length 0..8 */
    int el = (int)prop_below(4); /* extension length 0..3 */
    memset(sn, ' ', 11);
    for (int i = 0; i < bl; i++)
        sn[i] = base_chars[prop_below((u32)sizeof(base_chars))];
    for (int i = 0; i < el; i++)
        sn[8 + i] = ext_chars[prop_below((u32)sizeof(ext_chars))];
}

static void test_prop_fat_name(void)
{
    prop_begin("fat_name", 4, ITER_FATNAME);

    struct { /* the tail proves fat_short_to_name kept to its 13 bytes */
        char out[13];
        u8 tail[16];
    } o;

    for (; prop_iter < prop_iters; prop_iter++) {
        u8 sn[11];
        u8 ntres = (u8)prop_below(256);
        gen_short(sn);

        memset(&o, 0xEE, sizeof(o));
        fat_short_to_name(sn, ntres, o.out);
        prop_check(canary_damage_ee(o.tail, sizeof(o.tail)) == 0,
                   "fat_short_to_name overran its 13 bytes", 1, 0);

        /* what the result must look like, worked out from the contract */
        int b = 0;
        while (b < 8 && sn[b] != ' ') b++; /* base = bytes 0..b-1, b <= 8 */
        bool has_ext = sn[8] != ' ';       /* the dot exists only if byte 8 does */
        int e = 0;
        if (has_ext) {
            int i = 8;
            while (i < 11 && sn[i] != ' ') {
                e++;
                i++;
            }
        }
        size_t want_len = (size_t)b + (has_ext ? 1u + (size_t)e : 0u);

        size_t got_len = 0;
        while (got_len < sizeof(o.out) && o.out[got_len]) got_len++;
        prop_check(got_len < sizeof(o.out), "not NUL terminated inside the 13 bytes", got_len,
                   sizeof(o.out));
        prop_check(got_len == want_len, "wrong rendered length", got_len, want_len);

        bool lower_base = (ntres & FAT_NTRES_LOWER_BASE) != 0;
        bool lower_ext = (ntres & FAT_NTRES_LOWER_EXT) != 0;
        for (int i = 0; i < b; i++) {
            char want = lower_base ? ascii_tolower((char)sn[i]) : (char)sn[i];
            prop_check((u8)o.out[i] == (u8)want, "base byte ignores the NT bit", (u8)o.out[i],
                       (u8)want);
        }
        if (has_ext) {
            prop_check((u8)o.out[b] == '.', "separator dot missing or misplaced", (u8)o.out[b],
                       (u8)'.');
            for (int j = 0; j < e; j++) {
                char want = lower_ext ? ascii_tolower((char)sn[8 + j]) : (char)sn[8 + j];
                prop_check((u8)o.out[b + 1 + j] == (u8)want, "ext byte ignores the NT bit",
                           (u8)o.out[b + 1 + j], (u8)want);
            }
        }
        prop_check(want_len < sizeof(o.out) && (u8)o.out[want_len] == 0,
                   "byte after the name is not the terminator", (u8)o.out[want_len], 0);

        /* ---- matching, and what may never change an answer ---- */
        struct {
            struct fat_dirent e;
            u8 tail[16];
        } d;
        memset(&d, 0xEE, sizeof(d));
        memset(&d.e, 0, sizeof(d.e));
        memcpy(d.e.name, sn, 11);
        d.e.ntres = ntres;
        struct fat_dirent ref = d.e; /* the dirent is the caller's: snapshot it */

        bool base_clean = true;
        for (int i = 0; i < b && base_clean; i++)
            if (sn[i] == '.') base_clean = false;
        for (int i = b; i < 8 && base_clean; i++)
            if (sn[i] != ' ') base_clean = false;
        /* and the extension must be one run too: name_to_short() pads the
         * fields with spaces, so a byte after an embedded space in sn[8..11)
         * can never be matched back out of the display name */
        {
            bool gap = false;
            for (int i = 8; i < 11 && base_clean; i++) {
                if (sn[i] == ' ') gap = true;
                else if (gap) base_clean = false;
            }
        }

        bool direct = fat_name_eq_short(o.out, &d.e);
        if (base_clean)
            prop_check(direct, "display name does not match the entry it came from", 0, 1);

        /* case: flipping letters anywhere must not move the answer */
        char alt[16];
        memset(alt, 0, sizeof(alt));
        strncpy(alt, o.out, sizeof(alt) - 1);
        for (size_t i = 0; alt[i]; i++) {
            if (alt[i] >= 'a' && alt[i] <= 'z' && prop_below(2)) alt[i] -= 32;
            else if (alt[i] >= 'A' && alt[i] <= 'Z' && prop_below(2)) alt[i] += 32;
        }
        u64 alt_hits = fat_name_eq_short(alt, &d.e);
        prop_check(alt_hits == (u64)direct, "case changed a name match", alt_hits, (u64)direct);

        /* an extension past three characters: 8.3 stores three, so the extra
         * ones must be invisible -- but only when the extension is full */
        if (has_ext && e == 3 && strlen(o.out) + 3 <= sizeof(alt)) {
            memset(alt, 0, sizeof(alt));
            strncpy(alt, o.out, sizeof(alt) - 1);
            size_t ext_at = strlen(alt);
            alt[ext_at] = 'z';
            alt[ext_at + 1] = 'z';
            alt[ext_at + 2] = 0;
            u64 long_hits = fat_name_eq_short(alt, &d.e);
            prop_check(long_hits == (u64)direct, "a 4+ character extension mismatched",
                       long_hits, (u64)direct);
        }

        /* matching must not write a byte of the entry it was handed */
        prop_check(memcmp(&d.e, &ref, sizeof(ref)) == 0, "fat_name_eq_short wrote the dirent", 1,
                   0);
        prop_check(canary_damage_ee(d.tail, sizeof(d.tail)) == 0,
                   "fat_name_eq_short wrote past the dirent", 1, 0);
    }

    prop_end();
}

/* ===================================================================== *
 * 5. elf_load on mutated images
 *
 * Each iteration rebuilds a known-good ET_EXEC, applies one to three random
 * field mutations from a table of boundary values (0, 1, the ident bytes,
 * image-relative sizes, the window edges, all-ones, a plain random word), and
 * hands it to elf_load() against a throwaway address space.  Required: no
 * write outside the image buffer, no entry point outside user space, no
 * *brk_end write on a rejection, and -- once the address space is destroyed
 * again -- exactly as many free bytes as before.
 *
 * An image that would map more than PROP_MAX_ELF_PAGES frames is rejected at
 * the header level instead: spending all of QEMU's RAM is a resource bug, and
 * making boot depend on how much memory the machine has would turn this case
 * into a coin flip.  The extent is an *upper* bound -- the same conditions
 * elf_load needs to reach its map loop, minus the file-content checks, which
 * only ever make it map less.
 * ===================================================================== */

#define ELF_IMG 1024

static void build_good_elf(u8 *img)
{
    memset(img, 0, ELF_IMG);
    Elf64_Ehdr *eh = (Elf64_Ehdr *)img;
    memcpy(eh->e_ident, "\x7f"
                        "ELF",
           4);
    eh->e_ident[4] = 2; /* ELFCLASS64 */
    eh->e_ident[5] = 1; /* ELFDATA2LSB */
    eh->e_ident[6] = 1; /* EV_CURRENT */
    eh->e_type = 2;     /* ET_EXEC */
    eh->e_machine = 62; /* EM_X86_64 */
    eh->e_version = 1;
    eh->e_entry = USER_BASE + 0x1000;
    eh->e_phoff = sizeof(Elf64_Ehdr);
    eh->e_ehsize = sizeof(Elf64_Ehdr);
    eh->e_phentsize = sizeof(Elf64_Phdr);
    eh->e_phnum = 1;

    Elf64_Phdr *ph = (Elf64_Phdr *)(img + eh->e_phoff);
    ph->p_type = PT_LOAD;
    ph->p_flags = PF_R | PF_W | PF_X;
    ph->p_offset = 0x200;
    ph->p_vaddr = USER_BASE + 0x1000;
    ph->p_filesz = 0;
    ph->p_memsz = 0x1000;
    ph->p_align = 0x1000;
}

static u64 elf_map_extent(const u8 *img, size_t imgsize)
{
    if (imgsize < sizeof(Elf64_Ehdr)) return 0;
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)img;
    if (memcmp(eh->e_ident,
               "\x7f"
               "ELF",
               4) ||
        eh->e_ident[4] != 2)
        return 0;
    if (eh->e_type != 2) return 0;
    if (eh->e_entry < USER_BASE || eh->e_entry >= USER_MAX_VMA) return 0;
    if (eh->e_phoff > imgsize || eh->e_phnum > 128 ||
        (u64)eh->e_phnum * sizeof(Elf64_Phdr) > imgsize - eh->e_phoff)
        return 0;

    const Elf64_Phdr *ph = (const Elf64_Phdr *)(img + eh->e_phoff);
    u64 pages = 0;
    for (u32 i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        u64 va = ph[i].p_vaddr, msz = ph[i].p_memsz;
        if (va < USER_BASE || va >= USER_MAX_VMA) continue;
        if (va + msz <= va) continue; /* wraps: elf_load skips it too */
        u64 lo = ALIGN_DOWN(va, PAGE_SIZE);
        u64 end = ALIGN_UP(va + msz, PAGE_SIZE);
        if (end <= lo) return ~0ULL; /* ALIGN_UP overflowed: unknown, assume bad */
        pages += (end - lo) / PAGE_SIZE;
        if (pages > PROP_MAX_ELF_PAGES) return pages;
    }
    return pages;
}

enum elf_field {
    F_IDENT_BYTE,
    F_TYPE,
    F_MACHINE,
    F_ENTRY,
    F_PHOFF,
    F_PHNUM,
    F_PHENTSIZE,
    F_P_TYPE,
    F_P_FLAGS,
    F_P_OFFSET,
    F_P_VADDR,
    F_P_FILESZ,
    F_P_MEMSZ,
    F_P_ALIGN,
    F_FIELD_MAX
};

static void mutate_elf(u8 *img, size_t imgsize)
{
    static const u64 values[] = {
        0,
        1,
        2,
        3,
        6,
        62,
        0x7F,
        0x200,
        0x400,
        ELF_IMG,
        ELF_IMG + 1,
        PAGE_SIZE,
        0x10000,
        0xFFFFFFFFULL,
        ~0ULL,
        USER_BASE,
        USER_BASE + 0x1000,
        USER_MAX_VMA,
        USER_MAX_VMA + PAGE_SIZE,
        USER_BASE - 1,
    };
    Elf64_Ehdr *eh = (Elf64_Ehdr *)img;

    u32 n = 1 + prop_below(3); /* one to three mutations per image */
    for (u32 m = 0; m < n; m++) {
        u64 v = prop_below(3) ? values[prop_below((u32)(sizeof(values) / sizeof(values[0])))]
                              : prop_rand(); /* plus a plain random word */
        enum elf_field f = (enum elf_field)prop_below((u32)F_FIELD_MAX);

        switch (f) {
        case F_IDENT_BYTE:
            img[prop_below(16)] = (u8)v;
            break;
        case F_TYPE:
            eh->e_type = (u16)v;
            break;
        case F_MACHINE:
            eh->e_machine = (u16)v;
            break;
        case F_PHNUM:
            eh->e_phnum = (u16)v;
            break;
        case F_PHENTSIZE:
            eh->e_phentsize = (u16)v;
            break;
        case F_ENTRY:
            eh->e_entry = v;
            break;
        case F_PHOFF:
            eh->e_phoff = v;
            break;
        default:
            break; /* the segment fields are handled below */
        }

        if (f >= F_P_TYPE) {
            /* aim at a program header elf_load will actually read: inside the
             * image, and inside whatever e_phoff now says */
            if (eh->e_phoff <= imgsize && eh->e_phoff + sizeof(Elf64_Phdr) <= imgsize) {
                u32 room = (u32)((imgsize - eh->e_phoff) / sizeof(Elf64_Phdr));
                u32 idx = 0;
                if (eh->e_phnum) {
                    idx = prop_below(eh->e_phnum < room ? eh->e_phnum : room);
                    if (idx >= room) idx = 0;
                }
                Elf64_Phdr *ph = (Elf64_Phdr *)(img + eh->e_phoff + (u64)idx * sizeof(Elf64_Phdr));
                switch (f) {
                case F_P_TYPE:
                    ph->p_type = (u32)v;
                    break;
                case F_P_FLAGS:
                    ph->p_flags = (u32)v;
                    break;
                case F_P_OFFSET:
                    ph->p_offset = v;
                    break;
                case F_P_VADDR:
                    ph->p_vaddr = v;
                    break;
                case F_P_FILESZ:
                    ph->p_filesz = v;
                    break;
                case F_P_MEMSZ:
                    ph->p_memsz = v;
                    break;
                case F_P_ALIGN:
                    ph->p_align = v;
                    break;
                default:
                    break;
                }
            }
        }
    }
}

static void test_prop_elf(void)
{
    prop_begin("elf", 5, ITER_ELF);

    static const u32 sizes[] = {0, 8, 16, 31, 64, 65, 128, 512, 1023, 1024};

    for (; prop_iter < prop_iters; prop_iter++) {
        struct {
            u8 pre[32];
            u8 img[ELF_IMG];
            u8 post[32];
        } __attribute__((aligned(16))) g;

        memset(g.pre, 0xEE, sizeof(g.pre));
        memset(g.post, 0xEE, sizeof(g.post));
        build_good_elf(g.img);
        mutate_elf(g.img, ELF_IMG);

        size_t imgsize = sizes[prop_below((u32)(sizeof(sizes) / sizeof(sizes[0])))];
        bool forced = elf_map_extent(g.img, imgsize) > PROP_MAX_ELF_PAGES;
        if (forced) {
            /* header-level refusal: e_phoff past the end of the image, which
             * elf_load checks before it reads a single program header */
            ((Elf64_Ehdr *)g.img)->e_phoff = ELF_IMG + 1;
        }

        u64 sentinel = 0xDEADBEEFULL;
        u64 brk = sentinel;
        u64 before = pmm_free_bytes();
        u64 root = vmm_new_user_aspace();
        if (!root) {
            prop_check(0, "vmm_new_user_aspace", 0, 1);
            break;
        }
        u64 entry = elf_load(root, g.img, imgsize, &brk);
        vmm_destroy_user_aspace(root);

        prop_check(pmm_free_bytes() == before, "elf_load leaked frames", pmm_free_bytes(), before);
        prop_check(canary_damage_ee(g.pre, sizeof(g.pre)) == 0, "wrote before the image", 1, 0);
        prop_check(canary_damage_ee(g.post, sizeof(g.post)) == 0, "wrote past the image", 1, 0);

        if (forced)
            prop_check(entry == 0 && brk == sentinel, "a forced rejection had a side effect",
                       entry, sentinel);
        else {
            prop_check(entry == 0 || (entry >= USER_BASE && entry < USER_MAX_VMA),
                       "entry point outside user space", entry, 0);
            if (entry == 0)
                prop_check(brk == sentinel, "a rejected image wrote *brk_end", brk, sentinel);
            else
                prop_check(brk == 0 || (brk >= USER_BASE && brk <= USER_MAX_VMA),
                           "brk outside the window", brk, 0);
        }
    }

    prop_end();
}

KTEST("prop", KTEST_MM, test_prop_page_table);
KTEST("prop", KTEST_LATE, test_prop_usercopy);
KTEST("prop", KTEST_LATE, test_prop_path);
KTEST("prop", KTEST_LATE, test_prop_fat_name);
KTEST("prop", KTEST_LATE, test_prop_elf);
