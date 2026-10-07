/* T-005: a block device that says "no" must be believed.
 *
 * The defect under test (GAP_ANALYSIS C15): the FAT32 layer called its
 * sector helpers and threw the results away.  A read that failed left
 * whatever the buffer held -- uninitialised stack on the first use -- and
 * the write that followed committed it, so kernel stack bytes could end up
 * on disk as a directory entry; a write that failed still returned success
 * to user space.  Every one of those call sites now has to answer for its
 * return value, and this file is what stops that from regressing.
 *
 * A disk that fails on demand does not exist, so the block layer grew
 * blk_inject_io(): arm one (dev, lba) and every transfer covering it answers
 * LNXRM_EIO exactly as a refusing driver would -- through blk_cache_* too, so
 * a cached sector cannot hide the fault.  Nothing is written anywhere.
 *
 * Each case arms the fault, drives the operation, and asserts two things:
 * the failure reached the caller as LNXRM_EIO (never 0, EOF or ENOENT), and
 * every sector the operation could have touched is byte for byte what it
 * was.  The disk snapshots are read through blk_io_read(), i.e. straight
 * from the device, so a change the cache is still holding cannot mask one.
 *
 * The boot self-test runs before any user process exists, so there is no
 * second filesystem user to race; the cases call the *_impl entry points
 * directly (the locked wrappers in fat32.c are for tasks).  Every case must
 * leave the fault disarmed -- a forgotten arm poisons unrelated I/O. */
#include <sys/ktest.h>
#include <console.h>
#include <disk.h>
#include <fat32_priv.h>

static struct fat_mount *mnt_or_skip(const char *who)
{
    struct fat_mount *m = fat_priv;
    if (!m) {
        ktest_skip("no FAT32 volume (I/O error case skipped)");
        (void)who;
        return NULL;
    }
    return m;
}

/* Read one sector straight from the device (bypassing the cache), so a
 * before/after pair really is a before/after of the disk. */
static int disk_sector(struct fat_mount *m, u64 lba, u8 *buf)
{ return blk_io_read(m->dev, lba, 1, buf); }

/* The injection facility itself: if this does not fail transfers, nothing
 * below proves anything, so it gets its own case first. */
static void test_injection_reaches_the_device(void)
{
    struct fat_mount *m = mnt_or_skip("injection");
    if (!m) return;

    u64 lba = cluster_lba(m, m->root_cluster);
    u8 before[512], scratch[512], after[512];

    K_ASSERT_EQ(disk_sector(m, lba, before), 0);

    /* read side: refused through the raw path and through the cache */
    blk_inject_io(m->dev, lba, BLK_IO_READ);
    K_EXPECT(blk_io_read(m->dev, lba, 1, scratch) != 0);
    K_EXPECT(blk_cache_read(m->dev, lba, scratch) != 0); /* a hit must not mask it */
    K_EXPECT_EQ(blk_io_read(m->dev, lba + 1, 1, scratch), 0); /* neighbour unaffected */
    blk_inject_io_off();

    /* write side: a staged write is refused before it reaches the cache */
    blk_inject_io(m->dev, lba, BLK_IO_WRITE);
    K_EXPECT(blk_cache_write(m->dev, lba, before) != 0);
    blk_inject_io_off();

    K_EXPECT_EQ(disk_sector(m, lba, after), 0);
    K_EXPECT_EQ(memcmp(before, after, 512), 0); /* and no write slipped through */
    K_EXPECT_EQ(blk_io_read(m->dev, lba, 1, scratch), 0); /* disarmed = working */
}

/* C15 in its original form: fat_write_impl() reads a sector, merges the new
 * bytes into it and writes it back.  If the read is refused the buffer holds
 * stack garbage, and committing it would publish that as file data. */
static void test_write_refuses_an_unreadable_sector(void)
{
    struct fat_mount *m = mnt_or_skip("write");
    if (!m) return;

    struct vnode vn;
    memset(&vn, 0, sizeof(vn));
    K_ASSERT_EQ(fat_lookup_impl(NULL, "/README.md", &vn), 0);
    K_ASSERT(vn.fs_data != NULL);
    struct resolve *r = vn.fs_data;

    u32 clus = ((u32)r->de.fstclushi << 16) | r->de.fstcluslo;
    K_ASSERT(clus >= 2 && clus < m->max_cluster);
    u64 data_lba = cluster_lba(m, clus);
    u64 dir_lba = cluster_lba(m, m->root_cluster); /* where its size lives */

    u8 data0[512], dir0[512];
    K_ASSERT_EQ(disk_sector(m, data_lba, data0), 0);
    K_ASSERT_EQ(disk_sector(m, dir_lba, dir0), 0);

    static const char payload[] = "T-005 payload must never reach the disk";
    blk_inject_io(m->dev, data_lba, BLK_IO_READ | BLK_IO_WRITE);
    int rc = fat_write_impl(NULL, r, 0, payload, sizeof(payload) - 1);
    blk_inject_io_off();

    K_EXPECT_EQ(rc, LNXRM_EIO); /* refused, not "0 bytes written" */

    u8 data1[512], dir1[512];
    K_EXPECT_EQ(disk_sector(m, data_lba, data1), 0);
    K_EXPECT_EQ(disk_sector(m, dir_lba, dir1), 0);
    K_EXPECT_EQ(memcmp(data0, data1, 512), 0); /* no payload, no stack bytes */
    K_EXPECT_EQ(memcmp(dir0, dir1, 512), 0);   /* size and cluster did not move */

    /* and the file still reads back exactly what it held before */
    char back[48];
    memset(back, 0, sizeof(back));
    int n = fat_read_impl(NULL, r, 0, back, sizeof(back));
    K_EXPECT(n > 0);
    K_EXPECT(strncmp(back, "A tiny unix-like kernel", 23) == 0);

    kfree(vn.fs_data);
}

/* parent_entry_add() scans the parent for a free slot and writes it: both
 * halves are checked, and neither may report success when it did not happen
 * (the old code returned 0 for "no entry was written"). */
static void test_create_reports_the_failure(void)
{
    struct fat_mount *m = mnt_or_skip("create");
    if (!m) return;

    u64 dir_lba = cluster_lba(m, m->root_cluster);
    u8 dir0[512], dir1[512];
    K_ASSERT_EQ(disk_sector(m, dir_lba, dir0), 0);

    /* (a) the directory cannot be read: the slot must not be picked out of
     *     an unread -- i.e. uninitialised -- buffer */
    blk_inject_io(m->dev, dir_lba, BLK_IO_READ);
    int rc = fat_create_impl(NULL, "/t005a");
    blk_inject_io_off();
    K_EXPECT_EQ(rc, LNXRM_EIO);

    /* (b) reading works, writing is refused: the call must not claim the
     *     entry exists when it was never staged */
    blk_inject_io(m->dev, dir_lba, BLK_IO_WRITE);
    rc = fat_create_impl(NULL, "/t005b");
    blk_inject_io_off();
    K_EXPECT_EQ(rc, LNXRM_EIO);

    K_EXPECT_EQ(disk_sector(m, dir_lba, dir1), 0);
    K_EXPECT_EQ(memcmp(dir0, dir1, 512), 0); /* nothing was committed */

    struct resolve rr;
    int found = fat_resolve_path(m, "/t005a", &rr);
    K_EXPECT(found >= 0);   /* the volume is readable again ... */
    K_EXPECT(!rr.found);    /* ... and neither name is there */
    found = fat_resolve_path(m, "/t005b", &rr);
    K_EXPECT(found >= 0);
    K_EXPECT(!rr.found);

    /* Belt and braces: if a regression did let an entry through, take it
     * back out so the image is left exactly as it was found. */
    fat_unlink_impl(NULL, "/t005a");
    fat_unlink_impl(NULL, "/t005b");
}

/* The read half of every lookup-shaped call: an unreadable directory has to
 * surface as EIO.  Answering ENOENT/empty hides a dead disk behind "there is
 * nothing there", which is how unlink "succeeds" on a file that is still
 * present and readdir reports an empty directory that is not. */
static void test_lookup_reports_io_error_not_enoent(void)
{
    struct fat_mount *m = mnt_or_skip("lookup");
    if (!m) return;

    u64 dir_lba = cluster_lba(m, m->root_cluster);
    u8 dir0[512], dir1[512];
    K_ASSERT_EQ(disk_sector(m, dir_lba, dir0), 0);

    struct vnode root, vn;
    memset(&root, 0, sizeof(root));
    K_ASSERT_EQ(fat_lookup_impl(NULL, "/", &root), 0);
    K_ASSERT(root.fs_data != NULL);

    blk_inject_io(m->dev, dir_lba, BLK_IO_READ);

    memset(&vn, 0, sizeof(vn));
    int lk = fat_lookup_impl(NULL, "/README.md", &vn);
    int un = fat_unlink_impl(NULL, "/t005-missing"); /* name it could not read */
    int rm = fat_rmdir_impl(NULL, "/t005-missing-dir");

    struct dir_iter it = {.node = root.fs_data, .cookie = 0};
    struct dirent_out d;
    int gd = fat_dir_iter(&it, &d);

    /* The same two answers one storey up, where user space sees them: open()
     * must return EIO rather than "no such file", and a readdir must fail
     * rather than report an empty directory. */
    struct file *f = NULL;
    long o = vfs_open_file("/README.md", 0, &f);

    struct file *df = NULL;
    long od = vfs_open_file("/", 0, &df);
    long g1 = -99, g2 = -99;
    if (od == 0) {
        char db[256];
        memset(db, 0, sizeof(db));
        g1 = df->ops->getdent(df, db, sizeof(db));
        g2 = df->ops->getdent(df, db, sizeof(db)); /* still the dead scan */
    }

    blk_inject_io_off();

    K_EXPECT_EQ(lk, LNXRM_EIO);
    K_EXPECT(vn.fs_data == NULL); /* and nothing was allocated for it */
    K_EXPECT_EQ(un, LNXRM_EIO);   /* not ENOENT: the disk never answered */
    K_EXPECT_EQ(rm, LNXRM_EIO);
    K_EXPECT_EQ(gd, LNXRM_EIO);   /* not "end of directory" */

    K_EXPECT_EQ(o, LNXRM_EIO);    /* open(): not ENOENT, not EACCES */
    if (o == 0) vfs_close_file(f);
    K_EXPECT_EQ(od, 0);           /* the root itself needs no sector read */
    K_EXPECT_EQ(g1, LNXRM_EIO);   /* getdent(): not 0 = "directory is empty" */
    K_EXPECT_EQ(g2, LNXRM_EIO);
    if (od == 0) vfs_close_file(df);

    K_EXPECT_EQ(disk_sector(m, dir_lba, dir1), 0);
    K_EXPECT_EQ(memcmp(dir0, dir1, 512), 0);

    kfree(root.fs_data);
}

KTEST("blkio", KTEST_LATE, test_injection_reaches_the_device);
KTEST("blkio", KTEST_LATE, test_write_refuses_an_unreadable_sector);
KTEST("blkio", KTEST_LATE, test_create_reports_the_failure);
KTEST("blkio", KTEST_LATE, test_lookup_reports_io_error_not_enoent);
