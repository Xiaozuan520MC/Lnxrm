/* Kernel heap: power-of-two segregated free lists over a reserved physical
 * region already mapped at KHEAP_VMA. Blocks >= 4 KiB go through the buddy.
 * Every allocation carries an 8-byte header storing the bin index / order. */
#include <mm/mm.h>
#include <console.h>
#include <sys/spinlock.h>

#define NBINS KHEAP_NBINS /* 16,32,...,4096 */

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
    void vmm_heap_region(u64 * phys, u64 * size);
    vmm_heap_region(&phys, &size);
    heap_start = KHEAP_VMA;
    heap_end = KHEAP_VMA + size;
    kprintf("[kheap] %lu MiB window at 0x%lx (phys 0x%lx)\n", size >> 20, heap_start, phys);
}

static inline int bin_of(size_t n)
{
    int b = 4; /* smallest bin covers <=16 */
    while ((1UL << b) < n) b++;
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

extern u64 pmm_alloc_order(int order);
extern void pmm_free_order(u64 pa, int order);

void *kmalloc(size_t n)
{
    if (!n) n = 1;
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
    void *p = freelist[b];
    freelist[b] = *(void **)p;
    h = (struct hdr *)((u8 *)p - sizeof(struct hdr));
    heap_used += 1UL << (b + 4);

    spin_unlock_irqrestore(&kheap_lock, flags);
    return p;
}

/* Bring-up paths where a NULL return would just be dereferenced a line
 * later by code with nowhere to go: C++ `new` has no exceptions to throw
 * in a freestanding kernel, so it is the archetypal caller.  Runtime paths
 * -- anything a user program can reach -- must use kmalloc() and handle
 * the NULL instead of reaching for this. */
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
    if (h->magic != HEAP_MAGIC) panic("kfree: bad magic %p", p);

    u64 flags;
    spin_lock_irqsave(&kheap_lock, &flags);

    if (h->bin_or_order & 0x80) {
        int order = h->bin_or_order & 0x7f;
        heap_used -= PAGE_SIZE << order;
        pmm_free_order(VIRT_TO_PHYS((uptr)h), order);
        spin_unlock_irqrestore(&kheap_lock, flags);
        return;
    }
    *(void **)p = freelist[h->bin_or_order];
    freelist[h->bin_or_order] = p;
    heap_used -= 1UL << (h->bin_or_order + 4);

    spin_unlock_irqrestore(&kheap_lock, flags);
}
