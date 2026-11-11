/* MAP_SHARED's shared-frame cache -- see include/mm/filepage.h for the
 * contract (who owns a reference, why MAP_PRIVATE stays out of it, and why
 * dropping the file is the right moment to forget its frames).
 *
 * One lock, one hash table, no eviction: entries live exactly as long as
 * the struct file does, and a file only closes when no fd and no VMA holds
 * it any more.  The table therefore never grows beyond the file pages some
 * open file has actually been touched at, which for a 64 MiB volume is a
 * few kilobytes of nodes at worst.
 *
 * The rule this file is built around: never read a disk, and never take a
 * second lock, with fp_lock held.  filepage_get() therefore fills its frame
 * unlocked and only publishes it under the lock -- losing that race costs
 * one throwaway frame, winning it would have cost an interrupt-off window
 * across a whole disk read. */
#include <mm/filepage.h>
#include <mm/mm.h>
#include <sys/vfs.h>
#include <sys/spinlock.h>
#include <console.h>

#define FP_BUCKETS 64 /* power of two: the hash masks instead of dividing */

struct filepage {
    struct file *file;
    u64 off;
    u64 pa;
    struct filepage *next;
};

static struct filepage *fp_tab[FP_BUCKETS];
static spinlock_t fp_lock = SPINLOCK_INIT;

/* Identity + offset, mixed only enough to spread neighbouring offsets of
 * one file across the buckets (they would otherwise all land together as
 * a mapping walked forward through a file). */
static u32 fp_hash(const struct file *f, u64 off)
{
    u64 h = ((u64)(uptr)f >> 4) ^ (off >> 12);
    h ^= h >> 11;
    return (u32)(h & (FP_BUCKETS - 1));
}

/* Caller holds fp_lock. */
static struct filepage *fp_find(const struct file *f, u64 off)
{
    for (struct filepage *e = fp_tab[fp_hash(f, off)]; e; e = e->next)
        if (e->file == f && e->off == off) return e;
    return NULL;
}

u64 filepage_get(struct file *f, u64 off)
{
    if (!f) return 0;

    u64 iflags;
    u64 pa;
    spin_lock_irqsave(&fp_lock, &iflags);
    struct filepage *e = fp_find(f, off);
    pa = e ? e->pa : 0;
    if (e) pmm_ref(pa); /* the caller's mapping reference, taken while the
                         * entry is pinned: dropping it a moment later must
                         * not be able to race the cache's own teardown */
    spin_unlock_irqrestore(&fp_lock, iflags);
    if (pa) return pa;

    /* First touch of this offset.  Allocating, zeroing and reading the file
     * all happen with the lock released -- the read can block on the disk,
     * and this runs from a #PF handler with interrupts off otherwise. */
    u64 npa = pmm_alloc();
    if (!npa) return 0;
    memset((void *)PHYS_TO_VIRT(npa), 0, PAGE_SIZE);
    /* A short read is normal (the last page of a file that is not a whole
     * number of pages): the frame was zeroed first, so the tail reads as
     * zeros exactly as the file's own past-EOF bytes do.  A real I/O error
     * is not a page: hand the frame back and let the fault be refused. */
    long r = vfs_pread(f, (void *)PHYS_TO_VIRT(npa), PAGE_SIZE, off);
    if (r < 0) {
        pmm_free(npa);
        return 0;
    }

    struct filepage *ne = kmalloc(sizeof(*ne));
    if (!ne) {
        pmm_free(npa);
        return 0;
    }

    spin_lock_irqsave(&fp_lock, &iflags);
    e = fp_find(f, off);
    if (e) {
        /* Another address space filled the same offset while we read.  Its
         * frame is the one that stays: adopting ours now would leave two
         * frames for one file offset and the next writer would have to
         * choose between them. */
        pa = e->pa;
        pmm_ref(pa);
        spin_unlock_irqrestore(&fp_lock, iflags);
        kfree(ne);
        pmm_free(npa); /* ours was never published: its seed ref is the only one */
        return pa;
    }
    ne->file = f;
    ne->off = off;
    ne->pa = npa;
    ne->next = fp_tab[fp_hash(f, off)];
    fp_tab[fp_hash(f, off)] = ne;
    pmm_ref(npa); /* npa's seed reference becomes the CACHE's; this one is
                   * the caller's mapping, installed under the same lock so
                   * a concurrent drop_file can never free a frame we are
                   * about to hand out */
    spin_unlock_irqrestore(&fp_lock, iflags);
    return npa;
}

void filepage_drop_file(struct file *f)
{
    if (!f) return;
    u64 iflags;
    spin_lock_irqsave(&fp_lock, &iflags);
    for (u32 b = 0; b < FP_BUCKETS; b++) {
        struct filepage **link = &fp_tab[b];
        while (*link) {
            struct filepage *e = *link;
            if (e->file != f) {
                link = &e->next;
                continue;
            }
            *link = e->next;
            u64 pa = e->pa;
            kfree(e);
            /* Drop the CACHE's reference only.  Any mapping still pointing
             * at this frame -- which the VMA's own file reference says there
             * cannot be, but the count does not have to rely on that -- holds
             * its own and keeps the bytes alive until it unmaps. */
            pmm_free(pa);
        }
    }
    spin_unlock_irqrestore(&fp_lock, iflags);
}

#ifdef CONFIG_KTEST
u64 filepage_resident(void)
{
    u64 iflags, n = 0;
    spin_lock_irqsave(&fp_lock, &iflags);
    for (u32 b = 0; b < FP_BUCKETS; b++)
        for (struct filepage *e = fp_tab[b]; e; e = e->next) n++;
    spin_unlock_irqrestore(&fp_lock, iflags);
    return n;
}
#endif
