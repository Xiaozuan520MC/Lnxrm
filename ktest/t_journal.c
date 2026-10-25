/* The FAT32 journal, proved rather than asserted (defect C13).
 *
 * The driver's original "journal" was a lie: after-images that only ever
 * lived in RAM, an abort that rolled nothing back, a commit that re-wrote
 * sectors that had already been written, no log on disk and no replay.  It
 * was deleted, and GAP_ANALYSIS kept the slot open with one requirement --
 * "either really implement the log (on-disk + replay), or stop calling it
 * one".  This file is the "really implement" half made checkable.
 *
 * Three properties, each driven through the real entry points:
 *
 *   1. Write-ahead.  While a transaction is open, none of its metadata has
 *      reached its home location.  This is what makes a crash before the
 *      commit harmless -- the operation simply never happened.
 *   2. Replay.  Kill the apply step after the commit is durable (the exact
 *      window a power cut finds) and the log still finishes the job on the
 *      next recovery pass, byte for byte, and then reports itself clean.
 *   3. No log, no trace.  If the *log* refuses the records, the commit never
 *      exists, so recovery has nothing to replay and the operation is
 *      nowhere -- not half-applied, not "mostly there".
 *
 * Faults come from blk_inject_io(), the same facility t_ioerr.c uses: arm one
 * (dev, lba) and every transfer covering it answers LNXRM_EIO exactly as a
 * dying disk would, through the cache too.  Recovery is exercised by calling
 * fat_journal_mount() again on the live mount -- it is idempotent for a clean
 * log and re-runs the replay for a committed one, which is precisely what the
 * next boot does.
 *
 * Like t_ioerr.c, these cases call the *_impl entry points directly (the
 * locked wrappers in fat32.c are for tasks), so they open and close the
 * transaction themselves.  Every case must leave the fault disarmed and the
 * transaction closed. */
#include <sys/ktest.h>
#include <console.h>
#include <disk.h>
#include <fat32_priv.h>

static const char NAME1[] = "JRNTEST1   "; /* 8.3 image of "/jrntest1" */
static const char NAME2[] = "JRNTEST2   ";
static const char NAME3[] = "JRNTEST3   ";

static struct fat_mount *mnt_or_skip(const char *who)
{
    struct fat_mount *m = fat_priv;
    if (!m) {
        ktest_skip("no FAT32 volume (journal case skipped)");
        (void)who;
        return NULL;
    }
    if (!m->j_active) {
        ktest_skip("volume has no journal (journal case skipped)");
        return NULL;
    }
    return m;
}

/* One sector straight off the device, so "on disk" never means "in the
 * cache".  The write-ahead and replay claims are claims about the disk. */
static int disk_sector(struct fat_mount *m, u64 lba, u8 *buf)
{ return blk_io_read(m->dev, lba, 1, buf); }

/* Is the 8.3 name present in the root directory *as stored on the device*? */
static bool root_has(struct fat_mount *m, const char *name11)
{
    u8 sec[CACHE_SECTOR_SIZE];
    if (disk_sector(m, cluster_lba(m, m->root_cluster), sec)) return false;
    for (u32 off = 0; off + 11 <= sizeof(sec); off += 32)
        if (memcmp(sec + off, name11, 11) == 0) return true;
    return false;
}

/* The journal superblock's state word, read from the device. */
static int journal_state(struct fat_mount *m, u32 *state)
{
    u8 sec[CACHE_SECTOR_SIZE];
    struct jrn_super s;
    if (disk_sector(m, m->j_start, sec)) return -1;
    memcpy(&s, sec, sizeof(s));
    if (s.magic != JRN_MAGIC || s.version != JRN_VERSION) return -1;
    *state = s.state;
    return 0;
}

/* Property 1 + 2: the records wait in the log, and when the apply step is
 * refused the commit survives so recovery can finish it. */
static void test_journal_write_ahead_and_replay(void)
{
    struct fat_mount *m = mnt_or_skip("replay");
    if (!m) return;
    u64 dir_lba = cluster_lba(m, m->root_cluster);
    u32 st = 0xFFFFFFFF;

    K_EXPECT(!root_has(m, NAME1));

    K_ASSERT_EQ(fat_journal_begin(m), 0);
    K_ASSERT_EQ(fat_create_impl(NULL, "/jrntest1"), 0);

    /* Write-ahead: the directory entry exists only in the log buffer. */
    K_EXPECT(!root_has(m, NAME1));

    /* Refuse the home write.  The records and the commit land (they live in
     * the reserved area, untouched by this fault); only the apply dies. */
    blk_inject_io(m->dev, dir_lba, BLK_IO_WRITE);
    int crc = fat_journal_commit(m);
    blk_inject_io_off();

    K_EXPECT_EQ(crc, LNXRM_EIO); /* the caller is told the op did not finish */
    K_EXPECT(!root_has(m, NAME1)); /* and the entry really is not home yet */

    /* The log says: committed, replay owed.  This is the power-cut window. */
    K_ASSERT_EQ(journal_state(m, &st), 0);
    K_EXPECT_EQ(st, JRN_STATE_COMMIT);

    /* What the next boot does: run recovery again. */
    fat_journal_mount(m);

    K_ASSERT_EQ(journal_state(m, &st), 0);
    K_EXPECT_EQ(st, JRN_STATE_CLEAN); /* replay completed and cleared itself */
    K_EXPECT(root_has(m, NAME1));     /* ... and the entry is now home */

    /* The volume still resolves it through the ordinary path. */
    struct resolve rr;
    K_EXPECT(fat_resolve_path(m, "/jrntest1", &rr) >= 0);
    K_EXPECT(rr.found);

    /* tidy up for the image the smoke test fscks afterwards */
    K_ASSERT_EQ(fat_journal_begin(m), 0);
    K_EXPECT_EQ(fat_unlink_impl(NULL, "/jrntest1"), 0);
    K_EXPECT_EQ(fat_journal_commit(m), 0);
    K_EXPECT(!root_has(m, NAME1));

    /* One line the smoke test greps for, so "the journal tests ran" is an
     * observed fact in the transcript rather than an inference from a
     * passing tally.  It prints last: a failure anywhere above lands in the
     * tally, which is checked separately. */
    kprintf("[fat32] C13 journal: write-ahead + replay verified\n");
}

/* Property 3: when the log itself refuses the records there is no commit, so
 * the operation must be nowhere at all -- and recovery must not invent one. */
static void test_journal_log_failure_leaves_no_trace(void)
{
    struct fat_mount *m = mnt_or_skip("logfail");
    if (!m) return;
    u64 log_lba = m->j_start + 1; /* first record sector */
    u32 st = 0xFFFFFFFF;

    K_ASSERT_EQ(fat_journal_begin(m), 0);
    K_ASSERT_EQ(fat_create_impl(NULL, "/jrntest2"), 0);

    blk_inject_io(m->dev, log_lba, BLK_IO_WRITE);
    int crc = fat_journal_commit(m);
    blk_inject_io_off();

    K_EXPECT_EQ(crc, LNXRM_EIO);
    K_EXPECT(!root_has(m, NAME2)); /* never staged home ... */

    K_ASSERT_EQ(journal_state(m, &st), 0);
    K_EXPECT_EQ(st, JRN_STATE_CLEAN); /* ... and never committed */

    fat_journal_mount(m); /* recovery must not find work that was not done */
    K_EXPECT(!root_has(m, NAME2));
    K_EXPECT_EQ(journal_state(m, &st), 0);
    K_EXPECT_EQ(st, JRN_STATE_CLEAN);
}

/* The ordinary path: a transaction that commits cleanly applies, clears, and
 * survives a second recovery pass without being replayed into oblivion. */
static void test_journal_normal_commit_is_durable(void)
{
    struct fat_mount *m = mnt_or_skip("clean");
    if (!m) return;
    u32 st = 0xFFFFFFFF;

    K_ASSERT_EQ(fat_journal_begin(m), 0);
    K_ASSERT_EQ(fat_create_impl(NULL, "/jrntest3"), 0);
    K_ASSERT_EQ(fat_journal_commit(m), 0);

    K_EXPECT(root_has(m, NAME3));        /* applied home ... */
    K_ASSERT_EQ(journal_state(m, &st), 0);
    K_EXPECT_EQ(st, JRN_STATE_CLEAN);    /* ... and marked done */

    /* Re-running recovery is a no-op for a clean log, never a rollback. */
    fat_journal_mount(m);
    K_EXPECT(root_has(m, NAME3));
    K_ASSERT_EQ(journal_state(m, &st), 0);
    K_EXPECT_EQ(st, JRN_STATE_CLEAN);

    K_ASSERT_EQ(fat_journal_begin(m), 0);
    K_EXPECT_EQ(fat_unlink_impl(NULL, "/jrntest3"), 0);
    K_EXPECT_EQ(fat_journal_commit(m), 0);
    K_EXPECT(!root_has(m, NAME3));
}

KTEST("jrn", KTEST_LATE, test_journal_write_ahead_and_replay);
KTEST("jrn", KTEST_LATE, test_journal_log_failure_leaves_no_trace);
KTEST("jrn", KTEST_LATE, test_journal_normal_commit_is_durable);
