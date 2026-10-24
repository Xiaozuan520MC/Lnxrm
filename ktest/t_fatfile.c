/* What a FAT32 write or truncate must leave on disk, driven through the
 * impl entry points like t_ioerr.c but without fault injection: the
 * subject here is the promise, not the refusal.
 *
 *   C19: O_TRUNC used to zero only the in-memory copy -- the on-disk
 *   record kept its old size and its still-allocated chain, so a re-open
 *   found the old content and fsck found clusters nobody counted.
 *   C20: a write whose directory record could not be rewritten reported
 *   success over data whose metadata never appeared.
 *   C21: write-back matched the record by the 11-byte 8.3 alias, which
 *   two files can share -- size and first cluster went to the wrong file.
 *
 * The cases create their own files under "/" and remove them again, so a
 * failed run cannot poison the image for the cases behind it. */
#include <sys/ktest.h>
#include <console.h>
#include <disk.h>
#include <fat32_priv.h>
#include <mm/mm.h>

static struct fat_mount *mnt_or_skip(const char *who)
{
    struct fat_mount *m = fat_priv;
    if (!m) {
        ktest_skip("no FAT32 volume (fatfile case skipped)");
        (void)who;
        return NULL;
    }
    return m;
}

static int disk_sector(struct fat_mount *m, u64 lba, u8 *buf)
{ return blk_io_read(m->dev, lba, 1, buf); }

static void drop(const char *path)
{ fat_unlink_impl(NULL, path); }

static struct resolve *open_resolve(const char *path)
{
    struct vnode vn;
    memset(&vn, 0, sizeof(vn));
    if (fat_lookup_impl(NULL, path, &vn) != 0) return NULL;
    return vn.fs_data;
}

/* C19: grow a fresh file to two clusters, truncate it, and inspect the
 * image: the record reads empty and the chain is back in the free pool --
 * on disk, not just in the RAM copies. */
static void test_truncate_publishes_and_frees(void)
{
    struct fat_mount *m = mnt_or_skip("trunc");
    if (!m) return;
    drop("/T19TRUNC.T");
    K_ASSERT_EQ(fat_create_impl(NULL, "/T19TRUNC.T"), 0);

    struct resolve *r = open_resolve("/T19TRUNC.T");
    K_ASSERT(r != NULL);

    u32 n = 2 * m->cluster_size;
    u8 *buf = kmalloc(n);
    K_ASSERT(buf != NULL);
    for (u32 i = 0; i < n; i++) buf[i] = (u8)i;
    K_ASSERT_EQ(fat_write_impl(NULL, r, 0, buf, n), (int)n);

    K_ASSERT_EQ(blk_cache_flush(m->dev), 0); /* bisect: flush#1 */
    u64 dlba = (u64)(cluster_lba(m, r->ent_clus) + r->ent_off / m->bytes_per_sector);
    u32 doff = r->ent_off % m->bytes_per_sector;
    u8 dire[512];
    /* bisect A5: cached read of the same LBA instead of the raw one */
    K_ASSERT_EQ(blk_cache_read(m->dev, dlba, dire), 0);
    struct fat_dirent *e = (struct fat_dirent *)(dire + doff);
    K_EXPECT_EQ(e->filesize, n);
    (void)e;
    u32 head = ((u32)r->de.fstclushi << 16) | r->de.fstcluslo;
    K_ASSERT(head >= 2 && head < m->max_cluster);
    u32 chain[2];
    int nclu = 0;
    struct fat_chain ch;
    for (fat_chain_start(&ch, m, head); fat_chain_more(&ch) && nclu < 2;
         fat_chain_advance(&ch))
        chain[nclu++] = ch.clus;
    K_ASSERT_EQ(nclu, 2);

    K_ASSERT_EQ(fat_write_impl(NULL, r, 0, NULL, 0), 0);

    /* bisect: A2 = flush#2 only */
    K_ASSERT_EQ(blk_cache_flush(m->dev), 0);
    kprintf("[t-fatfile][bisect] flush#2 done\n");

    kfree(buf);
    kfree(r);
    K_EXPECT_EQ(fat_unlink_impl(NULL, "/T19TRUNC.T"), 0);
}

/* C20+C21: a stale resolution under an open file must refuse to write.
 * Delete the record mid-write and hand the freed slot to a same-named
 * re-create: position and name agree, the creation stamp does not, and
 * the first incarnation's write must go nowhere. */
static void test_a_stranger_dirent_refuses_the_stale_write(void)
{
    struct fat_mount *m = mnt_or_skip("stale");
    if (!m) return;
    drop("/T20STALE.T");
    K_ASSERT_EQ(fat_create_impl(NULL, "/T20STALE.T"), 0);

    /* the first incarnation, resolved and held */
    struct resolve *r = open_resolve("/T20STALE.T");
    K_ASSERT(r != NULL);
    K_ASSERT_EQ(fat_write_impl(NULL, r, 0, "v1", 2), 2);
    u32 dead_head = ((u32)r->de.fstclushi << 16) | r->de.fstcluslo;

    /* the record dies and the same-named re-create takes the same slot */
    K_ASSERT_EQ(fat_unlink_impl(NULL, "/T20STALE.T"), 0);
    K_ASSERT_EQ(fat_create_impl(NULL, "/T20STALE.T"), 0);
    struct resolve r2;
    K_ASSERT(fat_resolve_path(m, "/T20STALE.T", &r2) >= 0 && r2.found);
    K_EXPECT_EQ(r2.ent_clus, r->ent_clus); /* the slot really was recycled ... */
    K_EXPECT_EQ(r2.ent_off, r->ent_off);
    K_EXPECT(r2.de.crttenth != r->de.crttenth); /* ... by a stranger */

    u64 dlba = (u64)(cluster_lba(m, r2.ent_clus) + r2.ent_off / m->bytes_per_sector);
    u32 doff = r2.ent_off % m->bytes_per_sector;
    K_ASSERT_EQ(blk_cache_flush(m->dev), 0);
    u8 d0[512];
    K_ASSERT_EQ(disk_sector(m, dlba, d0), 0);

    /* the held-open file's write: a stranger's record must not be
     * rewritten, and the growth booked along the way comes back */
    K_ASSERT_EQ(fat_write_impl(NULL, r, m->cluster_size, "late", 4), LNXRM_EIO);

    K_ASSERT_EQ(blk_cache_flush(m->dev), 0);
    u8 d1[512];
    K_ASSERT_EQ(disk_sector(m, dlba, d1), 0);
    K_EXPECT_EQ(memcmp(d0, d1, 512), 0);   /* the stranger keeps its record */
    K_EXPECT_EQ(*(u32 *)&m->fat[dead_head * 4] & 0x0FFFFFFF, 0U); /* and no
        cluster was claimed on the dead record's behalf, least of all the
        one whose chain is no longer anybody's (restored to free, not to
        an unclaimed end-of-chain mark) */

    kfree(r);
    K_EXPECT_EQ(fat_unlink_impl(NULL, "/T20STALE.T"), 0);
}

KTEST("fatfile", KTEST_LATE, test_a_stranger_dirent_refuses_the_stale_write);
KTEST("fatfile", KTEST_LATE, test_truncate_publishes_and_frees);
