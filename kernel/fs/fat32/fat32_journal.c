#include "fat32_priv.h"

/* On-disk record header.  One record = this sector, immediately followed by
 * the 512-byte after-image it describes.  seq + crc let recovery reject a
 * torn or truncated tail instead of replaying garbage. */
#define JRN_REC_MAGIC 0x4C524E31u /* "LRN1" */

struct jrn_disk_rec {
    u32 magic;
    u32 seq; /* position within the transaction, 0-based */
    u64 lba; /* home LBA to write the following data sector at */
    u32 crc; /* CRC-32 of the data sector that follows */
    u32 pad;
};

/* CRC-32 (IEEE 802.3, reflected 0xEDB88320).  Bitwise -- this runs over one
 * 512-byte sector per record and one superblock per mount, so a table would
 * buy nothing but a kilobyte of .rodata. */
static u32 jrn_crc(const void *buf, size_t len)
{
    const u8 *p = buf;
    u32 crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

static u32 jrn_super_crc(const struct jrn_super *s)
{ return jrn_crc(s, __builtin_offsetof(struct jrn_super, crc)); }

/* Sectors below the data area belong to the reserved region: the boot sector,
 * FSInfo and the two FATs.  They are written with blk_io_write and are never
 * cached -- the invariant test_fat_region_is_never_cached guards it, and the
 * journal must not be the thing that breaks it.  Everything at or above
 * data_start_lba is a directory or file cluster and goes through the
 * write-back cache as it always did. */
static bool jrn_direct(struct fat_mount *m, u64 lba)
{ return lba < m->data_start_lba; }

/* One home write, exactly the way the pre-journal driver performed it:
 * reserved-region sectors go direct (FAT#1 sectors mirrored into FAT#2),
 * data-region sectors go through the cache.  Shared by the live write path
 * when no transaction is open and by apply/replay when there is one, so a
 * committed record and a plain write can never disagree about *where* a
 * sector ends up. */
static int jrn_write_home(struct fat_mount *m, u64 lba, const void *data)
{
    if (!jrn_direct(m, lba)) {
        if (blk_cache_write(m->dev, lba, data)) {
            fat_warn_io(m, "journal apply", lba);
            return LNXRM_EIO;
        }
        return 0;
    }
    if (blk_io_write(m->dev, lba, 1, data)) {
        fat_warn_io(m, "journal apply", lba);
        return LNXRM_EIO;
    }
    /* fsck.fat compares both tables and "FATs differ" alone makes it exit
     * nonzero, so a FAT#1 sector carries its twin along, exactly as
     * fat_set_entry() did before the journal existed. */
    if (lba >= m->reserved_sectors && lba < m->reserved_sectors + (u64)m->fatsz &&
        m->num_fats > 1) {
        if (blk_io_write(m->dev, lba + m->fatsz, 1, data)) {
            fat_warn_io(m, "journal apply FAT#2", lba + m->fatsz);
            return LNXRM_EIO;
        }
    }
    return 0;
}

/* Apply records [from, to) home, in order.  Ordering inside a transaction is
 * the order the operation performed the writes, which is the order the rest
 * of the driver already relies on (FAT claim before the dirent that points
 * at it, dirent removal before the chain is freed). */
static int jrn_apply(struct fat_mount *m, u32 from, u32 to)
{
    for (u32 i = from; i < to; i++) {
        int rc = jrn_write_home(m, m->tx_rec[i].lba, m->tx_rec[i].data);
        if (rc) return rc;
    }
    return 0;
}

/* --- on-disk superblock ------------------------------------------------- */

static int jrn_read_super(struct fat_mount *m, struct jrn_super *out)
{
    u8 sec[CACHE_SECTOR_SIZE];
    if (blk_io_read(m->dev, m->j_start, 1, sec)) return LNXRM_EIO;
    memcpy(out, sec, sizeof(*out));
    if (out->magic != JRN_MAGIC || out->version != JRN_VERSION) return LNXRM_EINVAL;
    if (out->crc != jrn_super_crc(out)) return LNXRM_EINVAL;
    /* A field that survived the CRC must still name something inside the log;
     * a plausible-but-out-of-range count would send replay off the end. */
    if (out->nrec > JRN_RECORDS_MAX) return LNXRM_EINVAL;
    if (out->head < 1 || out->head + (u64)out->nrec * 2 > m->j_sectors) return LNXRM_EINVAL;
    return 0;
}

static int jrn_write_super(struct fat_mount *m, u32 state, u32 nrec, u64 head)
{
    u8 sec[CACHE_SECTOR_SIZE];
    struct jrn_super s;
    memset(&s, 0, sizeof(s));
    s.magic = JRN_MAGIC;
    s.version = JRN_VERSION;
    s.state = state;
    s.nrec = nrec;
    s.head = head;
    s.crc = jrn_super_crc(&s);
    memset(sec, 0, sizeof(sec));
    memcpy(sec, &s, sizeof(s));
    /* The whole protocol hangs off this one sector write: before it, the
     * transaction exists only in RAM; after it, recovery owes a replay. */
    if (blk_io_write(m->dev, m->j_start, 1, sec)) {
        fat_warn_io(m, "write journal super", m->j_start);
        return LNXRM_EIO;
    }
    return 0;
}

/* --- mount: lay out, then roll an interrupted transaction forward -------- */

void fat_journal_mount(struct fat_mount *m)
{
    /* Deliberately leaves tx_rec/tx_cap alone: the buffer is allocated once,
     * lazily, by fat_journal_begin(), and this function is also how a test
     * re-runs recovery on a live mount. */
    m->j_active = false;
    m->j_start = 0;
    m->j_sectors = 0;
    m->tx_open = false;
    m->tx_nrec = 0;

    /* The journal records whole sectors, so it only exists on a volume whose
     * sector is the sector size the rest of the driver is written against.
     * A 4 KiB-sector image gets no journal and keeps working exactly as it
     * did before -- an honest absence beats a wrong one. */
    if (m->bytes_per_sector != CACHE_SECTOR_SIZE) return;
    /* mkfs's boot(0)/FSInfo(1)/backup-boot(6)/backup-FSInfo(7) layout is what
     * leaves sector 8 as the first unused reserved sector.  Below 8 there is
     * nowhere safe to put a log superblock. */
    if (m->reserved_sectors <= 8) return;

    m->j_start = 8;
    m->j_sectors = m->reserved_sectors - 8;
    /* Superblock + at least a couple of records, or the log cannot hold the
     * smallest possible transaction and pretending otherwise would turn a
     * working volume into ENOSPC failures. */
    if (m->j_sectors < 7) {
        m->j_start = 0;
        m->j_sectors = 0;
        return;
    }

    struct jrn_super s;
    int rc = jrn_read_super(m, &s);
    if (rc) {
        /* Never formatted, or a torn/foreign sector.  Formatting it is safe
         * either way: the log area is ours, and there is no committed
         * transaction we could be throwing away (a committed one would have
         * passed the magic + version + CRC + range checks above). */
        if (jrn_write_super(m, JRN_STATE_CLEAN, 0, 1)) {
            m->j_start = 0;
            m->j_sectors = 0;
            return;
        }
        m->j_active = true;
        kprintf("[fat32] journal: %u-sector log at LBA %u (formatted)\n", m->j_sectors,
                m->j_start);
        return;
    }

    m->j_active = true;
    if (s.state != JRN_STATE_COMMIT) { /* clean: nothing owed */
        kprintf("[fat32] journal: %u-sector log at LBA %u (clean)\n", m->j_sectors,
                m->j_start);
        return;
    }
    kprintf("[fat32] journal: %u-sector log at LBA %u (recovering)\n", m->j_sectors,
            m->j_start);

    /* Committed but never applied.  Re-do it -- the FAT image was loaded
     * into RAM before this call, so the replay has to refresh m->fat too,
     * not just the sectors on disk, or the mount would hand out clusters the
     * just-committed transaction already claimed. */
    u8 sec[CACHE_SECTOR_SIZE];
    u32 done = 0;
    for (u32 i = 0; i < s.nrec; i++) {
        struct jrn_disk_rec rec;
        u64 meta = m->j_start + s.head + (u64)i * 2;
        if (blk_io_read(m->dev, meta, 1, sec)) break;
        memcpy(&rec, sec, sizeof(rec));
        if (rec.magic != JRN_REC_MAGIC || rec.seq != i) break;
        if (blk_io_read(m->dev, meta + 1, 1, sec)) break;
        if (jrn_crc(sec, sizeof(sec)) != rec.crc) break;

        /* Keep the in-RAM FAT in step with what is about to land on disk. */
        if (rec.lba >= m->reserved_sectors &&
            rec.lba < m->reserved_sectors + (u64)m->fatsz) {
            u64 off = (rec.lba - m->reserved_sectors) * m->bytes_per_sector;
            if (off + m->bytes_per_sector <= m->fat_bytes)
                memcpy(&m->fat[off], sec, m->bytes_per_sector);
        }
        if (jrn_write_home(m, rec.lba, sec)) break;
        done++;
    }
    /* Data-region records went through the write-back cache; they have to be
     * on the disk before the commit below is cleared, or a crash in that
     * window would leave a CLEAN log over unwritten directories. */
    blk_cache_flush(m->dev);

    kprintf("[fat32] journal: replayed %u/%u records from interrupted "
            "transaction%s\n",
            done, s.nrec, done == s.nrec ? "" : " (partial -- log damaged)");
    /* Even a partial replay must clear the commit: replaying is idempotent,
     * so re-doing the survivors next boot is harmless, but a permanently
     * stuck COMMIT would keep the message (and the work) alive forever. */
    jrn_write_super(m, JRN_STATE_CLEAN, 0, 1);
}

/* --- transaction lifecycle --------------------------------------------- */

int fat_journal_begin(struct fat_mount *m)
{
    m->tx_open = false;
    m->tx_nrec = 0;
    if (!m->j_active) return 0; /* no log: the op runs exactly as it used to */

    if (!m->tx_rec) {
        /* Lazy: a read-only session never pays for the buffer. */
        u32 cap = (m->j_sectors - 1) / 2; /* each record owns meta + data */
        if (cap > JRN_RECORDS_MAX) cap = JRN_RECORDS_MAX;
        m->tx_rec = kmalloc((size_t)cap * sizeof(struct jrn_rec));
        if (!m->tx_rec) {
            /* Degrade rather than fail the operation: an unjournaled write
             * is the behaviour this driver had before the log existed, and
             * it is still crash-*safe* in the cross-link sense (FAT claim
             * before the dirent, exactly as fat_set_entry() orders it). */
            kprintf("[fat32] journal: no memory for %u records, running "
                    "unjournaled\n",
                    cap);
            m->j_active = false;
            return 0;
        }
        m->tx_cap = cap;
    }
    m->tx_open = true;
    return 0;
}

/* Write the open records into the log, stamp the commit, apply home, clear.
 * Also used mid-operation when the buffer fills: rather than drop a record
 * (the old journal's silent-overflow defect), the chunk is committed as its
 * own unit and the operation continues in a fresh one.  A crash between
 * chunks loses only the tail -- for unlink that means leaked clusters, never
 * cross-linked ones, which is the failure mode the driver already
 * documents. */
static int jrn_checkpoint(struct fat_mount *m)
{
    /* Ordered mode: file data written during this transaction lands first,
     * so a metadata record never becomes durable in front of the bytes it
     * describes. */
    blk_cache_flush(m->dev);
    if (!m->tx_nrec) return 0;

    /* jrn_super.head is an OFFSET from j_start, never an absolute LBA:
     * replay computes `j_start + head + i*2` and jrn_read_super() range-checks
     * it against j_sectors, so the two only agree if head is relative. */
    const u64 head = 1;              /* first sector after the superblock */
    u64 base = m->j_start + head;    /* absolute LBA of record 0's meta */
    u8 meta[CACHE_SECTOR_SIZE];
    u8 data[CACHE_SECTOR_SIZE];
    for (u32 i = 0; i < m->tx_nrec; i++) {
        struct jrn_disk_rec rec;
        memset(&rec, 0, sizeof(rec));
        rec.magic = JRN_REC_MAGIC;
        rec.seq = i;
        rec.lba = m->tx_rec[i].lba;
        memcpy(data, m->tx_rec[i].data, sizeof(data));
        rec.crc = jrn_crc(data, sizeof(data));
        memset(meta, 0, sizeof(meta));
        memcpy(meta, &rec, sizeof(rec));
        if (blk_io_write(m->dev, base + (u64)i * 2, 1, meta) ||
            blk_io_write(m->dev, base + (u64)i * 2 + 1, 1, data)) {
            fat_warn_io(m, "write journal record", base + (u64)i * 2);
            return LNXRM_EIO;
        }
    }

    if (jrn_write_super(m, JRN_STATE_COMMIT, m->tx_nrec, head)) return LNXRM_EIO;

    int rc = jrn_apply(m, 0, m->tx_nrec);
    /* A failed apply must leave the commit standing: clearing it here would
     * drop records nobody has written home yet, turning a recoverable
     * mid-apply stall into permanent loss.  With COMMIT kept, the next mount
     * re-applies the whole transaction -- idempotently -- and finishes it. */
    if (rc) return rc;
    blk_cache_flush(m->dev);
    /* Clearing the commit can only ever *lose* a replay if we did it before
     * the records were home, which is why it is last.  Doing it after is
     * always safe: if this write is the one that dies, recovery re-applies
     * identical bytes. */
    return jrn_write_super(m, JRN_STATE_CLEAN, 0, 1);
}

int fat_journal_commit(struct fat_mount *m)
{
    if (!m->tx_open) {
        /* Unjournaled volume: this is the old fat_sync() -- the single flush
         * under FAT_ENTER that made mkdir/touch/rm reach disk.img. */
        blk_cache_flush(m->dev);
        return 0;
    }
    int rc = jrn_checkpoint(m);
    m->tx_open = false;
    m->tx_nrec = 0;
    return rc;
}

/* Buffer one metadata sector's after-image.  Coalescing by LBA matters: a
 * directory scan rewrites the same sector for every entry it claims, and a
 * bulk free touches the same FAT sector 128 clusters at a time, so the
 * record count tracks distinct sectors touched, not writes performed. */
int fat_journal_add(struct fat_mount *m, u64 lba, const void *buf)
{
    if (!m->tx_open) return jrn_write_home(m, lba, buf);

    for (u32 i = 0; i < m->tx_nrec; i++) {
        if (m->tx_rec[i].lba == lba) {
            memcpy(m->tx_rec[i].data, buf, CACHE_SECTOR_SIZE);
            return 0;
        }
    }
    if (m->tx_nrec >= m->tx_cap) {
        /* Never silently drop: a lost record is a half-applied operation
         * with no way to finish it -- the exact defect C14. */
        int rc = jrn_checkpoint(m);
        if (rc) return rc;
        m->tx_nrec = 0;
        if (m->tx_cap == 0) return LNXRM_ENOSPC;
    }
    m->tx_rec[m->tx_nrec].lba = lba;
    memcpy(m->tx_rec[m->tx_nrec].data, buf, CACHE_SECTOR_SIZE);
    m->tx_nrec++;
    return 0;
}

bool fat_journal_read(const struct fat_mount *m, u64 lba, void *buf)
{
    if (!m->tx_open) return false;
    /* Read-your-own-writes: rename's LFN records and dirent_free() do
     * read-modify-write on one sector several times inside a single
     * operation, and none of those bytes have reached the cache or the
     * disk yet. */
    for (u32 i = 0; i < m->tx_nrec; i++) {
        if (m->tx_rec[i].lba == lba) {
            memcpy(buf, m->tx_rec[i].data, CACHE_SECTOR_SIZE);
            return true;
        }
    }
    return false;
}
