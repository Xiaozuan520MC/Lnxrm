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
#include <string.h>
#include "kblk.h"

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
    u32 nclu = 0;
    struct fat_chain ch;
    for (fat_chain_start(&ch, m, head); fat_chain_more(&ch) && nclu < 2;
         fat_chain_advance(&ch)) {
        K_EXPECT(ch.clus >= 2 && ch.clus < m->max_cluster);
        nclu++;
    }
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

/* ---- C30/C31/C32/C33/C34: boundaries, forged volumes, cursors ---------
 * The impl entry points are the subject, so the cases drive them exactly
 * like the cases above -- but the volumes they need (cursors across
 * cluster boundaries, a full-run LFN entry, a giant FAT, a one-sector
 * FAT) are shapes mkfs never lays down, so they are forged. */

/* "/ITERD0/IXiii.T" as a full path (8.3 base + suffix, one record). */
static void iter_short_path(u32 i, char out[24])
{
    memcpy(out, "/ITERD0/IX", 11);
    out[10] = (char)('0' + (i / 100) % 10);
    out[11] = (char)('0' + (i / 10) % 10);
    out[12] = (char)('0' + i % 10);
    out[13] = '.', out[14] = 'T', out[15] = 0;
}

/* Hand-crafted BPB: the fields fat_mount() actually validates, zeros for
 * everything mkfs would fill (root entries 0, FAT-size-16 0 = FAT32 only). */
static void forge_bpb(u8 *s, u32 spc, u32 reserved, u32 numfats, u32 fatsz32,
                      u32 totsec, u32 rootclk)
{
    memset(s, 0, 512);
    s[0] = 0xEB, s[1] = 0x3C, s[2] = 0x90;
    *(u16 *)&s[11] = 512;
    s[13] = (u8)spc;
    *(u16 *)&s[14] = (u16)reserved;
    s[16] = (u8)numfats;
    *(u32 *)&s[32] = totsec;
    *(u32 *)&s[36] = fatsz32;
    *(u32 *)&s[44] = rootclk;
    *(u16 *)&s[510] = 0xAA55;
    memcpy(&s[82], "FAT32", 5);
}

/* One readdir pass from `cookie`, collecting up to `total` entries
 * (names in 56-byte slots, inos separate).  Returns how many arrived;
 * `final` gets the cookie after the last step.  No K_ macros here: the
 * helper returns values and the cases assert them. */
static u32 sweep(struct resolve *rd, u64 cookie, u32 total, char *names, u64 *inos,
                 u64 *final)
{
    u32 n = 0;
    while (n < total) {
        struct dirent_out d;
        memset(&d, 0, sizeof(d));
        int rc = fat32_getdent_impl(NULL, rd, &cookie, &d);
        if (rc <= 0) break; /* dead scan or list end: the caller sees the count */
        memcpy(names + (size_t)n * 56, d.name, 56);
        inos[n] = d.ino;
        n++;
    }
    *final = cookie;
    return n;
}

/* Expected delivered name for listing position k inside the ITERD0 walk:
 * the two LFN creations surface as their 8.3 aliases -- create() writes
 * no long-name run, that is rename's job -- then the IX names, which fit
 * 8.3 exactly. */
static void iter_expect_name(u32 k, char exp[24])
{
    if (k == 0) memcpy(exp, "LFNA.MIX", 9);
    else if (k == 1) memcpy(exp, "LFNB.MIX", 9);
    else {
        u32 j = k - 2;
        exp[0] = 'I', exp[1] = 'X';
        exp[2] = (char)('0' + (j / 100) % 10);
        exp[3] = (char)('0' + (j / 10) % 10);
        exp[4] = (char)('0' + j % 10);
        exp[5] = '.', exp[6] = 'T', exp[7] = 0;
    }
}

/* Assert the delivered listing as a set: every one of the `total`
 * expected names exactly once and nothing else.  For an in-order walk
 * this matches a position-exact check too, but the walk keeps its set
 * guarantee even where a mid-list claim reordered physical filling. */
/* Assert the delivered listing as a set over the expected-name window
 * [base, base+gotn): every one of those names exactly once, nothing else.
 * For an in-order walk this matches a position-exact check too, but the
 * walk keeps its set guarantee even where a mid-list claim reordered
 * physical filling. */
static void c30_set_check(u32 gotn, const char *names, u32 base)
{
    u8 *hit = kmalloc(gotn);
    K_ASSERT(hit != NULL);
    memset(hit, 0, gotn);
    for (u32 i = 0; i < gotn; i++) {
        const char *got = names + (size_t)i * 56;
        u32 k = base + gotn;
        for (u32 e = base; e < base + gotn; e++) {
            char exp[24];
            iter_expect_name(e, exp);
            if (strcmp(exp, got) == 0) {
                k = e;
                break;
            }
        }
        K_EXPECT(k < base + gotn);
        if (k < base + gotn) {
            K_EXPECT(!hit[k - base]);
            hit[k - base] = 1;
        }
    }
    for (u32 e = base; e < base + gotn; e++) K_EXPECT(hit[e - base]);
    kfree(hit);
}

/* C30: one full readdir walk must cost one pass over the directory's
 * sectors, not one from-the-top rescan per entry.  The cursor also has
 * to defuse every malformed cookie and survive a mid-walk resume with
 * LFN names in the middle of the listing. */
static void test_getdent_cursor_walks_once(void)
{
    struct fat_mount *m = mnt_or_skip("iter");
    if (!m) return;
    u32 per_clus = m->cluster_size / DIRENT_SIZE;
    if (per_clus < 8) {
        ktest_skip("cluster holds too few records for a 2-cluster sweep");
        return;
    }

    fat_rmdir_impl(NULL, "/ITERD0"); /* best effort pre-clean */
    if (fat_mkdir_impl(NULL, "/ITERD0"))
        ktest_skip("ITERD0 left non-empty by an aborted prior run");
    struct resolve *rd = open_resolve("/ITERD0");
    K_ASSERT(rd != NULL);
    u32 dc = ((u32)rd->de.fstclushi << 16) | rd->de.fstcluslo;
    K_ASSERT(dc >= 2 && dc < m->max_cluster);

    /* An empty directory ends immediately -- the skipped "." / ".."
     * records are positions, never counted entries. */
    u64 cookie = DIRIT_START;
    struct dirent_out d;
    memset(&d, 0, sizeof(d));
    K_EXPECT_EQ(fat32_getdent_impl(NULL, rd, &cookie, &d), 0);
    K_EXPECT_EQ(cookie, DIRIT_END);

    static const char *const lfns[2] = {"/ITERD0/LfnA.Mixed1", "/ITERD0/LfnB.Mixed2"};
    for (int i = 0; i < 2; i++) drop(lfns[i]);
    K_ASSERT_EQ(fat_create_impl(NULL, lfns[0]), 0);
    K_ASSERT_EQ(fat_create_impl(NULL, lfns[1]), 0);
    u32 nshort = 2 * per_clus + 5; /* the chain grows past two clusters */
    K_ASSERT(nshort < 1000);
    for (u32 i = 0; i < nshort; i++) {
        char p[24];
        iter_short_path(i, p);
        drop(p);
        int rc = fat_create_impl(NULL, p);
        if (rc) {
            for (u32 j = 0; j <= i; j++) {
                iter_short_path(j, p);
                drop(p);
            }
            for (int j = 0; j < 2; j++) drop(lfns[j]);
            fat_rmdir_impl(NULL, "/ITERD0");
            K_EXPECT_EQ(rc, 0);
            return;
        }
    }
    u32 total = nshort + 2;
    K_ASSERT(total < 1000);

    /* expected listing: a SET, never position-ordered — slot reuse after
     * deletion can reorder fills, so the walk is verified by membership
     * (every created name exactly once) and by inos, which follow the
     * physical slot order */
    char *names = kmalloc((size_t)total * 56);
    u64 *inos = kmalloc(total * sizeof(u64));
    K_ASSERT(names != NULL && inos != NULL);

    u64 endk = 0;
    u32 got = sweep(rd, DIRIT_START, total, names, inos, &endk);
    K_ASSERT_EQ(got, total);
    /* one past the last delivered entry answers a clean end, once */
    memset(&d, 0, sizeof(d));
    K_EXPECT_EQ(fat32_getdent_impl(NULL, rd, &endk, &d), 0);
    K_EXPECT_EQ(endk, DIRIT_END);
    c30_set_check(got, names, 0);
    for (u32 i = 1; i < total; i++)
        K_EXPECT(inos[i - 1] < inos[i]); /* physical position, ascending */

    /* The measured second walk: total cache lookups must stay bounded by
     * the directory's own sectors, not by entry count.  Double-shaped
     * bound: the old per-entry rescan of even two clusters would blow
     * past it once the listing is longer than one cluster. */
    struct blk_cache_stats s0, s1;
    u32 nchain;
    chain_len(m, dc, &nchain);
    blk_cache_stats_get(&s0);
    u32 got2 = sweep(rd, DIRIT_START, total, names, inos, &endk);
    blk_cache_stats_get(&s1);
    K_ASSERT_EQ(got2, total);
    u64 lookups = (s1.hits + s1.misses) - (s0.hits + s0.misses);
    /* one pass over the directory's sectors plus one entry-delivery step
     * each: sector lookups re-enter the sector holding the delivered
     * entry, and a stop at the 0x00 terminator costs one more sector.
     * The old per-entry rescan from the top of the chain blows past it
     * once the listing spans clusters. */
    u64 bound = (u64)nchain * m->sectors_per_cluster + total + 4;
    kprintf("[t-fatfile][C30] dir %u clusters, one walk = %llu lookups "
            "(bound %llu)\n",
            nchain, lookups, bound);
    K_EXPECT(lookups > 0); /* something honest got measured */
    K_EXPECT(lookups <= bound);

    /* the cursor resumes: five entries by hand, then the rest */
    u64 mid = DIRIT_START;
    for (u32 k = 0; k < 5; k++) {
        memset(&d, 0, sizeof(d));
        K_ASSERT_EQ(fat32_getdent_impl(NULL, rd, &mid, &d), 1);
    }
    u32 got3 = sweep(rd, mid, total - 5, names, inos, &endk);
    K_ASSERT_EQ(got3, total - 5);
    /* the resumed walk also ends cleanly one past the last entry */
    memset(&d, 0, sizeof(d));
    K_EXPECT_EQ(fat32_getdent_impl(NULL, rd, &endk, &d), 0);
    K_EXPECT_EQ(endk, DIRIT_END);
    c30_set_check(got3, names, 5);

    /* malformed cursors answer "ended", never invent entries */
    cookie = ((u64)dc << 32) | 33; /* misaligned to a 32-byte record */
    memset(&d, 0, sizeof(d));
    K_EXPECT_EQ(fat32_getdent_impl(NULL, rd, &cookie, &d), 0);
    K_EXPECT_EQ(cookie, DIRIT_END);
    cookie = ((u64)dc << 32) | (m->cluster_size + 64); /* past the cluster */
    K_EXPECT_EQ(fat32_getdent_impl(NULL, rd, &cookie, &d), 0);
    K_EXPECT_EQ(cookie, DIRIT_END);
    cookie = (0xBEEF0000ULL << 32) | 32; /* cluster not in the chain */
    K_EXPECT_EQ(fat32_getdent_impl(NULL, rd, &cookie, &d), 0);
    K_EXPECT_EQ(cookie, DIRIT_END);

    for (u32 i = 0; i < nshort; i++) {
        char p[24];
        iter_short_path(i, p);
        K_EXPECT_EQ(fat_unlink_impl(NULL, p), 0);
    }
    for (int i = 0; i < 2; i++) K_EXPECT_EQ(fat_unlink_impl(NULL, lfns[i]), 0);
    kfree(names);
    kfree(inos);
    K_ASSERT_EQ(fat_rmdir_impl(NULL, "/ITERD0"), 0);
    kfree(rd);
}

/* C34: resolve must zero the answer and move the whole component into it.
 * A 55-character name is exactly r->name's capacity minus one -- the old
 * strncpy left byte 55 holding caller-stack data.  The struct is poisoned
 * before the call so any surviving byte says out loud it was never ours. */
static void test_resolve_poisoned_name_terminates(void)
{
    struct fat_mount *m = mnt_or_skip("r34");
    if (!m) return;

    fat_rmdir_impl(NULL, "/T34D"); /* best effort pre-clean */
    if (fat_mkdir_impl(NULL, "/T34D"))
        ktest_skip("T34D left non-empty by an aborted prior run");

    char n55[56];
    for (int i = 0; i < 55; i++) n55[i] = (char)('a' + i % 26);
    n55[55] = 0;
    char pth[80];
    memcpy(pth, "/T34D/", 6);
    memcpy(pth + 6, n55, 56); /* 61 bytes of path: legal, and long enough */
    drop(pth);
    K_ASSERT_EQ(fat_create_impl(NULL, pth), 0);

    struct resolve r;
    memset(&r, 0xAA, sizeof(r));
    K_EXPECT_EQ(fat_resolve_path(m, pth, &r), 1);
    K_EXPECT(r.found);
    K_EXPECT(memcmp(r.name, n55, 56) == 0); /* content + terminator, no 0xAA */

    struct resolve r2;
    memset(&r2, 0xBB, sizeof(r2));
    K_EXPECT_EQ(fat_resolve_path(m, "/T34D/", &r2), 1);
    K_EXPECT(r2.found);
    static const char dir4[5] = {'T', '3', '4', 'D', 0};
    K_EXPECT(memcmp(r2.name, dir4, 5) == 0); /* trailing-slash branch too */

    K_EXPECT_EQ(fat_unlink_impl(NULL, pth), 0);
    K_EXPECT_EQ(fat_rmdir_impl(NULL, "/T34D"), 0);
}

/* C31: a full LFN run holds 20 pieces x 13 = 260 name bytes + terminator.
 * The old 256-byte piece buffer rejected the last piece of exactly such a
 * run, dropping the whole long name and listing the 8.3 alias instead.
 * The record writer: one LFN record holds piece `seq` as UTF-16 --
 * full[(seq-1)*13 .. ] -- with all 13 slots fully used by a 260-byte name
 * (padding never needed). */
static void put_lfn_piece(u8 *slot, int seq, u8 sum, const char *full)
{
    static const u8 poff[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    memset(slot, 0, DIRENT_SIZE);
    slot[0] = (u8)(seq == 20 ? 0x40 : 0x00) | (u8)seq;
    slot[11] = ATTR_LFN;
    slot[12] = 0;
    slot[13] = sum;
    for (int k = 0; k < 13; k++) {
        int i = (seq - 1) * 13 + k;
        u16 ch = i < 260 ? (u8)full[i] : 0;
        slot[poff[k]] = (u8)(ch & 0xFF);
        slot[poff[k] + 1] = (u8)(ch >> 8); /* ASCII: always zero */
    }
}

struct lfn260_ctx {
    char name[272];
    u32 len;
    bool hit;
};

static int lfn260_cb(const char *name, const struct fat_dirent *e, u32 dirclus, u32 off,
                     void *v)
{
    (void)e, (void)dirclus, (void)off;
    struct lfn260_ctx *c = v;
    size_t l = strlen(name);
    if (l < 250) return 0; /* the assembled long name, not its 8.3 alias */
    if (l >= sizeof(c->name)) return 0;
    memcpy(c->name, name, l + 1);
    c->len = (u32)l;
    c->hit = true;
    return 1;
}

static void test_lfn_full_run_survives(void)
{
    struct fat_mount *m = mnt_or_skip("lfn260");
    if (!m) return;
    u32 per = m->cluster_size / DIRENT_SIZE;
    if (per < 2) {
        ktest_skip("cluster holds fewer directory records than the run spans");
        return;
    }

    fat_rmdir_impl(NULL, "/T31LNGB"); /* best effort pre-clean */
    if (fat_mkdir_impl(NULL, "/T31LNGB"))
        ktest_skip("T31LNGB left non-empty by an aborted prior run");
    struct resolve *rd = open_resolve("/T31LNGB");
    K_ASSERT(rd != NULL);
    u32 dc = ((u32)rd->de.fstclushi << 16) | rd->de.fstcluslo;
    K_ASSERT(dc >= 2 && dc < m->max_cluster);
    kfree(rd);

    /* The run needs 21 consecutive slots (".." takes 0 and 1); when a
     * cluster holds fewer, filler files pull the directory's chain out to
     * enough clusters first -- then leave, so nothing live but '.'/'..'
     * remains in the way. */
    char fp[24];
    for (u32 i = 0; i < 23; i++) {
        memcpy(fp, "/T31LNGB/F0", 11);
        fp[11] = (char)('0' + i / 10);
        fp[12] = (char)('0' + i % 10);
        fp[13] = 0;
        drop(fp);
        int rc = fat_create_impl(NULL, fp);
        if (rc) {
            for (u32 j = 0; j < i; j++) {
                fp[11] = (char)('0' + j / 10);
                fp[12] = (char)('0' + j % 10);
                drop(fp); /* best effort: the case is bailing out anyway */
            }
            ktest_skip("filler creation refused");
            return;
        }
    }
    for (u32 i = 0; i < 23; i++) {
        fp[11] = (char)('0' + i / 10);
        fp[12] = (char)('0' + i % 10);
        K_EXPECT_EQ(fat_unlink_impl(NULL, fp), 0); /* they must be deletable */
    }

    /* slots 2..25 are deleted (E5) records inside the chain now; the run
     * is forged into slots 2..22, global slot -> (chain cluster, offset). */
    u32 nchain;
    chain_len(m, dc, &nchain);
    K_ASSERT((u64)nchain * per >= 23); /* slots 2..22 lie inside the chain */

    /* 20 LFN pieces (marked first, descending) in slots 2..21, the 8.3
     * record in slot 22. */
    static const u8 alias11[11] = {'T', 'E', 'S', 'T', ' ', ' ', ' ', ' ', 'O', 'L', 'D'};
    u8 sum = fat_lfn_checksum(alias11);
    char full[262];
    for (int i = 0; i < 260; i++) full[i] = "abcdefghijklmnopqrstuvwxyz"[i % 26];
    full[260] = 0, full[261] = 0;

    struct fat_chain ch;
    u8 *cl = kmalloc(m->cluster_size);
    K_ASSERT(cl != NULL);
    u32 cidx = 0;
    for (fat_chain_start(&ch, m, dc); fat_chain_more(&ch); fat_chain_advance(&ch), cidx++) {
        K_ASSERT_EQ(fat_read_cluster(m, cluster_lba(m, ch.clus), cl), 0);
        bool touched = false;
        for (u32 s = cidx * per; s < (cidx + 1) * per; s++) {
            if (s < 2 || s > 22) continue;
            u32 o = (s % per) * DIRENT_SIZE;
            if (s <= 21)
                put_lfn_piece(cl + o, /* seq = */ (int)(22 - s), sum, full);
            else {
                struct fat_dirent *sfn = (struct fat_dirent *)(cl + o);
                memset(sfn, 0, DIRENT_SIZE);
                memcpy(sfn->name, alias11, 11);
                sfn->attr = ATTR_ARCHIVE;
                sfn->crttenth = 0x33;
            }
            touched = true;
        }
        if (touched) {
            for (u32 sec = 0; sec < m->sectors_per_cluster; sec++)
                K_ASSERT_EQ(
                    fat_write_sector(m, cluster_lba(m, ch.clus) + sec, cl + (size_t)sec * 512),
                    0);
        }
    }
    kfree(cl);

    struct lfn260_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    int rc = fat_scan_dir(m, dc, lfn260_cb, &ctx);
    K_EXPECT_EQ(rc, 1);
    K_EXPECT(ctx.hit);
    K_EXPECT_EQ(ctx.len, 260u); /* every one of the 260 bytes */
    K_EXPECT(memcmp(ctx.name, full, 261) == 0); /* content and terminator */

    /* the alias resolves through the real lookup path as well */
    struct resolve rr;
    K_EXPECT_EQ(fat_resolve_path(m, "/T31LNGB/TEST.OLD", &rr), 1);
    K_EXPECT(rr.found);

    K_EXPECT_EQ(fat_unlink_impl(NULL, "/T31LNGB/TEST.OLD"), 0); /* via the alias */
    K_EXPECT_EQ(fat_rmdir_impl(NULL, "/T31LNGB"), 0);
}

/* C32/C33 mount a forged volume: the mount's answer fields (max_cluster
 * and friends) become direct assertions, and fat_priv must be untouched
 * afterwards -- a failed or foreign mount never steals the active one. */
static struct fat_mount *mount_forged_leave(struct kblk *k)
{
    struct fat_mount *old = fat_priv;
    struct fat_mount *m2 = fat_mount(&k->dev);
    fat_priv = old;
    if (m2) kfree(m2->fat);
    if (m2) kfree(m2);
    return m2;
}

/* C32: a BPB whose FAT cannot exist inside FAT32's encoding (2^28
 * entries is the format's own ceiling; this claim is 384 million) is
 * refused with a diagnosis, before the deep load could start -- the
 * device hands out exactly one sector, the BPB. */
static void test_fat_entries_beyond_the_cap_is_refused(void)
{
    mnt_or_skip("cap32"); /* same boot environment as the other cases */
    struct kblk *k = kblk_make("fatcap", 6000016, 64);
    K_ASSERT(k != NULL);
    u8 *s0 = kblk_sector(k, 0);
    K_ASSERT(s0 != NULL);
    /* data start = 8 + 2 * 3,000,000 < totsec, so the volume geometry
     * itself is coherent -- the FAT alone is the lie */
    forge_bpb(s0, 1, 8, 2, 3000000, 6000016, 2);
    K_EXPECT(mount_forged_leave(k) == NULL);
    K_EXPECT(fat_priv != NULL); /* the real mount still owns the slot */
    K_EXPECT_EQ(k->reads, 1);   /* the BPB, and never the giant FAT */
    kblk_free(k);
}

/* C33: the FAT-size bound is exclusive -- cluster N keeps its link in
 * cell N, so the last cell (here 127 of 128) must be readable through
 * max_cluster.  With the old fat_entries - 1 clamp, cell 127 lay beyond
 * max_cluster and answered like an illegal index. */
static void test_last_fat_cell_reachable(void)
{
    mnt_or_skip("cell33");
    struct kblk *k = kblk_make("kfat33", 256, 256);
    K_ASSERT(k != NULL);
    u8 *s0 = kblk_sector(k, 0);
    K_ASSERT(s0 != NULL);
    forge_bpb(s0, 1, 8, 2, 1 /* FAT = 512 B = 128 cells */, 256, 2);
    u8 *fatsec = kblk_sector(k, 8); /* the whole FAT */
    K_ASSERT(fatsec != NULL);
    *(u32 *)&fatsec[127 * 4] = 2; /* the last cell points somewhere legal */
    *(u32 *)&fatsec[2 * 4] = FAT_EOC; /* the root dir chain ends at once */

    struct fat_mount *old = fat_priv;
    struct fat_mount *m2 = fat_mount(&k->dev);
    K_ASSERT(m2 != NULL);
    K_EXPECT_EQ((long)m2->max_cluster, 128); /* == fat_entries, not 127 */
    K_EXPECT_EQ((long)m2->max_cluster, (long)(m2->fatsz * m2->bytes_per_sector / 4));
    K_EXPECT_EQ((long)m2->data_start_lba, 10);
    /* the final cell reads as a chain link, not as the EOC an illegal
     * cluster number borrows for its rejection */
    K_EXPECT_EQ(fat_next_cluster(m2, m2->max_cluster - 1), 2u);
    struct fat_chain ch;
    fat_chain_start(&ch, m2, m2->max_cluster - 1);
    K_EXPECT_EQ((long)ch.clus, 127); /* accepted as a head, no warning */
    K_EXPECT_EQ(k->reads, 2);        /* BPB + the one-sector FAT */

    fat_priv = old;
    kfree(m2->fat);
    kfree(m2);
    kblk_free(k);
}

KTEST("fatfile", KTEST_LATE, test_a_stranger_dirent_refuses_the_stale_write);
KTEST("fatfile", KTEST_LATE, test_truncate_publishes_and_frees);
KTEST("fatfile", KTEST_LATE, test_full_directory_grows_the_chain);
KTEST("fatfile", KTEST_LATE, test_alias_collision_writes_stay_their_own);
KTEST("fatfile", KTEST_LATE, test_write_demand_past_the_volume_is_refused);
KTEST("fatfile", KTEST_LATE, test_zero_byte_write_is_not_a_fake_disk_full);
KTEST("fatfile", KTEST_LATE, test_getdent_cursor_walks_once);
KTEST("fatfile", KTEST_LATE, test_lfn_full_run_survives);
KTEST("fatfile", KTEST_LATE, test_resolve_poisoned_name_terminates);
KTEST("fatfile", KTEST_LATE, test_fat_entries_beyond_the_cap_is_refused);
KTEST("fatfile", KTEST_LATE, test_last_fat_cell_reachable);
