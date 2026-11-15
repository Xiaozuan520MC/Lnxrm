/* T-007 (C24) and T-008 (C26): what the block cache promises.
 *
 * T-007 -- the victim.  The cache used to choose its victim with a single
 * `used` bit a hit set and nothing cleared, so once all 64 slots were "used"
 * the only move was to clear every bit and take slot 0: a working set larger
 * than the cache recycled the same slots forever.  Two cases pin the policy:
 *
 *   test_lru_victim_is_the_least_recently_used
 *       fills all 64 slots, touches one, asks for a 65th sector and checks
 *       that the *oldest untouched* one went and the touched one stayed.
 *   test_hot_sectors_survive_a_cold_stream
 *       rewrites 8 sectors 6 times while streaming 96 others past them.
 *       Staging a resident sector costs nothing and only evicting a dirty one
 *       costs a device write, so "<= 8 + slack writebacks" is exactly "the hot
 *       set was never evicted".
 *
 * The write-back cache is measured, not asserted about: blk_cache_stats()
 * counts hits, misses, evictions, writebacks and the transfers that really
 * reached a driver.
 *
 * T-008 -- coherence.  C26 claimed the read path bypassed the cache while the
 * write path did not, so a read could return what was on disk before the
 * write.  That split is gone, but the rule deserves a test rather than a
 * comment: a value written through the cache must come back identical, both
 * while resident and after the cache has been thrown away.  The other half is
 * that each region uses exactly one path -- the FAT is written straight to the
 * device and read from m->fat_pages, never from the cache; if a FAT sector
 * ever became resident the two answers could disagree.
 *
 * Everything here is destructive only on sectors the case itself created, or
 * read-only, and every case leaves the cache empty of its own traffic.
 *
 * C7 (2026-11-13) -- the lock itself.  cache_lock used to be held across the
 * whole device transfer on both paths, stacked under fat_fs_lock, so one slow
 * disk held up every CPU touching the cache or the filesystem.
 * test_cache_lock_is_released_across_device_io below asks the cache, at the
 * instant it is about to run a transfer, whether it still owns its own lock --
 * the claim is measured rather than read off the code, and the probe answers
 * with a plain flag read so a wrong answer fails instead of spinning. */
#include <sys/ktest.h>
#include <console.h>
#include <disk.h>
#include <fat32.h>
#include "kblk.h"

/* The mounted volume, or a skip if there is none (same rule as t_ioerr.c). */
static struct fat_mount *mnt_or_skip(const char *who)
{
    struct fat_mount *m = fat_priv;
    if (!m) {
        ktest_skip("no FAT32 volume (cache case skipped)");
        (void)who;
        return NULL;
    }
    return m;
}

/* A stale file from an aborted earlier run must not fail this case. */
static void drop(const char *path)
{ fat_unlink_impl(fat_priv, path); }

static int open_resolve(const char *path, struct vnode *vn)
{
    memset(vn, 0, sizeof(*vn));
    return fat_lookup_impl(fat_priv, path, vn);
}

static void close_resolve(struct vnode *vn)
{
    if (vn->fs_data) kfree(vn->fs_data);
    vn->fs_data = NULL;
}

/* The first `n` sectors of `path`, as device LBAs.  They belong to a file
 * this file created, so staging data on them is safe. */
static int file_lbas(struct fat_mount *m, const char *path, u64 *out, int n)
{
    struct vnode vn;
    if (open_resolve(path, &vn) != 0) return -1;
    struct resolve *r = vn.fs_data;
    u32 clus = r ? (((u32)r->de.fstclushi << 16) | r->de.fstcluslo) : 0;
    int got = 0;
    if (clus >= 2 && clus < m->max_cluster) {
        struct fat_chain ch;
        fat_chain_start(&ch, m, clus);
        while (got < n && fat_chain_more(&ch)) {
            u64 base = cluster_lba(m, ch.clus);
            for (u32 s = 0; s < m->sectors_per_cluster && got < n; s++) out[got++] = base + s;
            fat_chain_advance(&ch);
        }
    }
    close_resolve(&vn);
    return got;
}

/* ------------------------------- T-007 -------------------------------- */

/* Fill the cache, touch one entry, then ask for a 65th sector: the victim
 * must be the least recently used entry, not the touched one. */
static void test_lru_victim_is_the_least_recently_used(void)
{
    struct fat_mount *m = mnt_or_skip("lru.order");
    if (!m) return;
    struct blkdev *dev = m->dev;
    K_ASSERT(dev->num_sectors > 140);

    /* Start from a known state: everything written back, nothing resident,
     * so the64 fills land in slots 0..63 and the numbering below holds.  The
     * LBAs used here are only ever read, so this case can touch any sector of
     * the disk -- no file has to exist and nothing can change. */
    K_ASSERT_EQ(blk_cache_purge(NULL), 0);
    blk_cache_stats_reset();

    u64 base = dev->num_sectors - 70; /* base..base+64 all inside the disk */
    u8 buf[CACHE_SECTOR_SIZE];
    for (int i = 0; i < CACHE_ENTRIES; i++)
        K_ASSERT_EQ(blk_cache_read(dev, base + i, buf), 0);
    K_ASSERT_EQ(blk_cache_read(dev, base, buf), 0);    /* touch S0 -> MRU */
    K_ASSERT_EQ(blk_cache_read(dev, base + 64, buf), 0); /* needs a slot */

    struct blk_cache_stats s;
    blk_cache_stats_get(&s);
    K_EXPECT_EQ(s.misses, 65);  /* 64 fills + one new sector */
    K_EXPECT_EQ(s.hits, 1);     /* the touch was served from the cache */
    K_EXPECT_EQ(s.evictions, 1);
    K_EXPECT_EQ(s.dev_reads, 65);

    K_EXPECT_EQ(blk_cache_lookup(dev, base + 0), 0);   /* touched: kept, slot 0 */
    K_EXPECT_EQ(blk_cache_lookup(dev, base + 1), -1);  /* oldest: evicted */
    K_EXPECT_EQ(blk_cache_lookup(dev, base + 63), 63); /* untouched: still there */
    K_EXPECT_EQ(blk_cache_lookup(dev, base + 64), 1);  /* the newcomer took it */

    kprintf("[blk_cache] T-007 lru: victim = least recently used "
            "(dev_reads=%llu, evictions=%llu)\n",
            s.dev_reads, s.evictions);
}

/* The card's acceptance: a file spanning 200 sectors.  Every sector is
 * staged exactly once, so the bound "written back at most once each" holds
 * no matter which slot is picked -- what it proves is that no sector is
 * staged, evicted and staged again (which would double every count). */
static void test_a_200_sector_file_costs_one_writeback_each(void)
{
    struct fat_mount *m = mnt_or_skip("lru.200");
    if (!m) return;
    struct blkdev *dev = m->dev;
    drop("/t007big");
    K_ASSERT_EQ(blk_cache_purge(dev), 0);
    K_ASSERT_EQ(fat_create_impl(fat_priv, "/t007big"), 0);

    struct vnode vn;
    K_ASSERT_EQ(open_resolve("/t007big", &vn), 0);
    blk_cache_stats_reset();

    u8 page[CACHE_SECTOR_SIZE];
    for (u32 i = 0; i < 200; i++) {
        memset(page, (int)(i * 7 + 3), sizeof(page));
        K_ASSERT_EQ(fat_write_impl(fat_priv, vn.fs_data, (u64)i * 512, page, sizeof(page)),
                    (int)sizeof(page));
    }

    struct blk_cache_stats s;
    blk_cache_stats_get(&s);
    /* 200 data sectors + the directory entry that grew with them.  FAT
     * sectors are not cached, so their (direct) writes are not writebacks. */
    K_EXPECT(s.writebacks <= 200 + 8);
    K_EXPECT(s.evictions > 0); /* a 200-sector run must have evicted something */

    /* read a few back while the tail is still resident ... */
    u8 back[CACHE_SECTOR_SIZE];
    int mismatch = -1;
    for (u32 i = 0; i < 200; i += 37) {
        memset(page, (int)(i * 7 + 3), sizeof(page));
        if (fat_read_impl(fat_priv, vn.fs_data, (u64)i * 512, back, sizeof(back)) !=
                (int)sizeof(back) ||
            memcmp(back, page, sizeof(back)) != 0)
            mismatch = (int)i;
    }
    K_EXPECT_EQ(mismatch, -1);

    /* ... and again after the cache has been thrown away, so the bytes are
     * the device's, not the cache's (T-008 coherence, same file). */
    if (mismatch == -1) K_ASSERT_EQ(blk_cache_purge(NULL), 0);
    for (u32 i = 0; mismatch == -1 && i < 200; i += 37) {
        memset(page, (int)(i * 7 + 3), sizeof(page));
        if (fat_read_impl(fat_priv, vn.fs_data, (u64)i * 512, back, sizeof(back)) !=
                (int)sizeof(back) ||
            memcmp(back, page, sizeof(back)) != 0)
            mismatch = (int)i + 1000;
    }
    K_EXPECT_EQ(mismatch, -1);

    close_resolve(&vn);
    drop("/t007big");
    kprintf("[blk_cache] T-007 200-sector file: %llu writebacks, %llu evictions "
            "(bounded by 208)\n",
            s.writebacks, s.evictions);
}

/* Rewrite a hot set while a cold stream runs past it.  Only evicting a dirty
 * sector costs a device write, so "at most one writeback per hot sector" and
 * "the hot set is still resident" are the same statement. */
static void test_hot_sectors_survive_a_cold_stream(void)
{
    struct fat_mount *m = mnt_or_skip("lru.hot");
    if (!m) return;
    struct blkdev *dev = m->dev;
    K_ASSERT(dev->num_sectors > 170);
    drop("/t007hot");
    K_ASSERT_EQ(blk_cache_purge(NULL), 0);
    K_ASSERT_EQ(fat_create_impl(fat_priv, "/t007hot"), 0);

    /* Enough bytes for at least 8 sectors whatever the cluster size is
     * (a cluster holds a whole number of sectors, so >= 8 sectors of file
     * means >= 8 LBAs to stage on). */
    u8 seed[CACHE_SECTOR_SIZE];
    memset(seed, 0x5A, sizeof(seed));
    struct vnode vn;
    K_ASSERT_EQ(open_resolve("/t007hot", &vn), 0);
    for (u32 off = 0; off < 8 * m->bytes_per_sector; off += sizeof(seed))
        K_ASSERT_EQ(fat_write_impl(fat_priv, vn.fs_data, off, seed, sizeof(seed)),
                    (int)sizeof(seed));
    close_resolve(&vn);

    u64 hot[8];
    int nh = file_lbas(m, "/t007hot", hot, 8);
    K_ASSERT_EQ(nh, 8);

    const u64 cold_base = dev->num_sectors - 170; /* read-only LBAs */
    const int ROUNDS = 6, COLD = 16;
    u8 page[CACHE_SECTOR_SIZE], cold[CACHE_SECTOR_SIZE];
    struct blk_cache_stats s0, s1;
    blk_cache_stats_reset();
    blk_cache_stats_get(&s0);

    for (int round = 0; round < ROUNDS; round++) {
        for (int j = 0; j < 8; j++) {
            memset(page, 0x40 + j, sizeof(page));
            page[0] = (u8)round; /* distinguishable if it ever hits the disk */
            K_ASSERT_EQ(blk_cache_write(dev, hot[j], page), 0);
        }
        for (int k = 0; k < COLD; k++)
            K_ASSERT_EQ(blk_cache_read(dev, cold_base + (u64)round * COLD + k, cold), 0);
    }
    K_ASSERT_EQ(blk_cache_flush(dev), 0);
    blk_cache_stats_get(&s1);

    /* 96 distinct cold sectors, each read exactly once; the hot set is only
     * ever staged, never read, so it cannot show up here. */
    K_EXPECT_EQ(s1.dev_reads - s0.dev_reads, ROUNDS * COLD);
    K_EXPECT(s1.evictions - s0.evictions > 0); /* the stream really did push */
    /* The hot sectors are rewritten 6 times each; only their final writeback
     * may reach the device.  The old victim choice wrote them back every
     * round (slot 0 first), which is well over this bound. */
    K_EXPECT(s1.writebacks - s0.writebacks <= 8 + 4);
    for (int j = 0; j < 8; j++)
        K_EXPECT(blk_cache_lookup(dev, hot[j]) >= 0);

    drop("/t007hot");
    kprintf("[blk_cache] T-007 hot set: %llu writebacks after %d cold reads "
            "(bounded by 12)\n",
            s1.writebacks - s0.writebacks, ROUNDS * COLD);
}

/* ------------------------------- T-008 -------------------------------- */

/* 1000 write->read round trips on one sector, then the same bytes with the
 * cache thrown away.  A stale copy in either direction fails one of them. */
static void test_write_readback_is_coherent(void)
{
    struct fat_mount *m = mnt_or_skip("cache.coherent");
    if (!m) return;
    struct blkdev *dev = m->dev;
    drop("/t008");
    K_ASSERT_EQ(blk_cache_purge(dev), 0);
    K_ASSERT_EQ(fat_create_impl(fat_priv, "/t008"), 0);

    struct vnode vn;
    K_ASSERT_EQ(open_resolve("/t008", &vn), 0);

    u8 page[CACHE_SECTOR_SIZE], back[CACHE_SECTOR_SIZE];
    int bad = -1;
    for (int i = 0; i < 1000 && bad == -1; i++) {
        memset(page, (int)(i * 13 + 5), sizeof(page));
        page[0] = (u8)(i >> 8); /* every iteration has its own first bytes */
        page[1] = (u8)i;
        if (fat_write_impl(fat_priv, vn.fs_data, 0, page, sizeof(page)) != (int)sizeof(page)) {
            bad = -2; /* the write refused: not a coherence problem, but a failure */
            break;
        }
        memset(back, 0xAA, sizeof(back));
        if (fat_read_impl(fat_priv, vn.fs_data, 0, back, sizeof(back)) != (int)sizeof(back) ||
            memcmp(back, page, sizeof(back)) != 0)
            bad = i;
    }
    K_EXPECT_EQ(bad, -1);

    /* Same value, but asked of the device: purge flushes what is staged and
     * drops every entry of this device, so this read can only come from the
     * disk. */
    K_ASSERT_EQ(blk_cache_purge(dev), 0);
    memset(back, 0, sizeof(back));
    int n = fat_read_impl(fat_priv, vn.fs_data, 0, back, sizeof(back));
    K_EXPECT_EQ(n, (int)sizeof(back));
    K_EXPECT_EQ(memcmp(back, page, sizeof(back)), 0);

    close_resolve(&vn);
    drop("/t008");
    kprintf("[fat32] T-008 coherent: 1000 write->read round trips, then the "
            "same bytes straight from the device\n");
}

/* The FAT uses one path and the data region another; both halves have to
 * stay true, or one of them is reading a copy nobody refreshes. */
static void test_fat_region_is_never_cached(void)
{
    struct fat_mount *m = mnt_or_skip("cache.fatsplit");
    if (!m) return;

    /* fat_set_entry() writes the FAT sector straight to the device: the
     * allocation has to be on disk before the directory entry naming the
     * cluster can be staged, and FAT reads come from m->fat_pages, not from
     * a sector buffer.  A resident FAT sector would be a stale third copy
     * (C26 in its remaining form). */
    u64 first = m->reserved_sectors;
    u64 last = (u64)m->reserved_sectors + (u64)m->num_fats * m->fatsz;
    int cached = 0;
    for (u64 lba = first; lba < last; lba++)
        if (blk_cache_lookup(m->dev, lba) >= 0) cached++;
    K_EXPECT_EQ(cached, 0);
    K_EXPECT(last > first);

    kprintf("[fat32] T-008 FAT region: %llu sectors, none of them cached "
            "(data and directories are)\n",
            last - first);
}

/* C27: the durability doors.  A staged dirty sector must reach its
 * device at the first sync, the push must be measurable in the counters,
 * and a second sync over a clean cache must be a no-op (no writebacks,
 * no device writes at all).  Both facts are read from the DEVICE, not
 * from the cache: blk_io_read() is the driver-level door the cache sits
 * on top of.  The image is freshly mkfs'd, so the clusters under a new
 * file hold known zeros -- "before" cannot accidentally equal "after". */
static void test_sync_hands_dirty_sectors_to_the_device(void)
{
    struct fat_mount *m = mnt_or_skip("sync");
    if (!m) return;
    struct blkdev *dev = m->dev;
    drop("/t027");
    K_ASSERT_EQ(blk_cache_purge(NULL), 0);
    K_ASSERT_EQ(fat_create_impl(fat_priv, "/t027"), 0);

    struct vnode vn;
    K_ASSERT_EQ(open_resolve("/t027", &vn), 0);
    u8 page[CACHE_SECTOR_SIZE];
    memset(page, 0x3C, sizeof(page));
    page[0] = 0xC7;
    K_ASSERT_EQ(fat_write_impl(fat_priv, vn.fs_data, 0, page, sizeof(page)),
                (int)sizeof(page));
    u64 lbas[2];
    K_ASSERT_EQ(file_lbas(m, "/t027", lbas, 1), 1);

    struct blk_cache_stats s0, s1, s2, s3;
    blk_cache_stats_reset();
    blk_cache_stats_get(&s0);
    K_EXPECT_EQ(s0.writebacks, 0); /* the reset is the start line */
    K_EXPECT_EQ(s0.dev_writes, 0);

    u8 raw[2][CACHE_SECTOR_SIZE];
    K_ASSERT_EQ(blk_io_read(dev, lbas[0], 1, raw[0]), 0);
    K_EXPECT(memcmp(raw[0], page, sizeof(page)) != 0); /* staged, not durable */

    K_EXPECT_EQ(blk_cache_sync_all(), 0); /* one door: the tick, SYNC and */
                                          /* close all funnel into this */
    blk_cache_stats_get(&s1);
    K_EXPECT(s1.writebacks >= 1);         /* the push is measurable */
    K_EXPECT(s1.dev_writes >= 1);         /* and reached a driver */
    K_ASSERT_EQ(blk_io_read(dev, lbas[0], 1, raw[1]), 0);
    K_EXPECT(memcmp(raw[1], page, sizeof(page)) == 0); /* durable now */

    /* Everything is clean: a second sync answers 0 and moves nothing. */
    blk_cache_stats_get(&s2);
    K_EXPECT_EQ(blk_cache_sync_all(), 0);
    blk_cache_stats_get(&s3);
    K_EXPECT_EQ(s3.writebacks - s2.writebacks, 0);
    K_EXPECT_EQ(s3.dev_writes - s2.dev_writes, 0);

    close_resolve(&vn);
    drop("/t027");
    kprintf("[blk_cache] sync: staged bytes are device bytes after one "
            "sync (%llu writebacks), second sync is a no-op\n",
            s1.writebacks);
}

/* ---- C7: cache_lock is a metadata lock, not a transfer lock ------------
 * Two paths in this file perform a device transfer: a FILL (blk_cache_read
 * on a resident miss) and a WRITEBACK (blk_cache_flush / sync / purge of a
 * staged sector).  Both used to run it while holding cache_lock, which sat
 * under fat_fs_lock -- so one slow disk reached the whole machine instead
 * of one device.  The fix reserves the slot and drops the lock for the
 * transfer; this case verifies that by asking, at the instant the transfer
 * is about to start, rather than by trusting the shape of the code.
 *
 * blk_cache_set_io_probe() installs a callback at every such point.  It
 * answers with blk_cache_lock_is_free() -- a plain read of the flag, not a
 * trylock -- so a regression that puts the lock back around the transfer
 * makes the count non-zero and fails, instead of deadlocking the self-test
 * against a lock this same CPU already holds.  Staging (blk_cache_write)
 * is pure metadata and must NOT probe; that is asserted too, so the probe
 * count means "transfers" and nothing else. */
static int c7_probes;
static int c7_probes_locked;

static void c7_probe(void)
{
    c7_probes++;
    if (!blk_cache_lock_is_free()) c7_probes_locked++;
}

static void test_cache_lock_is_released_across_device_io(void)
{
    struct kblk *k = kblk_make("kblkc7", 64, 64);
    K_ASSERT(k != NULL);
    /* Empty the whole cache first, not just this device's part of it: the
     * probe is about to be armed, and an unrelated dirty sector found by
     * the victim choice would fire it for a reason this case cannot name.
     * Done before arming, so this purge's own traffic cannot count either. */
    K_ASSERT_EQ(blk_cache_purge(NULL), 0);

    c7_probes = 0;
    c7_probes_locked = 0;
    blk_cache_set_io_probe(c7_probe);

    /* (1) WRITEBACK path: stage a sector, then push it out. */
    u8 page[CACHE_SECTOR_SIZE];
    memset(page, 0x37, sizeof(page));
    K_ASSERT_EQ(blk_cache_write(&k->dev, 0, page), 0);
    K_EXPECT_EQ(c7_probes, 0);   /* staging is metadata: no transfer yet */
    K_EXPECT_EQ(k->writes, 0u);  /* ... so nothing reached the device */
    K_ASSERT_EQ(blk_cache_flush(&k->dev), 0);
    int after_writeback = c7_probes;
    K_EXPECT(after_writeback >= 1); /* the flush really probed */
    K_EXPECT_EQ(k->writes, 1u);     /* and really wrote */

    /* (2) FILL path: a sector that is not resident, so the read has to go
     *     to the device.  Purge drops the one above without a transfer (it
     *     is clean now), which keeps the read count exact. */
    K_ASSERT_EQ(blk_cache_purge(&k->dev), 0);
    K_EXPECT_EQ(blk_cache_lookup(&k->dev, 5), -1);
    u8 out[CACHE_SECTOR_SIZE];
    K_ASSERT_EQ(blk_cache_read(&k->dev, 5, out), 0);
    K_EXPECT(c7_probes > after_writeback); /* the fill probed as well */
    K_EXPECT_EQ(k->reads, 1u);
    K_EXPECT(blk_cache_lookup(&k->dev, 5) >= 0); /* and installed it */
    K_EXPECT_EQ(out[0], 0); /* kblk zeroes what it backs: readable bytes */

    blk_cache_set_io_probe(NULL);

    /* What the case is here for: every transfer outside the lock, none in. */
    K_EXPECT(c7_probes >= 2);
    K_EXPECT_EQ(c7_probes_locked, 0);

    K_ASSERT_EQ(blk_cache_purge(&k->dev), 0);
    kblk_free(k);
    kprintf("[blk_cache] C7: %d transfers ran with cache_lock released, "
            "%d found it held\n", c7_probes, c7_probes_locked);
}

KTEST("blkcache", KTEST_LATE, test_lru_victim_is_the_least_recently_used);
KTEST("blkcache", KTEST_LATE, test_a_200_sector_file_costs_one_writeback_each);
KTEST("blkcache", KTEST_LATE, test_hot_sectors_survive_a_cold_stream);
KTEST("blkcache", KTEST_LATE, test_write_readback_is_coherent);
KTEST("blkcache", KTEST_LATE, test_fat_region_is_never_cached);
KTEST("blkcache", KTEST_LATE, test_sync_hands_dirty_sectors_to_the_device);
KTEST("blkcache", KTEST_LATE, test_cache_lock_is_released_across_device_io);
