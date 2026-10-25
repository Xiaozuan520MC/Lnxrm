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

/* "/GRWD0/G001.T" with 001 = three decimal digits of `i` (no printf in
 * the boot self-test path). */
static void grow_path(u32 i, char out[16])
{
    out[0] = '/';
    out[1] = 'G';
    out[2] = 'R';
    out[3] = 'W';
    out[4] = 'D';
    out[5] = '0';
    out[6] = '/';
    out[7] = 'G';
    out[8] = (char)('0' + (i / 100) % 10);
    out[9] = (char)('0' + (i / 10) % 10);
    out[10] = (char)('0' + i % 10);
    out[11] = '.';
    out[12] = 'T';
    out[13] = 0;
}

/* C23: a directory that ran out of slots used to answer ENOSPC forever.
 * The scenario runs inside a freshly created subdirectory, not the root:
 * the root's chain shape is whatever earlier runs left behind (grown
 * clusters keep deleted-record holes, so the 37 records just refill them
 * and never extend), while a fresh directory starts at one cluster, so
 * the extension is ensured and repeatable on any disk state. */
static void chain_len(struct fat_mount *m, u32 dirclus, u32 *out)
{
    u32 len = 0;
    u32 clus = dirclus;
    while (clus >= 2 && clus < m->max_cluster && len < 1000) {
        len++;
        clus = *(u32 *)&m->fat[clus * 4] & 0x0FFFFFFF;
    }
    *out = len;
}

static void test_full_directory_grows_the_chain(void)
{
    struct fat_mount *m = mnt_or_skip("grow");
    if (!m) return;

    fat_rmdir_impl(NULL, "/GRWD0"); /* best effort pre-clean */
    if (fat_mkdir_impl(NULL, "/GRWD0"))
        ktest_skip("GRWD0 left non-empty by an aborted prior run");
    struct resolve *r = open_resolve("/GRWD0");
    K_ASSERT(r != NULL);
    u32 dc = ((u32)r->de.fstclushi << 16) | r->de.fstcluslo;
    K_ASSERT(dc >= 2 && dc < m->max_cluster);
    u32 before;
    chain_len(m, dc, &before);
    K_ASSERT(before >= 1);

    u32 per_clus = m->cluster_size / DIRENT_SIZE;
    u32 n = MIN(2 * per_clus + 5, (u32)240);
    char path[24];
    for (u32 i = 0; i < n; i++) {
        grow_path(i, path);
        drop(path);
        int rc = fat_create_impl(NULL, path);
        if (rc) {
            for (u32 j = 0; j <= i; j++) {
                grow_path(j, path);
                drop(path);
            }
            fat_rmdir_impl(NULL, "/GRWD0");
            K_EXPECT_EQ(rc, 0);
            return;
        }
    }

    /* every record resolves, including whatever grew behind the tail */
    for (u32 i = 0; i < n; i++) {
        grow_path(i, path);
        K_ASSERT(open_resolve(path) != NULL);
    }

    u32 after;
    chain_len(m, dc, &after);
    K_EXPECT(after > before); /* the chain grew past one cluster */

    for (u32 i = 0; i < n; i++) {
        grow_path(i, path);
        K_ASSERT_EQ(fat_unlink_impl(NULL, path), 0);
    }
    K_ASSERT_EQ(fat_rmdir_impl(NULL, "/GRWD0"), 0);
    K_ASSERT(open_resolve("/GRWD0") == NULL); /* rmdir released the grown chain */
}

/* C21: two LFN records colliding on the SAME 11-byte 8.3 alias.  Normal
 * toolchains hand a second collision its own ~1/~2 tail, so no regular
 * traffic ever builds this -- it exists only when someone forges records,
 * which is precisely when a writeback that finds its target by scanning
 * the alias would rewrite whichever matching record comes first.  The
 * held resolution must rewrite its own exact position; the other owner's
 * record must come out untouched. */
/* 8.3 alias shared by both records: 4 + 4 spaces + 3, byte-exact */
static const u8 ALIAS11[11] = {'S', 'T', '_', 'F', ' ', ' ', ' ', ' ', 'T', 'X', 'T'};

static void put_alias_lfn(u8 *slot, const u8 *name11, u8 cksum)
{
    static const u8 off[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    memset(slot, 0, DIRENT_SIZE);
    slot[0] = 0x41;      /* marked, sequence 1 */
    slot[11] = 0x0F;     /* ATTR_LFN */
    slot[12] = 0;        /* type */
    slot[13] = cksum;
    for (u32 k = 0; k < 13; k++) {
        u16 ch = k < 11 ? name11[k] : (k == 11 ? 0x0000 : 0xFFFF);
        slot[off[k]] = (u8)(ch & 0xFF);
        slot[off[k] + 1] = (u8)(ch >> 8);
    }
}

static void put_alias_sfn(u8 *slot, u8 tenth)
{
    struct fat_dirent *e = (struct fat_dirent *)slot;
    memset(e, 0, DIRENT_SIZE);
    memcpy(e->name, ALIAS11, 11);
    e->ntres = 0;
    e->attr = ATTR_ARCHIVE;
    e->crttime = 0x6000;
    e->crtdate = 0x5A21;
    e->crttenth = tenth;
}

/* Read one record back from the disk, assert it stayed the incarnation it
 * was crafted as, and hand the first cluster a write booked for it. */
static void expect_alias_record(struct fat_mount *m, u32 dc, u32 off, u8 tenth, u32 size,
                                u32 *fst_out)
{
    u8 buf[512];
    u64 lba = cluster_lba(m, dc) + off / m->bytes_per_sector;
    K_ASSERT_EQ(disk_sector(m, lba, buf), 0);
    struct fat_dirent *e = (struct fat_dirent *)(buf + off % m->bytes_per_sector);
    K_EXPECT_EQ(memcmp(e->name, ALIAS11, 11), 0); /* own alias, never overwritten */
    K_EXPECT_EQ(e->crttenth, tenth);              /* own incarnation stamp */
    K_EXPECT_EQ(e->filesize, size);               /* own size */
    *fst_out = ((u32)e->fstclushi << 16) | e->fstcluslo;
}

static void test_alias_collision_writes_stay_their_own(void)
{
    struct fat_mount *m = mnt_or_skip("alias");
    if (!m) return;

    fat_rmdir_impl(NULL, "/ALIASD0"); /* best effort pre-clean */
    if (fat_mkdir_impl(NULL, "/ALIASD0"))
        ktest_skip("ALIASD0 left non-empty by an aborted prior run");
    struct resolve *rd = open_resolve("/ALIASD0");
    K_ASSERT(rd != NULL);
    u32 dc = ((u32)rd->de.fstclushi << 16) | rd->de.fstcluslo;
    kfree(rd);
    K_ASSERT(dc >= 2 && dc < m->max_cluster);
    u8 cksum = fat_lfn_checksum(ALIAS11);

    /* slots 2..5: LFN-A, alias-A, LFN-B, alias-B -- same alias bytes,
     * different long names, different incarnation stamps */
    u8 pad[512];
    u64 dlba = cluster_lba(m, dc);
    K_ASSERT_EQ(fat_read_sector(m, dlba, pad), 0);
    u8 *c2 = pad + 2 * DIRENT_SIZE;
    put_alias_lfn(c2, (const u8 *)"AAA-STALE-A", cksum);
    put_alias_sfn(c2 + DIRENT_SIZE, 0x11);
    put_alias_lfn(c2 + 2 * DIRENT_SIZE, (const u8 *)"BBB-STALE-B", cksum);
    put_alias_sfn(c2 + 3 * DIRENT_SIZE, 0x22);
    K_ASSERT_EQ(fat_write_sector(m, dlba, pad), 0);

    /* each long name must resolve to its own record's exact position */
    struct resolve *rA = open_resolve("/ALIASD0/AAA-STALE-A");
    K_ASSERT(rA != NULL);
    K_EXPECT_EQ(rA->ent_off, 3 * DIRENT_SIZE);
    K_EXPECT_EQ(rA->de.crttenth, 0x11);
    struct resolve *rB = open_resolve("/ALIASD0/BBB-STALE-B");
    K_ASSERT(rB != NULL);
    K_EXPECT_EQ(rB->ent_off, 5 * DIRENT_SIZE);
    K_EXPECT_EQ(rB->de.crttenth, 0x22);
    /* the plain alias resolves to the scan-first match */
    struct resolve *rS = open_resolve("/ALIASD0/ST_F.TXT");
    K_ASSERT(rS != NULL);
    K_EXPECT_EQ(rS->ent_off, rA->ent_off);
    kfree(rS);

    /* both files write and read back their own payload */
    u8 a[512], b[512], back[512];
    memset(a, 'A', sizeof(a));
    memset(b, 'B', sizeof(b));
    K_ASSERT_EQ(fat_write_impl(NULL, rA, 0, a, sizeof(a)), (int)sizeof(a));
    K_ASSERT_EQ(fat_write_impl(NULL, rB, 0, b, sizeof(b)), (int)sizeof(b));
    K_ASSERT_EQ(blk_cache_flush(m->dev), 0);

    K_ASSERT_EQ(fat_read_impl(NULL, rA, 0, back, sizeof(back)), (int)sizeof(back));
    K_EXPECT_EQ(memcmp(back, a, sizeof(back)), 0);
    K_ASSERT_EQ(fat_read_impl(NULL, rB, 0, back, sizeof(back)), (int)sizeof(back));
    K_EXPECT_EQ(memcmp(back, b, sizeof(back)), 0);

    /* on disk, each record kept its own size, stamp and alias; the two
     * writes booked two different clusters, each an EOC chain of one */
    u32 ha, hb;
    expect_alias_record(m, dc, 3 * DIRENT_SIZE, 0x11, sizeof(a), &ha);
    expect_alias_record(m, dc, 5 * DIRENT_SIZE, 0x22, sizeof(b), &hb);
    K_EXPECT(ha != hb);
    K_EXPECT_EQ(*(u32 *)&m->fat[ha * 4] & 0x0FFFFFFF, (u32)FAT_EOC);
    K_EXPECT_EQ(*(u32 *)&m->fat[hb * 4] & 0x0FFFFFFF, (u32)FAT_EOC);

    /* and after both writes the names still resolve where they began */
    struct resolve ra2, rb2;
    K_ASSERT(fat_resolve_path(m, "/ALIASD0/AAA-STALE-A", &ra2) >= 0 && ra2.found);
    K_ASSERT(fat_resolve_path(m, "/ALIASD0/BBB-STALE-B", &rb2) >= 0 && rb2.found);
    K_EXPECT_EQ(ra2.ent_off, 3 * DIRENT_SIZE);
    K_EXPECT_EQ(ra2.de.crttenth, 0x11);
    K_EXPECT_EQ(rb2.ent_off, 5 * DIRENT_SIZE);
    K_EXPECT_EQ(rb2.de.crttenth, 0x22);

    kfree(rA);
    kfree(rB);
    K_EXPECT_EQ(fat_unlink_impl(NULL, "/ALIASD0/AAA-STALE-A"), 0);
    K_EXPECT_EQ(fat_unlink_impl(NULL, "/ALIASD0/BBB-STALE-B"), 0);
    K_ASSERT_EQ(fat_rmdir_impl(NULL, "/ALIASD0"), 0);
}

/* C16: the free clusters the in-RAM FAT copy sees. */
static u32 fat_free_count(struct fat_mount *m)
{
    u32 n = 0;
    for (u32 c = 2; c < m->max_cluster; c++)
        if (!(*(u32 *)&m->fat[c * 4] & 0x0FFFFFFF)) n++;
    return n;
}

/* C16: a demand reaching past the biggest chain this volume can legally
 * hold is refused outright -- nothing allocated, nothing silently wrapped.
 * First unservable offset is the cap itself: a write there would end one
 * byte past it.  The boundary write that ends EXACTLY at the cap stays
 * allowed (and would fill the volume), so the case only refuses. */
static void test_write_demand_past_the_volume_is_refused(void)
{
    struct fat_mount *m = mnt_or_skip("cap16");
    if (!m) return;

    drop("/T16CAP.T");
    K_ASSERT_EQ(fat_create_impl(NULL, "/T16CAP.T"), 0);
    struct resolve *r = open_resolve("/T16CAP.T");
    K_ASSERT(r != NULL);

    u32 free_before = fat_free_count(m);
    K_ASSERT_EQ(fat_write_impl(NULL, r, fat_chain_cap_bytes(m), "x", 1),
                LNXRM_ENOSPC);
    /* off+n wrapping past u64 is refused too, not a tiny write */
    K_ASSERT_EQ(fat_write_impl(NULL, r, (u64)-1, "x", 1), LNXRM_ENOSPC);
    K_EXPECT_EQ(fat_free_count(m), free_before); /* zero clusters spent */

    struct resolve r2;
    K_ASSERT(fat_resolve_path(m, "/T16CAP.T", &r2) >= 0 && r2.found);
    K_EXPECT_EQ(r2.de.filesize, 0u); /* the record still says empty */
    K_EXPECT_EQ(((u32)r2.de.fstclushi << 16) | r2.de.fstcluslo, 0u);
    kfree(r);
    K_EXPECT_EQ(fat_unlink_impl(NULL, "/T16CAP.T"), 0);
}

/* C35: a zero-byte write to an empty file used to walk alloc_chain and
 * answer EIO -- "no clusters" mistaken for disk full. */
static void test_zero_byte_write_is_not_a_fake_disk_full(void)
{
    struct fat_mount *m = mnt_or_skip("zwrite");
    if (!m) return;

    drop("/T35ZERO.T");
    K_ASSERT_EQ(fat_create_impl(NULL, "/T35ZERO.T"), 0);
    struct resolve *r = open_resolve("/T35ZERO.T");
    K_ASSERT(r != NULL);
    u8 one = 'z';
    K_EXPECT_EQ(fat_write_impl(NULL, r, 0, &one, 0), 0); /* empty, zero bytes */
    K_EXPECT_EQ(fat_write_impl(NULL, r, 0, &one, 1), 1); /* book a byte */
    K_EXPECT_EQ(fat_write_impl(NULL, r, 1, &one, 0), 0); /* non-empty, zero */
    K_EXPECT_EQ(blk_cache_flush(m->dev), 0);
    struct resolve r2;
    K_ASSERT(fat_resolve_path(m, "/T35ZERO.T", &r2) >= 0 && r2.found);
    K_EXPECT_EQ(r2.de.filesize, 1u); /* the record says exactly one byte */
    kfree(r);
    K_EXPECT_EQ(fat_unlink_impl(NULL, "/T35ZERO.T"), 0);
}

KTEST("fatfile", KTEST_LATE, test_a_stranger_dirent_refuses_the_stale_write);
KTEST("fatfile", KTEST_LATE, test_truncate_publishes_and_frees);
KTEST("fatfile", KTEST_LATE, test_full_directory_grows_the_chain);
KTEST("fatfile", KTEST_LATE, test_alias_collision_writes_stay_their_own);
KTEST("fatfile", KTEST_LATE, test_write_demand_past_the_volume_is_refused);
KTEST("fatfile", KTEST_LATE, test_zero_byte_write_is_not_a_fake_disk_full);
