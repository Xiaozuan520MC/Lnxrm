/* Kernel heap: power-of-two segregated free lists over a reserved physical
 * region already mapped at KHEAP_VMA. Blocks >= 4 KiB go through the buddy.
 * Every allocation carries an 8-byte header storing the bin index / order.
 *
 * Pure C (rewritten from kheap.cpp 2026-10-17): the allocator is core
 * infrastructure and had no C++-only need -- the one C++ symbol it used to
 * host (operator new) went away with kernel/mm/knew.cpp when ahci.cpp, the
 * last `new` user, was converted to C.  Rewriting also retired the C++
 * linkage hazard noted in mm.h (a file-local prototype silently gaining
 * C++ linkage when the TU was .cpp). */
#include <mm/mm.h>
#include <console.h>
#include <sys/spinlock.h>

#define NBINS KHEAP_NBINS /* 16,32,...,4096 */

/* The three constants below must agree or the small-block path silently
 * mis-sizes: NBINS decides how many free lists exist, PAGE_SIZE decides how
 * many chunks a refill page carves, and 4096 is the small/large cutoff in
 * kmalloc().  They used to be pinned only by a ktest sweep of bin_of();
 * these asserts make a drift a build failure instead of heap corruption. */
LNXRM_STATIC_ASSERT(PAGE_SIZE == 4096, "kheap refill math assumes 4 KiB pages");
LNXRM_STATIC_ASSERT(1UL << (KHEAP_NBINS - 1 + 4) == 4096,
                    "last bin must cover exactly 4096 bytes");
LNXRM_STATIC_ASSERT(PAGE_SIZE >> (KHEAP_NBINS - 1 + 4) >= 1,
                    "refill must carve >= 1 chunk per page in every bin");

static u64 heap_start, heap_end; /* virtual (== KHEAP_VMA window) */
static u64 heap_used;
struct hdr {
    u32 bin_or_order;
    u32 magic;
#define HEAP_MAGIC 0xECEC0001
};
static void *freelist[NBINS];
static spinlock_t kheap_lock = SPINLOCK_INIT;

void kheap_init(void)
{
    u64 phys, size;
    vmm_heap_region(&phys, &size);
    heap_start = KHEAP_VMA;
    heap_end = KHEAP_VMA + size;
    kprintf("[kheap] %lu MiB window at 0x%lx (phys 0x%lx)\n", size >> 20, heap_start, phys);
}

static inline int bin_of(size_t n)
{
    int b = 4; /* smallest bin covers <=16 */
    /* b < 63: for a huge n the shift would reach bit 64 and beyond, which is
     * undefined behaviour.  Clamped, an oversized n returns a bin index far
     * past KHEAP_NBINS -- the documented "no bin" sentinel -- instead of
     * stepping off the end of the shift. */
    while (b < 63 && (1UL << b) < n) b++;
    return b - 4;
}

/* published for the boot self-test: bin_of() is the one place where an
 * off-by-one turns into a silently mis-sized free list. */
int kheap_bin_of(size_t n)
{
    return bin_of(n);
}

u64 kheap_used_bytes(void)
{
    return heap_used;
}

void *kmalloc(size_t n)
{
    if (!n) n = 1;
    /* Guard the buddy path against a size_t wrap.  For a huge n, n + hdr sits
     * near 2^64 and ALIGN_UP() in the buddy branch below overflows down to a
     * tiny `need`, so a small block would be handed out as though it were
     * enormous -- a caller-driven heap overflow.  Nothing larger than the
     * buddy's biggest block (4 MiB) is satisfiable anyway, so reject it here
     * as a plain allocation failure. */
    if (n > (4UL << 20) - sizeof(struct hdr)) return NULL;
    struct hdr *h;

    u64 flags;
    spin_lock_irqsave(&kheap_lock, &flags);

    if (n + sizeof(struct hdr) > 4096) {
        int order = 0;
        size_t need = ALIGN_UP(n + sizeof(struct hdr), PAGE_SIZE);
        while ((PAGE_SIZE << order) < need) order++;
        u64 pa = pmm_alloc_order(order);
        if (!pa) {
            /* T-004: an out-of-memory allocation is an error return, never
             * a panic -- otherwise a user program that loops brk()/fork()
             * until the buddy is dry takes the kernel down with it. */
            spin_unlock_irqrestore(&kheap_lock, flags);
            return NULL;
        }
        h = (struct hdr *)(PHYS_TO_VIRT(pa));
        h->bin_or_order = 0x80 | order;
        h->magic = HEAP_MAGIC;
        /* account the whole buddy block, exactly what kfree() subtracts */
        heap_used += (u64)PAGE_SIZE << order;
        spin_unlock_irqrestore(&kheap_lock, flags);
        return (u8 *)h + sizeof(struct hdr);
    }

    int b = bin_of(n + sizeof(struct hdr));
    if (!freelist[b]) {
        /* carve a fresh block from the buddy: one page per refill */
        u64 pa = pmm_alloc();
        if (!pa) {
            spin_unlock_irqrestore(&kheap_lock, flags);
            return NULL;
        }
        u8 *base = (u8 *)PHYS_TO_VIRT(pa);
        size_t sz = PAGE_SIZE >> (b + 4); /* chunks per page */
        for (size_t i = 0; i < sz; i++) {
            h = (struct hdr *)(base + i * (1UL << (b + 4)));
            h->bin_or_order = b;
            h->magic = HEAP_MAGIC;
            *(void **)((u8 *)h + sizeof(struct hdr)) = freelist[b];
            freelist[b] = (u8 *)h + sizeof(struct hdr);
        }
    }
    /* Defensive: the static asserts above prove sz >= 1 for every in-range
     * bin, so a successful refill leaves the list non-empty.  A NULL here
     * would be a silent constant drift; fail loudly as OOM instead. */
    if (!freelist[b]) {
        spin_unlock_irqrestore(&kheap_lock, flags);
        return NULL;
    }
    void *p = freelist[b];
    freelist[b] = *(void **)p;
    h = (struct hdr *)((u8 *)p - sizeof(struct hdr));
    /* T-040: re-arm the magic.  kfree() poisons it on the way onto the
     * freelist (so a double free is caught), so a recycled block must have
     * it restored before it is handed out again. */
    h->magic = HEAP_MAGIC;
    heap_used += 1UL << (b + 4);

    spin_unlock_irqrestore(&kheap_lock, flags);
    return p;
}

/* Bring-up paths where a NULL return would just be dereferenced a line
 * later by code with nowhere to go.  Runtime paths -- anything a user
 * program can reach -- must use kmalloc() and handle the NULL instead of
 * reaching for this.  No caller today: its archetypal one was the C++
 * global operator new (knew.cpp), deleted 2026-10-17 when ahci.cpp -- the
 * kernel's last `new` user -- became ahci.c.  Kept as the bring-up entry
 * point C12/T-004 explicitly retained, not removed as dead code. */
void *kmalloc_or_panic(size_t n)
{
    void *p = kmalloc(n);
    if (!p) panic("kmalloc: out of memory (%lu bytes)", n);
    return p;
}

void kfree(void *p)
{
    if (!p) return;
    struct hdr *h = (struct hdr *)((u8 *)p - sizeof(struct hdr));

    u64 flags;
    spin_lock_irqsave(&kheap_lock, &flags);

    /* Magic is validated INSIDE the lock (TOCTOU fix, 2026-10-17).  Checked
     * outside, two CPUs kfree()ing the same pointer could both read the
     * live magic before either poisoned it, and the block would be pushed
     * onto a freelist twice -- a duplicate-listed chunk that two later
     * allocations then alias.  Under the lock the second free sees the
     * poison written by the first. */
    if (h->magic != HEAP_MAGIC) {
        /* T-040: kfree() poisons magic to 0 on the way onto a freelist, so a
         * block that is freed, then freed again, reads 0 here: that is a
         * double free.  Any other value is a wild pointer or a clobbered
         * header.  Either way the heap cannot be trusted -- stop here, where
         * the fault is legible, rather than corrupting a freelist and dying
         * later in an unrelated allocation.  (Panic does not return and does
         * not need this lock, so holding it across the panic is harmless.) */
        if (h->magic == 0) panic("kfree: double free %p", p);
        panic("kfree: bad magic %p (wild pointer or corrupt header)", p);
    }

    if (h->bin_or_order & 0x80) {
        int order = h->bin_or_order & 0x7f;
        heap_used -= PAGE_SIZE << order;
        pmm_free_order(VIRT_TO_PHYS((uptr)h), order);
        spin_unlock_irqrestore(&kheap_lock, flags);
        return;
    }
    /* four.2: before linking onto a freelist, require a sane bin and correct
     * chunk alignment.  A wild or interior pointer whose header happens to
     * read back HEAP_MAGIC would otherwise seed a free list with a bogus
     * address and surface much later as corruption in an unrelated alloc. */
    {
        int b = h->bin_or_order;
        if (b >= NBINS) panic("kfree: bad block %p (bin %d out of range)", p, b);
        size_t chunk = 1UL << (b + 4);
        if ((uptr)h & (chunk - 1)) panic("kfree: bad block %p (bin %d misaligned)", p, b);
    }
    *(void **)p = freelist[h->bin_or_order];
    freelist[h->bin_or_order] = p;
    heap_used -= 1UL << (h->bin_or_order + 4);
    /* T-040: poison the magic so a second kfree() of this block is caught
     * above as a double free.  The freelist link lives at p (8 bytes past
     * the header), so this does not disturb it; kmalloc() re-arms the magic
     * when the block is handed out again. */
    h->magic = 0;

    spin_unlock_irqrestore(&kheap_lock, flags);
}
