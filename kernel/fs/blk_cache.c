/* Block cache: LRU sector cache for improving disk I/O performance.
 * All device traffic (including this file's own) goes through the
 * blk_io_read()/blk_io_write() pair declared in <disk.h> -- the single choke
 * point where a transfer can be validated or, for the boot self-test, made to
 * fail (T-005 fault injection).  Since C10 it is also where a transfer is
 * QUEUED: each request rides the owning device's FIFO under that device's
 * io_lock, and the drivers keep no lock of their own.
 *
 * ---- C7: cache_lock is a METADATA lock, and no transfer runs under it ----
 * It used to be the middle layer of three irqsave locks all held across the
 * same device transfer (fat_fs_lock -> cache_lock -> dev->io_lock), so one
 * slow disk froze the whole machine rather than one device.  The rule now:
 *
 *   hold cache_lock for the array (valid/dirty/dev/lba/stamp/ownership),
 *   RESERVE the slot that is about to move, drop the lock, let the device
 *   run, re-take the lock to publish the result.
 *
 * Reservation is the whole mechanism, in two shapes:
 *
 *   fill    valid is cleared (no reader can find it) and busy is set, so the
 *           driver may write data[] directly while nobody else can see,
 *           evict or reuse the slot;
 *   write   dirty is cleared, busy is set, and the bytes are copied to a
 *           caller-owned bounce buffer first -- readers may still hit the
 *           slot (their copy is still correct) and a writer staging over it
 *           only re-dirties it, which the finish step leaves alone.
 *
 * Either way the slot cannot be chosen as a victim or purged while busy, so
 * the two facts that used to need the lock held through the I/O -- "this
 * data[] belongs to me" and "nobody changes it under the driver" -- are
 * carried by the entry itself.  dev->io_lock still covers the transfer (per
 * device, by design); what is gone is the machine-wide layer. */
#include <disk.h>
#include <console.h>
#include <mm/mm.h>
#include <sys/spinlock.h>
#include <sys/cpu.h>   /* jiffies, HZ (tick door) */
#include <sys/sched.h> /* current                 */

static struct cache_entry cache[CACHE_ENTRIES];
static spinlock_t cache_lock = SPINLOCK_INIT;

/* ---- recency bookkeeping and counters (T-007) --------------------------
 * `lru_seq` is bumped under cache_lock on every hit, fill and stage; the
 * entry with the smallest stamp is the least recently used one. */
static u64 lru_seq;
static struct blk_cache_stats stats;

/* Atomics, not the lock: every counter moves from both sides of cache_lock
 * -- hits/misses under it, writebacks with it deliberately released (C7) --
 * and blk_io_* has callers outside the cache entirely (mount, FAT writes).
 * Taking the lock here would self-deadlock on the first kind, so the
 * counter never asks. */
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

/* ---- C10: per-device request queue ------------------------------------
 * Every transfer is a node on the owning device's queue, and the queue
 * moves under that device's io_lock in two phases:
 *
 *   phase 1  take io_lock, push the node (it lives on the caller's stack),
 *            drop the lock;
 *   phase 2  take io_lock again and blk_drain_locked() the WHOLE queue in
 *            arrival order -- including requests other CPUs parked in
 *            between -- then drop it.
 *
 * Holding the lock across the drain (not just the pop) is what makes the
 * answer local: once the queue is empty under that lock, every node ever
 * pushed has been executed and stamped BLK_REQ_DONE, so the producer reads
 * its own `status` straight off its stack node -- no spin, no completion
 * flag, no second lock.  Stack nodes are safe here precisely because the
 * drain cannot outlive any producer: a producer between the phases is
 * either still in blk_io_* (node alive) or the machine is in panic() (the
 * dangling node is never walked again).
 *
 * io_lock is the ONLY lock around a transfer now -- the drivers' own
 * ide_lock/ahci_lock are gone with C10 (2026-11-13).  Merge/cancel still
 * are not done: a merged request waits on somebody else's completion, and
 * no path here can sleep -- FS callers hold fat_fs_lock the whole way
 * down, and everything runs at IF=0 for the whole syscall anyway (C7 moved
 * cache_lock out of the transfer, not the caller's own context). */
static void blk_enqueue_locked(struct blkdev *dev, struct blk_req *r)
{
    r->next = NULL;
    if (dev->req_tail) dev->req_tail->next = r;
    else dev->req_head = r;
    dev->req_tail = r;
}

/* Run every request on `dev`'s queue, FIFO.  Caller holds io_lock; when it
 * returns, the queue is empty and each node it carried is BLK_REQ_DONE
 * with its own status -- an error on one request is that request's alone
 * and never poisons the rest of the queue. */
static void blk_drain_locked(struct blkdev *dev)
{
    while (dev->req_head) {
        struct blk_req *r = dev->req_head;
        dev->req_head = r->next;
        if (!dev->req_head) dev->req_tail = NULL;
        /* Both ops were validated when the node entered (blk_io_* phase 1
         * refuses a device without the matching op; the queue only ever
         * holds nodes that came through one of the two entrances). */
        int rc = r->is_write ? dev->write(dev, r->lba, r->count, r->buf)
                             : dev->read(dev, r->lba, r->count, r->buf);
        r->status = rc;
        r->state = BLK_REQ_DONE;
    }
}

int blk_io_read(struct blkdev *dev, u64 lba, u32 count, void *buf)
{
    if (!dev || !dev->read || !buf) return LNXRM_EFAIL;
    if (io_fault_hit(dev, lba, count, BLK_IO_READ)) return LNXRM_EIO;
    bump(&stats.dev_reads); /* only transfers that really reach a driver */

    struct blk_req r = {.lba = lba,
                        .count = count,
                        .buf = buf,
                        .is_write = false,
                        .state = BLK_REQ_QUEUED,
                        .status = 0};
    u64 flags;
    spin_lock_irqsave(&dev->io_lock, &flags); /* phase 1: park on the queue */
    blk_enqueue_locked(dev, &r);
    spin_unlock_irqrestore(&dev->io_lock, flags);

    spin_lock_irqsave(&dev->io_lock, &flags); /* phase 2: drain it in order */
    blk_drain_locked(dev);
    spin_unlock_irqrestore(&dev->io_lock, flags);

    return r.status; /* queue empty under the lock => r.state == DONE */
}

int blk_io_write(struct blkdev *dev, u64 lba, u32 count, const void *buf)
{
    if (!dev || !dev->write || !buf) return LNXRM_EFAIL;
    if (io_fault_hit(dev, lba, count, BLK_IO_WRITE)) return LNXRM_EIO;
    bump(&stats.dev_writes);

    struct blk_req r = {.lba = lba,
                        .count = count,
                        .buf = (void *)buf, /* const dropped here, as in
                                              * ide_blk_xfer(): no driver
                                              * ever writes through it */
                        .is_write = true,
                        .state = BLK_REQ_QUEUED,
                        .status = 0};
    u64 flags;
    spin_lock_irqsave(&dev->io_lock, &flags);
    blk_enqueue_locked(dev, &r);
    spin_unlock_irqrestore(&dev->io_lock, flags);

    spin_lock_irqsave(&dev->io_lock, &flags);
    blk_drain_locked(dev);
    spin_unlock_irqrestore(&dev->io_lock, flags);

    return r.status;
}

#ifdef CONFIG_KTEST
/* C10 test support (ktest/t_blk.c): park a request on `dev`'s queue
 * WITHOUT executing it, so the next blk_io_* on that device drains the
 * queue and runs this node first -- arrival order is then observable
 * (a read issued after a parked write must see what that write wrote).
 * One file-static node: pushing while it is still queued would splice the
 * same node in twice, so that case is refused with NULL instead. */
static struct blk_req ktest_req;

struct blk_req *blk_queue_push_test(struct blkdev *dev, bool is_write, u64 lba, u32 count,
                                    void *buf)
{
    if (!dev || !buf) return NULL;
    if (is_write ? !dev->write : !dev->read) return NULL;
    for (struct blk_req *q = dev->req_head; q; q = q->next)
        if (q == &ktest_req) return NULL; /* still parked: not reusable yet */

    ktest_req.lba = lba;
    ktest_req.count = count;
    ktest_req.buf = buf;
    ktest_req.is_write = is_write;
    ktest_req.state = BLK_REQ_QUEUED;
    ktest_req.status = 0;
    u64 flags;
    spin_lock_irqsave(&dev->io_lock, &flags);
    blk_enqueue_locked(dev, &ktest_req);
    spin_unlock_irqrestore(&dev->io_lock, flags);
    return &ktest_req;
}
#endif

void blk_cache_init(void)
{
    for (int i = 0; i < CACHE_ENTRIES; i++) {
        cache[i].valid = false;
        cache[i].dirty = false;
        cache[i].busy = false;
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
 * forever (C24).
 * C7: a busy slot is never a candidate -- it is the one an in-flight
 * transfer owns (see struct cache_entry), and handing it out would put a
 * second LBA's bytes under a driver still writing the first one's.  -1 only
 * if every slot is spoken for at once, which one transfer per CPU cannot
 * reach, but the callers check it rather than assume it. */
static int cache_find_victim(void)
{
    for (int i = 0; i < CACHE_ENTRIES; i++)
        if (!cache[i].valid && !cache[i].busy) return i;

    int victim = -1;
    for (int i = 0; i < CACHE_ENTRIES; i++) {
        if (cache[i].busy) continue;
        if (victim < 0 || cache[i].stamp < cache[victim].stamp) victim = i;
    }
    return victim;
}

/* Account for taking `idx` away from whatever it held.  Called before the
 * slot is reused: eviction is a real event (a resident sector is being
 * dropped), and a dirty one costs a writeback. */
static void cache_note_eviction(int idx)
{
    if (cache[idx].valid) bump(&stats.evictions);
}

#ifdef CONFIG_KTEST
/* C7 observability: fired at every "the device runs now and cache_lock is
 * NOT held" point below, so a case can assert blk_cache_lock_is_free()
 * there instead of merely believing the comment.  Installed by one test,
 * NULL everywhere else -- and NULL in any non-ktest build. */
static void (*io_probe)(void);
void blk_cache_set_io_probe(void (*fn)(void)) { io_probe = fn; }
bool blk_cache_lock_is_free(void) { return cache_lock.locked == 0; }
static inline void ktest_io_probe(void) { if (io_probe) io_probe(); }
#else
static inline void ktest_io_probe(void) {}
#endif

/* ---- the writeback job: one dirty sector moved with the lock released ----
 * wb_take_* runs under cache_lock and closes the slot's participation: the
 * bytes are copied to the job's own bounce buffer, dirty is cleared and the
 * slot is marked busy.  The caller then drops the lock, runs the transfer
 * and calls wb_finish with the lock back, at which point the slot is free
 * to move again.  Nothing can be lost in between: a writer staging over
 * the slot while we are in flight only sets dirty again, and the finish
 * step leaves it that way -- our bounce carries the OLD bytes, so the new
 * ones still owe a writeback of their own. */
struct wb_job {
    int idx;
    struct blkdev *dev;
    u64 lba;
    u8 bounce[CACHE_SECTOR_SIZE];
};

/* Caller holds cache_lock.  Takes exactly this slot, which must be dirty
 * and not already spoken for. */
static void wb_take_idx_locked(struct wb_job *j, int idx)
{
    j->idx = idx;
    j->dev = cache[idx].dev;
    j->lba = cache[idx].lba;
    memcpy(j->bounce, cache[idx].data, CACHE_SECTOR_SIZE);
    cache[idx].dirty = false; /* this job owns those bytes now */
    cache[idx].busy = true;   /* and the slot stays put until they land */
}

/* Same, but by search: the first dirty resident of `dev` (NULL = any
 * device) that this pass has not already attempted.  `tried` is the pass's
 * bitmask of failures -- a refused writeback goes back on the dirty list,
 * and without the mask the search would pick it again forever where the
 * old single for-loop simply walked past it to the next slot. */
static bool wb_take_locked(struct wb_job *j, struct blkdev *dev, u64 tried)
{
    for (int i = 0; i < CACHE_ENTRIES; i++) {
        if (!cache[i].valid || !cache[i].dirty || cache[i].busy) continue;
        if (dev && cache[i].dev != dev) continue;
        if (tried & (1ULL << i)) continue;
        wb_take_idx_locked(j, i);
        return true;
    }
    return false;
}

/* Re-open the slot after the transfer.  Caller re-holds cache_lock. */
static void wb_finish_locked(struct wb_job *j, int rc)
{
    cache[j->idx].busy = false;
    /* Never reached the device: the bytes are still in data[] (we only
     * copied them out), so put the entry back where it was -- dirty, and
     * whatever the entry is now holds the truth either way. */
    if (rc != 0) cache[j->idx].dirty = true;
}

/* all helpers below assume cache_lock is held */

/* Reserve the slot a miss will fill, doing its writeback first if the
 * chosen victim happens to be dirty.  Caller enters holding cache_lock with
 * `flags` valid.  On success: 0, lock still HELD, *idx_out a slot that is
 * neither dirty nor busy and has been accounted as an eviction.  On
 * failure the lock has ALREADY been dropped (the writeback needed it
 * released), so the caller has nothing left to undo. */
static int cache_slot_take(u64 flags, int *idx_out)
{
    int idx = cache_find_victim();
    if (idx < 0) {
        spin_unlock_irqrestore(&cache_lock, flags);
        return LNXRM_EFAIL;
    }

    if (cache[idx].valid && cache[idx].dirty) {
        struct wb_job j;
        wb_take_idx_locked(&j, idx); /* dirty was checked above: it hits */
        spin_unlock_irqrestore(&cache_lock, flags);
        ktest_io_probe(); /* C7: this transfer runs lock-free too */
        bump(&stats.writebacks);
        int rc = blk_io_write(j.dev, j.lba, 1, j.bounce);
        spin_lock_irqsave(&cache_lock, &flags);
        wb_finish_locked(&j, rc);
        if (rc != 0) {
            /* Answer the miss with the writeback's own error: a victim we
             * could not flush is not one we may overwrite (T-005). */
            spin_unlock_irqrestore(&cache_lock, flags);
            return rc;
        }
        /* The lock was released for that transfer, so ask again rather
         * than assume the same slot is still the right answer. */
        idx = cache_find_victim();
        if (idx < 0) {
            spin_unlock_irqrestore(&cache_lock, flags);
            return LNXRM_EFAIL;
        }
    }

    cache_note_eviction(idx);
    *idx_out = idx;
    return 0;
}

/* Flush every dirty sector of `dev` (NULL = every device), one sector at a
 * time with the lock dropped around each transfer.  Caller enters holding
 * cache_lock with `flags` valid; returns with it RELEASED.  That shape is
 * the point: a slow disk is held off this lock for a memcpy, not for the
 * length of the queue. */
static int cache_flush_dev(u64 flags, struct blkdev *dev)
{
    int ret = 0;
    u64 tried = 0;
    for (;;) {
        struct wb_job j;
        if (!wb_take_locked(&j, dev, tried)) break;
        int idx = j.idx;
        spin_unlock_irqrestore(&cache_lock, flags);
        ktest_io_probe(); /* C7: prove the lock is really gone here */
        bump(&stats.writebacks);
        int rc = blk_io_write(j.dev, j.lba, 1, j.bounce);
        spin_lock_irqsave(&cache_lock, &flags);
        wb_finish_locked(&j, rc);
        if (rc != 0) {
            ret = rc;
            tried |= 1ULL << idx; /* dirty again: not ours to retry this pass */
        }
    }
    spin_unlock_irqrestore(&cache_lock, flags);
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
    int slot;
    int rc = cache_slot_take(flags, &slot);
    if (rc < 0) return rc; /* failure already dropped the lock */

    /* Take the slot out of circulation before the device touches it: valid
     * is what a reader looks for and busy is what the victim choice looks
     * for, so from here to the publish below nobody can find this entry or
     * hand its data[] to anyone else -- which is precisely what lets the
     * lock go while the driver writes into it. */
    cache[slot].valid = false;
    cache[slot].dirty = false;
    cache[slot].busy = true;
    spin_unlock_irqrestore(&cache_lock, flags);

    /* --- C7: the device runs with cache_lock NOT held --- */
    ktest_io_probe();
    int ret = blk_io_read(dev, lba, 1, cache[slot].data);

    spin_lock_irqsave(&cache_lock, &flags);
    if (ret != 0) {
        /* The driver may have filled part of the buffer before refusing, and
         * this slot still claims to hold its *previous* LBA (valid is set
         * only below).  Keeping it would hand every later reader of that LBA
         * a half-overwritten sector, so a failed read drops the slot: a lost
         * cached copy costs one more I/O, corrupted data costs a filesystem. */
        cache[slot].valid = false;
        cache[slot].dirty = false;
        cache[slot].dev = NULL;
        cache[slot].lba = 0;
        cache[slot].stamp = 0;
        cache[slot].busy = false;
        spin_unlock_irqrestore(&cache_lock, flags);
        return ret;
    }

    /* Publish it -- the slot was invisible to everyone for the whole
     * transfer, so nothing else can have claimed it in the meantime. */
    cache[slot].lba = lba;
    cache[slot].dev = dev;
    cache[slot].valid = true;
    cache[slot].dirty = false;
    cache[slot].stamp = ++lru_seq; /* freshly filled == most recently used */
    cache[slot].busy = false;

    /* Copy to user buffer */
    memcpy(buf, cache[slot].data, CACHE_SECTOR_SIZE);
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
        /* Cache miss: need to allocate new entry.  Taking the slot may
         * flush a dirty victim, which needs the lock released -- but the
         * staging below is pure metadata (no transfer of its own), so
         * everything from here to the unlock really is under the lock. */
        bump(&stats.misses);
        int slot;
        int rc = cache_slot_take(flags, &slot);
        if (rc < 0) return rc;

        idx = slot;
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
    return cache_flush_dev(flags, dev); /* releases the lock on the way out */
}

/* C27: a staged dirty sector reached the device only at eviction or
 * explicit flush, so a crash could lose up to 32 KiB of file bytes the
 * caller may believe are theirs; the journal protects its own metadata
 * only.  Three doors now close that window -- the BSP tick asks about
 * once a second (blk_cache_tick), SYS_sync asks synchronously, and the
 * close of a regular file asks once -- all funnel here.  A dirty copy
 * whose writeback fails stays dirty (it is the only copy; clearing it
 * would publish a durability the device never got), and the last error
 * wins. */
int blk_cache_sync_all(void)
{
    u64 flags;
    spin_lock_irqsave(&cache_lock, &flags);
    return cache_flush_dev(flags, NULL);
}

/* The panic-side variant refuses to wait: if the panic itself came from
 * inside this cache's lock region, the machine must still reach `hlt` --
 * losing those bytes beats hanging forever without a halt reached. */
int blk_cache_sync_all_try(void)
{
    u64 flags;
    if (!spin_trylock_irqsave(&cache_lock, &flags)) return LNXRM_EFAIL;
    return cache_flush_dev(flags, NULL);
}

/* BSP tick hook -- the periodic door.  Deliberately silent while no task
 * exists yet (the whole boot self-test suite runs with current == NULL,
 * and the timer does not even tick before init is spawned): a background
 * writeback would randomly move the writeback/transfer counts the ktests
 * pin down, turning count assertions flaky.  panic() has its own door,
 * so a dying machine never queues a slower one.
 *
 * C7: the door KNOCKS, it does not wait.  This runs in interrupt context,
 * and the old blocking blk_cache_sync_all() here meant the BSP's timer
 * handler could spin with interrupts off against whoever was doing cache
 * metadata right now -- the exact "a slow disk reaches the whole machine"
 * amplification this defect is.  A trylock that loses costs one delayed
 * second (the sectors stay dirty for SYS_sync, the file close, or the next
 * tick); a trylock that wins still spends only a memcpy under the lock,
 * because cache_flush_dev() drops it around every transfer. */
#define BLK_SYNC_TICKS (HZ) /* one second */

void blk_cache_tick(void)
{
    static u64 last_sync;
    if (in_panic() || !current) return;
    if ((u64)(jiffies - last_sync) < BLK_SYNC_TICKS) return;
    last_sync = jiffies;
    blk_cache_sync_all_try();
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
    u64 tried = 0; /* writebacks refused by this pass: same rule as flush */
    spin_lock_irqsave(&cache_lock, &flags);
    for (;;) {
        int idx = -1;
        for (int i = 0; i < CACHE_ENTRIES; i++) {
            if (!cache[i].valid || (dev && cache[i].dev != dev)) continue;
            if (cache[i].busy) continue;   /* C7: an in-flight transfer owns it */
            if (tried & (1ULL << i)) continue;
            idx = i;
            break;
        }
        if (idx < 0) break;

        if (cache[idx].dirty) {
            struct wb_job j;
            wb_take_idx_locked(&j, idx);
            spin_unlock_irqrestore(&cache_lock, flags);
            ktest_io_probe(); /* C7: the lock is gone for the transfer */
            bump(&stats.writebacks);
            int rc = blk_io_write(j.dev, j.lba, 1, j.bounce);
            spin_lock_irqsave(&cache_lock, &flags);
            wb_finish_locked(&j, rc);
            if (rc != 0) {
                /* The sector never reached the device: keep it (still dirty)
                 * rather than drop the only copy of what was written. */
                ret = rc;
                tried |= 1ULL << idx;
                continue;
            }
        }
        /* Drop it.  It cannot have moved while busy was set, so this is
         * still the slot the loop found -- and the search above is what
         * keeps a busy slot from being dropped under its transfer. */
        cache[idx].valid = false;
        cache[idx].dirty = false;
        cache[idx].dev = NULL;
        cache[idx].stamp = 0;
    }
    spin_unlock_irqrestore(&cache_lock, flags);
    return ret;
}
