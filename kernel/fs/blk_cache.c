/* Block cache: LRU sector cache for improving disk I/O performance.
 * All device traffic (including this file's own) goes through the
 * blk_io_read()/blk_io_write() pair declared in <disk.h> -- the single
 * choke point where a transfer can be validated or, for the boot
 * self-test, made to fail (T-005 fault injection). */
#include <disk.h>
#include <console.h>
#include <mm/mm.h>
#include <sys/spinlock.h>

static struct cache_entry cache[CACHE_ENTRIES];
static spinlock_t cache_lock = SPINLOCK_INIT;

/* ---- recency bookkeeping and counters (T-007) --------------------------
 * `lru_seq` is bumped under cache_lock on every hit, fill and stage; the
 * entry with the smallest stamp is the least recently used one. */
static u64 lru_seq;
static struct blk_cache_stats stats;

/* Relaxed atomics, not the lock: blk_io_* is called both outside the cache
 * (mount, FAT writes) and from cache_writeback() *while cache_lock is held*,
 * so taking the lock here would deadlock on the second kind of caller. */
static void bump(u64 *counter)
{ __sync_fetch_and_add(counter, 1); }

void blk_cache_stats_get(struct blk_cache_stats *out)
{
    if (out) *out = stats;
}

void blk_cache_stats_reset(void)
{ memset(&stats, 0, sizeof(stats)); }

/* ---- T-005 fault injection: the one place a transfer can be made to fail ----
 * The FAT32 layer used to ignore the result of most of its block I/O, and the
 * only way to prove otherwise is to make an I/O actually fail.  Arming this
 * makes blk_io_read()/blk_io_write() answer LNXRM_EIO for one (dev, lba) --
 * the same code a driver returns when the hardware refuses -- without touching
 * the disk.  Test-only: no production path ever arms it. */
static struct {
    struct blkdev *dev;
    u64 lba;
    int ops;
} io_fault;

void blk_inject_io(struct blkdev *dev, u64 lba, int ops)
{
    io_fault.dev = dev;
    io_fault.lba = lba;
    io_fault.ops = ops;
}

void blk_inject_io_off(void)
{
    io_fault.dev = NULL;
    io_fault.ops = 0;
}

static int io_fault_hit(struct blkdev *dev, u64 lba, u32 count, int op)
{
    if (!(io_fault.ops & op) || io_fault.dev != dev) return 0;
    if (!count) return 0;
    /* the armed LBA must fall inside [lba, lba + count) */
    return lba <= io_fault.lba && io_fault.lba - lba < count;
}

int blk_io_read(struct blkdev *dev, u64 lba, u32 count, void *buf)
{
    if (!dev || !dev->read || !buf) return LNXRM_EFAIL;
    if (io_fault_hit(dev, lba, count, BLK_IO_READ)) return LNXRM_EIO;
    bump(&stats.dev_reads); /* only transfers that really reach a driver */
    return dev->read(dev, lba, count, buf);
}

int blk_io_write(struct blkdev *dev, u64 lba, u32 count, const void *buf)
{
    if (!dev || !dev->write || !buf) return LNXRM_EFAIL;
    if (io_fault_hit(dev, lba, count, BLK_IO_WRITE)) return LNXRM_EIO;
    bump(&stats.dev_writes);
    return dev->write(dev, lba, count, buf);
}

void blk_cache_init(void)
{
    for (int i = 0; i < CACHE_ENTRIES; i++) {
        cache[i].valid = false;
        cache[i].dirty = false;
        cache[i].dev = NULL;
        cache[i].lba = 0;
        cache[i].stamp = 0;
    }
    lru_seq = 0;
    kprintf("[blk_cache] initialized %d entries\n", CACHE_ENTRIES);
}

/* Find a cache entry for a given device and LBA.
 * Returns the index if found, -1 otherwise.  A hit makes the entry the most
 * recently used one -- recency is what the victim choice below is based on. */
static int cache_find(struct blkdev *dev, u64 lba)
{
    for (int i = 0; i < CACHE_ENTRIES; i++) {
        if (cache[i].valid && cache[i].dev == dev && cache[i].lba == lba) {
            cache[i].stamp = ++lru_seq;
            return i;
        }
    }
    return LNXRM_EFAIL;
}

/* Pick the slot a new sector may occupy.
 * An empty slot is always the right answer -- nothing is lost by taking it.
 * Otherwise the victim is the *least recently used* resident sector: the one
 * with the smallest stamp.  The old code keyed eviction off a single `used`
 * bit that a hit set and nothing ever cleared per entry, so once all 64 slots
 * were "used" it cleared every bit and took slot 0 regardless of who had just
 * been touched: a working set larger than the cache hammered the same slots
 * forever (C24). */
static int cache_find_victim(void)
{
    for (int i = 0; i < CACHE_ENTRIES; i++)
        if (!cache[i].valid) return i;

    int victim = 0;
    for (int i = 1; i < CACHE_ENTRIES; i++)
        if (cache[i].stamp < cache[victim].stamp) victim = i;
    return victim;
}

/* Account for taking `idx` away from whatever it held.  Called before the
 * slot is reused: eviction is a real event (a resident sector is being
 * dropped), and a dirty one costs a writeback. */
static void cache_note_eviction(int idx)
{
    if (cache[idx].valid) bump(&stats.evictions);
}

/* Writeback a dirty cache entry */
static int cache_writeback(int idx)
{
    if (!cache[idx].valid || !cache[idx].dirty) return 0;

    struct blkdev *dev = cache[idx].dev;
    u64 lba = cache[idx].lba;
    bump(&stats.writebacks);
    int ret = blk_io_write(dev, lba, 1, cache[idx].data);
    if (ret == 0) cache[idx].dirty = false;
    return ret;
}

/* all helpers below assume cache_lock is held */
static int cache_flush_locked(struct blkdev *dev)
{
    int ret = 0;
    for (int i = 0; i < CACHE_ENTRIES; i++) {
        if (cache[i].valid && cache[i].dirty && cache[i].dev == dev) {
            int r = cache_writeback(i);
            if (r != 0) ret = r;
        }
    }
    return ret;
}

int blk_cache_read(struct blkdev *dev, u64 lba, void *buf)
{
    /* Validate buffer pointer */
    if (!buf) return LNXRM_EFAIL;
    /* Checked before the lookup: a fault that only fired on a device access
     * would be invisible whenever the sector happens to be cached, and the
     * filesystem must see the same error either way. */
    if (io_fault_hit(dev, lba, 1, BLK_IO_READ)) return LNXRM_EIO;

    u64 flags;
    spin_lock_irqsave(&cache_lock, &flags);

    int idx = cache_find(dev, lba);

    if (idx >= 0) {
        /* Cache hit: cache_find() has already stamped it as MRU */
        bump(&stats.hits);
        memcpy(buf, cache[idx].data, CACHE_SECTOR_SIZE);
        spin_unlock_irqrestore(&cache_lock, flags);
        return 0;
    }

    /* Cache miss: need to read from disk */
    bump(&stats.misses);
    idx = cache_find_victim();
    cache_note_eviction(idx);

    /* Writeback if victim is dirty */
    if (cache[idx].valid && cache[idx].dirty) {
        int ret = cache_writeback(idx);
        if (ret != 0) {
            spin_unlock_irqrestore(&cache_lock, flags);
            return ret;
        }
    }

    /* Read from disk into cache entry */
    int ret = blk_io_read(dev, lba, 1, cache[idx].data);
    if (ret != 0) {
        /* The driver may have filled part of the buffer before refusing, and
         * this slot still claims to hold its *previous* LBA (valid is set
         * only below).  Keeping it would hand every later reader of that LBA
         * a half-overwritten sector, so a failed read drops the slot: a lost
         * cached copy costs one more I/O, corrupted data costs a filesystem. */
        cache[idx].valid = false;
        cache[idx].dirty = false;
        cache[idx].stamp = 0;
        spin_unlock_irqrestore(&cache_lock, flags);
        return ret;
    }

    /* Update cache entry */
    cache[idx].lba = lba;
    cache[idx].dev = dev;
    cache[idx].valid = true;
    cache[idx].dirty = false;
    cache[idx].stamp = ++lru_seq; /* freshly filled == most recently used */

    /* Copy to user buffer */
    memcpy(buf, cache[idx].data, CACHE_SECTOR_SIZE);
    spin_unlock_irqrestore(&cache_lock, flags);
    return 0;
}

int blk_cache_write(struct blkdev *dev, u64 lba, const void *buf)
{
    /* Validate buffer pointer */
    if (!buf) return LNXRM_EFAIL;
    /* This path never reaches the device -- it only stages the sector for
     * writeback -- so the fault has to be raised here or a refused write
     * would look exactly like an accepted one until the next flush. */
    if (io_fault_hit(dev, lba, 1, BLK_IO_WRITE)) return LNXRM_EIO;

    u64 flags;
    spin_lock_irqsave(&cache_lock, &flags);

    int idx = cache_find(dev, lba);

    if (idx < 0) {
        /* Cache miss: need to allocate new entry */
        bump(&stats.misses);
        idx = cache_find_victim();
        cache_note_eviction(idx);

        /* Writeback if victim is dirty */
        if (cache[idx].valid && cache[idx].dirty) {
            int ret = cache_writeback(idx);
            if (ret != 0) {
                spin_unlock_irqrestore(&cache_lock, flags);
                return ret;
            }
        }

        /* Initialize new cache entry */
        cache[idx].lba = lba;
        cache[idx].dev = dev;
        cache[idx].valid = true;
        cache[idx].stamp = ++lru_seq;
    } else {
        bump(&stats.hits); /* staging over a resident sector: no device I/O */
    }

    /* Update cache with new data */
    memcpy(cache[idx].data, buf, CACHE_SECTOR_SIZE);
    cache[idx].dirty = true;
    spin_unlock_irqrestore(&cache_lock, flags);
    return 0;
}

int blk_cache_flush(struct blkdev *dev)
{
    u64 flags;
    spin_lock_irqsave(&cache_lock, &flags);
    int ret = cache_flush_locked(dev);
    spin_unlock_irqrestore(&cache_lock, flags);
    return ret;
}

int blk_cache_lookup(struct blkdev *dev, u64 lba)
{
    /* Deliberately does *not* touch the stamp: asking whether a sector is
     * resident must not change which sector gets evicted next. */
    u64 flags;
    int idx = -1;
    spin_lock_irqsave(&cache_lock, &flags);
    for (int i = 0; i < CACHE_ENTRIES; i++) {
        if (cache[i].valid && cache[i].dev == dev && cache[i].lba == lba) {
            idx = i;
            break;
        }
    }
    spin_unlock_irqrestore(&cache_lock, flags);
    return idx;
}

int blk_cache_purge(struct blkdev *dev)
{
    u64 flags;
    int ret = 0;
    spin_lock_irqsave(&cache_lock, &flags);
    for (int i = 0; i < CACHE_ENTRIES; i++) {
        if (!cache[i].valid || (dev && cache[i].dev != dev)) continue;
        if (cache[i].dirty) {
            int r = cache_writeback(i);
            if (r != 0) {
                /* The sector never reached the device: keep it (still dirty)
                 * rather than drop the only copy of what was written. */
                ret = r;
                continue;
            }
        }
        cache[i].valid = false;
        cache[i].dirty = false;
        cache[i].dev = NULL;
        cache[i].stamp = 0;
    }
    spin_unlock_irqrestore(&cache_lock, flags);
    return ret;
}
