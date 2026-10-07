/* Physical frame buddy allocator (MAX_ORDER 10 => 4 MiB blocks).
 * Free lists are intrusive: the first 8 bytes of each free block hold the
 * next-free pointer. Per-order bitmaps record which blocks are free so the
 * buddy of a block being freed can be found and coalesced. */
#include <mm/mm.h>
#include <console.h>
#include <boot.h>
#include <sys/spinlock.h>

#define MAX_ORDER      11
#define MIN_ORDER_BITS 12

struct freelist {
    u64 head;
    u64 count;
};

static struct freelist fl[MAX_ORDER];
static u8 *order_bits[MAX_ORDER];     /* bitmaps, allocated from low scratch */
static u64 order_bit_size[MAX_ORDER]; /* actual bytes per order bitmap */

/* safe zone: 0x10000 (64 KiB) -- above trampoline/GDT/E820, below EBDA */
#define BITS_BASE 0x10000UL

/* PMM_WINDOW_TOP (include/mm/mm.h) caps the managed window at the size of
 * the identity map / high-half alias.  See the comment there: the old 4 GiB
 * clamp sent the seeding loop straight into unmapped VA 0x40000000 on any
 * real machine with >= ~1.1 GiB of RAM, and with no IDT yet installed that
 * was a triple fault, not a panic. */

static u64 base_addr, top_addr;      /* managed window */
static u64 reserved_lo, reserved_hi; /* [lo,hi) excluded (kernel image etc.) */
static u64 total_pg, free_pg;
static spinlock_t pmm_lock = SPINLOCK_INIT;

/* MMIO ranges (the LFB) that must never be handed out as frames.
 * 8, not 4: beyond the LFB and its PD window, start_kernel() has to reserve
 * the two frame ranges whose high-half alias slots are consumed by a fixed
 * window (VGA slot 96, devices slot 128, kheap slots 256..271). */
#define RESV_MAX 8
static struct {
    u64 lo, hi;
} resv[RESV_MAX];
static int resv_n;

void pmm_reserve(u64 lo, u64 hi)
{
    if (lo >= hi || resv_n >= RESV_MAX) return;
    resv[resv_n].lo = lo;
    resv[resv_n].hi = hi;
    resv_n++;
}

/* does [pa, pa+size) touch any reserved range? */
static int resv_hits(u64 pa, u64 size)
{
    for (int i = 0; i < resv_n; i++)
        if (pa < resv[i].hi && pa + size > resv[i].lo) return 1;
    return 0;
}

static inline u64 bits_bytes(int order)
{
    u64 blocks = (top_addr - base_addr) >> (MIN_ORDER_BITS + order);
    return (blocks + 7) / 8 + 1;
}

static inline int bit_get(int order, u64 pa)
{
    u64 idx = (pa - base_addr) >> (MIN_ORDER_BITS + order);
    return !!(order_bits[order][idx >> 3] & (1 << (idx & 7)));
}

static inline void bit_set(int order, u64 pa)
{
    u64 idx = (pa - base_addr) >> (MIN_ORDER_BITS + order);
    order_bits[order][idx >> 3] |= 1 << (idx & 7);
}

static inline void bit_clr(int order, u64 pa)
{
    u64 idx = (pa - base_addr) >> (MIN_ORDER_BITS + order);
    order_bits[order][idx >> 3] &= ~(1 << (idx & 7));
}

static inline u64 buddy_of(u64 pa, int order)
{
    u64 sz = 1UL << (MIN_ORDER_BITS + order);
    return pa ^ sz;
}

void pmm_init(void)
{

    /* find the largest usable E820 region above 1 MiB */
    u64 best = 0, best_end = 0;
    for (int i = 0; i < bootinfo.map_len; i++) {
        struct e820_entry *e = &bootinfo.map[i];
        if (e->type != E820_RAM) continue;
        u64 lo = MAX(e->addr, 0x100000UL);
        u64 hi = e->addr + e->size;
        if (lo >= hi || lo > 0x100000000UL) /* stay under 4 GiB for DMA ease */
            continue;
        hi = MIN(hi, 0x100000000UL);
        hi = MIN(hi, PMM_WINDOW_TOP); /* ... and inside the mapped window */
        if (lo >= hi) continue;       /* region lives entirely above 1 GiB */
        if (hi - lo > best_end - best) {
            best = lo;
            best_end = hi;
        }
    }
    /* reserve everything up to the END of .bss (which contains the boot
     * stack): __bss_end is a VMA symbol */
    extern u8 __bss_end[];
    u64 kern_end = ALIGN_UP(VIRT_TO_PHYS((uptr)__bss_end), PAGE_SIZE);

    /* Not enough low RAM for the kernel image + the 16 MiB heap: say so and
     * stop.  Falling through used to underflow kheap_place()'s subtraction,
     * wrap top_addr to ~2^64, and then memset() the bitmap below with a
     * wrapped-around length -- wiping the live page tables at 0x50000 and
     * triple-faulting one line after the e820 dump. */
    if (best_end < kern_end + (18UL << 20))
        panic("pmm: no usable e820 RAM for kernel+heap (map_len=%d best_end=%#lx)", bootinfo.map_len,
              best_end);

    /* carve the kernel heap out of the top before the buddy sees it */
    extern void kheap_place(u64 * region_top, u64 size);
    u64 heap_top = ALIGN_DOWN(best_end, 0x200000UL);
    kheap_place(&heap_top, 16 << 20); /* reserves downward, returns new top */

    reserved_lo = best;
    reserved_hi = kern_end;
    /* 2 MiB safety margin past the kernel image -- guards against any
     * off-by-frame in early allocations touching image-adjacent RAM */
    base_addr = ALIGN_UP(reserved_hi, PAGE_SIZE) + 0x200000UL;
    top_addr = heap_top;

    /* dynamically size bitmaps: place sequentially starting at BITS_BASE */
    u64 bit_total = 0;
    for (int o = 0; o < MAX_ORDER; o++) {
        order_bit_size[o] = bits_bytes(o);
        order_bits[o] = (u8 *)BITS_BASE + bit_total;
        bit_total += order_bit_size[o];
    }
    memset((u8 *)BITS_BASE, 0, bit_total);

    total_pg = (top_addr - base_addr) >> MIN_ORDER_BITS;
    free_pg = 0;

    /* seed the free lists with maximal aligned power-of-two blocks */
    u64 cur = base_addr;
    while (cur < top_addr) {
        int o = 0;
        while (o + 1 < MAX_ORDER && ((cur & ((2UL << (MIN_ORDER_BITS + o)) - 1)) == 0) &&
               cur + (2UL << (MIN_ORDER_BITS + o)) <= top_addr)
            o++;
        /* shrink a block until it clears every reserved MMIO range */
        while (o > 0 && resv_hits(cur, 1UL << (MIN_ORDER_BITS + o))) o--;
        if (resv_hits(cur, 1UL << MIN_ORDER_BITS)) {
            if (top_addr - cur >= 1UL << MIN_ORDER_BITS) total_pg--;
            cur += 1UL << MIN_ORDER_BITS;
            continue;
        }
        pmm_free_order(cur, o);
        cur += 1UL << (MIN_ORDER_BITS + o);
    }
    kprintf("[pmm] frames %#lx..#%lx (%lu KiB)\n", base_addr, top_addr,
            (top_addr - base_addr) >> 10);
}

/* T-004 fault injection, used only by the boot self-test: while armed, the
 * next `n` frame allocations fail exactly as an empty buddy would, so every
 * OOM branch (kmalloc's two, the page-table refill, the aspace root) can be
 * driven to its error return on a machine that has plenty of memory.
 * Exhausting RAM for real would take the whole boot down with it. */
static int oom_inject;

void pmm_inject_oom(int n)
{
    oom_inject = n;
}

u64 pmm_alloc_order(int order)
{
    u64 flags;
    spin_lock_irqsave(&pmm_lock, &flags);

    if (oom_inject > 0) { /* injected failure: behave like a dry buddy */
        oom_inject--;
        goto fail;
    }

    int o = order;
    if (o >= MAX_ORDER) goto fail;
    while (o < MAX_ORDER && fl[o].count == 0) o++;
    if (o >= MAX_ORDER) goto fail;

    u64 addr = fl[o].head;
    u64 next = *(u64 *)(uptr)addr;
    fl[o].head = next;
    fl[o].count--;
    bit_clr(o, addr);

    /* split down to the requested order */
    while (o > order) {
        o--;
        u64 half = addr + (1UL << (MIN_ORDER_BITS + o));
        *(u64 *)(uptr)half = fl[o].head;
        fl[o].head = half;
        fl[o].count++;
        bit_set(o, half);
    }
    free_pg -= 1UL << order;

    spin_unlock_irqrestore(&pmm_lock, flags);
    return addr;

fail:
    spin_unlock_irqrestore(&pmm_lock, flags);
    return 0;
}

void pmm_free_order(u64 pa, int order)
{
    u64 flags;
    spin_lock_irqsave(&pmm_lock, &flags);

    int o = order;
    while (o + 1 < MAX_ORDER) {
        u64 bud = buddy_of(pa, o);
        if (bud < base_addr || bud >= top_addr) break; /* buddy outside managed window */
        if (!bit_get(o, bud)) break;
        /* unlink buddy from list[o] */
        u64 *p = &fl[o].head;
        while (*p != bud) p = (u64 *)(uptr)*p;
        *p = *(u64 *)(uptr)bud;
        fl[o].count--;
        bit_clr(o, bud);
        pa = MIN(pa, bud);
        o++;
    }
    *(u64 *)(uptr)pa = fl[o].head;
    fl[o].head = pa;
    fl[o].count++;
    bit_set(o, pa);
    free_pg += 1UL << order;

    spin_unlock_irqrestore(&pmm_lock, flags);
}


u64 pmm_free_bytes(void)
{ return free_pg << MIN_ORDER_BITS; }
