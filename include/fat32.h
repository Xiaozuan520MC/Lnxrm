// SPDX-License-Identifier: Apache-2.0

/* Shared internals of the FAT32 driver: on-disk structures, mount state,
 * path/cluster helpers and the cross-file entry points of fat32.c /
 * fat32_dir.c / fat32_file.c / fat32_meta.c.  Also consumed by the boot
 * self-test (ktest/t_fs.c). */
#ifndef LNXRM_FAT32_H
#define LNXRM_FAT32_H

#include <sys/vfs.h>
#include <console.h>
#include <mm/mm.h>
#include <disk.h>
#include <sys/spinlock.h>
extern void *fat_priv;          /* the volume fat_*_impl with no mount argument
                                * acts on: the one mounted at "/", else the
                                * first mounted (C51 -- never overwritten) */
extern struct fs_ops fat32_ops; /* defined in fat32.c */

/* Global FAT32 lock: serialises cluster allocation, directory scans and FAT
 * updates across CPUs -- otherwise two CPUs can allocate the same free cluster
 * (cross-linking).  Lock order: fat -> cache -> driver.
 *
 * C7 (2026-11-13): taken with spin_lock_irqon, i.e. WITHOUT touching
 * interrupt state -- the one lock here that may be.  Safe because only the
 * fs_ops wrappers in fat32.c take it (no IRQ handler, tick or panic() does),
 * nothing between FAT_ENTER and FAT_LEAVE can raise a #PF while it is held,
 * and ring-0 holders are never preempted; every taker already arrives at
 * IF=0, so this costs the lock only its cli/popfq pair (a contract change,
 * not a behavioural one).  It becomes load-bearing the moment a caller runs
 * with interrupts on (GAP §3), where irqsave would stop this CPU's tick for
 * the length of every FS operation.  It does NOT buy much yet: the FS is
 * still one critical section wide across device I/O -- the remaining half of C7. */
extern spinlock_t fat_fs_lock;

#define FAT_ENTER() spin_lock_irqon(&fat_fs_lock)
#define FAT_LEAVE() spin_unlock_irqon(&fat_fs_lock)

static inline char ascii_toupper(char c)
{ return c >= 'a' && c <= 'z' ? c - 32 : c; }

static inline char ascii_tolower(char c)
{ return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* ---------------- crash-consistency journal (fat32_journal.c) ----------
 * One record = the after-image of a single metadata sector plus the LBA it
 * belongs at, buffered in RAM while a transaction is open, written to the log
 * before any home location is touched, replayed on mount after a crash.
 * Recording the *after*-image is what makes replay an idempotent redo. */

#define JRN_RECORDS_MAX 128 /* hard ceiling on one transaction's records */

struct jrn_rec {
    u64 lba;                    /* home LBA this after-image belongs at */
    u8 data[CACHE_SECTOR_SIZE]; /* the sector contents to write there */
};

/* Journal on-disk superblock, kept in the first reserved sector of the log.
 * state is the only field recovery trusts to decide whether a replay is
 * owed; everything else is validated by magic + version + CRC so a corrupted
 * or foreign sector can never be mistaken for a commit record. */
#define JRN_MAGIC   0x4A524E31u /* "JRN1" */
#define JRN_VERSION 1u

#define JRN_STATE_CLEAN 0u /* no committed-but-unapplied transaction */
#define JRN_STATE_COMMIT 1u /* records are durable; replay them on mount */

struct jrn_super {
    u32 magic;
    u32 version;
    u32 state;    /* JRN_STATE_CLEAN or JRN_STATE_COMMIT */
    u32 nrec;     /* committed record count (valid when state == COMMIT) */
    u64 head;     /* log slot the committed records start at (sector offset) */
    u32 crc;      /* CRC over every field above */
};

/* Journal entry points (fat32_journal.c), all called with fat_fs_lock held,
 * from the locked wrappers in fat32.c or from fat_mount(). */
struct fat_mount;
/* Format-or-find the journal and roll a committed but unapplied transaction
 * forward.  Safe on a volume never journalled: it lays the superblock down.
 * Leaves j_active false when there is no usable reserved area, in which case
 * every operation below degrades to the pre-journal write path. */
void fat_journal_mount(struct fat_mount *m);
/* Open a transaction.  Always 0: no log, or no record buffer, simply runs
 * unjournaled instead of failing the operation. */
int fat_journal_begin(struct fat_mount *m);
/* Close the transaction durably: flush file data, write the records and the
 * commit record, apply them home, then clear the commit.  Also the entry point
 * for an unjournaled volume, where it is just fat_sync()'s cache flush.
 * Returns 0 or LNXRM_EIO. */
int fat_journal_commit(struct fat_mount *m);
/* Buffer one metadata sector's after-image, or -- with no transaction open
 * -- write it home the way the driver did before the journal existed.
 * Returns 0 or LNXRM_EIO.  Called by fat_write_sector()/fat_set_entry(). */
int fat_journal_add(struct fat_mount *m, u64 lba, const void *buf);
/* Serve a read from the open transaction's buffer, if it holds `lba`.
 * Returns true when it filled `buf`.  Called by fat_read_sector(). */
bool fat_journal_read(const struct fat_mount *m, u64 lba, void *buf);

struct fat_mount {
    struct blkdev *dev;
    u32 bytes_per_sector;
    u32 sectors_per_cluster;
    u32 cluster_size;
    u32 reserved_sectors;
    u32 num_fats;
    u32 fatsz; /* sectors per FAT */
    u32 root_cluster;
    u64 data_start_lba; /* cluster 2 */
    u32 max_cluster;
    /* Cached FAT table as an array of PAGE_SIZE frames, not one contiguous
     * buffer: a real volume's FAT is proportional to its size (8 MiB for a
     * 16 GiB partition), so a single kmalloc(fat_bytes) used to fail silently
     * above ~4 MiB of table.  Paging removes the ceiling -- fat_npages frames,
     * each pmm_alloc()ed.  A FAT entry (4 B) and a sector (512..4096 B) both
     * divide PAGE_SIZE, so neither ever straddles a frame: the lookup below is
     * exact. */
    u8 **fat_pages; /* fat_npages frames, each PAGE_SIZE */
    u32 fat_npages; /* == ceil(fat_bytes / PAGE_SIZE) */
    u64 fat_bytes;
    u32 fat_warns; /* corruption reports printed so far (rate limiter) */
    u8 dirent_stamp; /* advances once per created record: the creation
                      * tenth-of-second crttenth field gets this value, so
                      * two incarnations of the same name never share their
                      * identity stamp (the guard behind C21) */

    /* --- crash-consistency journal (fat32_journal.c) -------------------
     * A metadata-only redo log in the reserved sectors mkfs leaves unused
     * (sector 8 .. reserved_sectors-1).  It turns each mutating operation
     * into one atomic transaction: after-images of every metadata sector it
     * touches are buffered in RAM, committed to the log *before* any of them
     * reaches its home location, and replayed on the next mount if the machine
     * died between commit and apply.  File data is deliberately not logged
     * (ordered mode): a crash can leave stale bytes inside a file, but never
     * a half-updated FAT or a dangling directory entry. */
    u32 j_start;   /* first LBA of the journal (0 = no journal) */
    u32 j_sectors; /* total journal sectors including the superblock */
    bool j_active; /* journal present and usable on this volume */

    /* open transaction state.  Guarded by fat_fs_lock like everything else. */
    bool tx_open;
    u32 tx_nrec;                 /* records buffered so far */
    struct jrn_rec *tx_rec;      /* heap array of after-images, tx_cap slots */
    u32 tx_cap;                  /* allocated slots in tx_rec */
};

#define DIRENT_SIZE 32

struct fat_dirent {
    u8 name[11];
    u8 attr;
    u8 ntres;
    u8 crttenth;
    u16 crttime, crtdate, lstaccdate;
    u16 fstclushi;
    u16 wrttime, wrtdate;
    u16 fstcluslo;
    u32 filesize;
} __attribute__((packed));

#define ATTR_READONLY 0x01
#define ATTR_VOLUME  0x08
#define ATTR_DIR     0x10
#define ATTR_ARCHIVE 0x20
#define ATTR_LFN     0x0F
/* FAT-standard slot first-byte states: 0x00 is an unused slot and the
 * directory terminator, 0xE5 is deleted, anything else is live.  0x05 is live
 * too: the spec re-uses it to carry a literal 0xE5 in an 8.3 alias, and the
 * fifth piece of a long-name run begins with its sequence number 5.  Keying
 * end-of-directory on 0x05 while scanning on past 0x00 cut a 20-piece long
 * name at piece five and let stranded records surface as phantom names. */
#define ENT_END      0x00 /* scanners stop here; claims may take the slot */
#define ENT_E5       0xE5 /* deleted: scanners skip it, claims may reuse  */

/* Sector I/O through the write-back cache.  Contract (T-005): returns 0 or
 * LNXRM_EIO, never a driver-specific code -- callers propagate the value
 * unchanged, so "the disk refused" reaches user space as EIO rather than as
 * EOF, "not found" or success.  Every result must be checked: an unchecked
 * read feeds uninitialised bytes to the write below.  Reads consult the open
 * journal transaction first, so a mutating operation sees its own metadata
 * writes before they reach their home location. */
static inline int fat_read_sector(struct fat_mount *m, u64 lba, void *buf)
{
    if (fat_journal_read(m, lba, buf)) return 0;
    return blk_cache_read(m->dev, lba, buf) ? LNXRM_EIO : 0;
}

/* Metadata write (directory entries, new-directory contents, FAT sectors).
 * With a transaction open this buffers the after-image instead of touching
 * the home location -- the commit record must reach the log first, or a crash
 * mid-operation would leave a half-applied change with nothing to roll forward
 * from.  With no transaction (or no journal) it is the plain write-back-cache
 * write, i.e. exactly the pre-journal behaviour. */
static inline int fat_write_sector(struct fat_mount *m, u64 lba, const void *buf)
{ return fat_journal_add(m, lba, buf); }

/* File *data* write: bypasses the journal entirely (ordered mode).  Logging
 * contents would multiply every write by two for no consistency gain -- data
 * bytes carry no cross-referential structure to keep coherent. */
static inline int fat_write_sector_raw(struct fat_mount *m, u64 lba, const void *buf)
{ return blk_cache_write(m->dev, lba, buf) ? LNXRM_EIO : 0; }

/* Read a whole cluster through the same write-back cache every write goes
 * through.  A raw dev->read would bypass dirty cache lines, so an entry just
 * created would be invisible to path resolution, ls and unlink until writeback
 * -- exactly how mkdir/touch could "succeed" and then never appear. */
static inline int fat_read_cluster(struct fat_mount *m, u64 clba, void *buf)
{
    u8 *p = buf;
    for (u32 s = 0; s < m->sectors_per_cluster; s++) {
        int rc = fat_read_sector(m, clba + s, p + (size_t)s * m->bytes_per_sector);
        if (rc) return rc; /* LNXRM_EIO: never report a failed scan as "empty" */
    }
    return 0;
}

static inline void name_to_short(const char *in, u8 out[11])
{
    memset(out, ' ', 11);
    int i = 0, o = 0;
    while (in[i] && in[i] != '.' && o < 8) out[o++] = ascii_toupper(in[i++]);
    if (in[i] == '.') {
        i++;
        o = 8;
        while (in[i] && o < 11) out[o++] = ascii_toupper(in[i++]);
    }
}

/* NT case bits (dirent.ntres): which half of an 8.3 name renders lowercase.
 * The *only* place FAT32 records case for a short name -- a driver that
 * ignores them cannot show "README.md" as anything but "readme.md". */
#define FAT_NTRES_LOWER_BASE 0x08u /* base is lower case */
#define FAT_NTRES_LOWER_EXT  0x10u /* extension is lower case */

/* LFN alias checksum (ECMA-107): shared by writer and reader, so an
 * assembled long name can be bound to the 8.3 record that owns it. */
static inline u8 fat_lfn_checksum(const u8 *sn)
{
    u8 sum = 0;
    for (int i = 0; i < 11; i++) sum = (u8)(((sum & 1) << 7) + (sum >> 1) + sn[i]);
    return sum;
}

/* 8.3 short name -> display name, honouring the NT case bits:
 *   "README  MD " + FAT_NTRES_LOWER_EXT  -> "README.md"
 *   "BIN        " + FAT_NTRES_LOWER_BASE -> "bin"
 * A clear bit hands the byte back exactly as stored (Linux does the same). */
void fat_short_to_name(const u8 *sn, u8 ntres, char out[13]);
/* Case bits that let `name`'s own 8.3 alias render back to `name` verbatim:
 * an all-one-case part needs no long-name record, a mixed-case part cannot be
 * spelled by 8.3 at all, so fat_lfn_count()'s strcmp sees the mismatch and
 * allocates an LFN instead of storing a name that would read back wrong. */
static inline u8 fat_short_ntres(const char *name)
{
    u8 bits = 0;
    int i = 0, o = 0;
    bool lower = true;
    for (; name[i] && name[i] != '.' && o < 8; i++, o++)
        if (name[i] >= 'A' && name[i] <= 'Z') lower = false;
    if (lower) bits |= FAT_NTRES_LOWER_BASE;
    if (name[i] == '.') {
        lower = true;
        for (i++, o = 8; name[i] && o < 11; i++, o++)
            if (name[i] >= 'A' && name[i] <= 'Z') lower = false;
        if (lower) bits |= FAT_NTRES_LOWER_EXT;
    }
    return bits;
}

/* does `want` match the stored short name (after 8.3 truncation)? */
bool fat_name_eq_short(const char *want, const struct fat_dirent *e);

static inline int cluster_is_eoc(u32 c)
{ return c >= 0x0FFFFFF8; }

#define FAT_EOC 0x0FFFFFFFu

/* Hard ceiling on one cluster-chain walk: a legal FAT32 chain never holds
 * more than 4 GiB / 512 B = 8,388,608 clusters, so a walk that outlives this
 * budget can only be a cyclic or hostile FAT -- never a valid file. */
#define FAT_CHAIN_MAX_STEPS 0x800010u /* 8,388,624 */

/* C16: longest chain one volume can legally hold, capped by
 * FAT_CHAIN_MAX_STEPS.  A write demand beyond fat_chain_cap_bytes() is refused
 * before any allocation -- never wrapped through u32, never booked into the
 * whole free pool. */
static inline u32 fat_chain_cap(struct fat_mount *m)
{
    u32 data = m->max_cluster > 2 ? m->max_cluster - 2 : 0;
    return MIN(FAT_CHAIN_MAX_STEPS, data);
}

static inline u64 fat_chain_cap_bytes(struct fat_mount *m)
{
    return (u64)fat_chain_cap(m) * m->cluster_size;
}

/* A single bounded walk over a cluster chain -- every chain walk goes through
 * one of these instead of open-coded `c = fat_next_cluster()` loops: start
 * rejects an illegal first cluster, advance refuses an entry that is neither
 * a data cluster nor end-of-chain, and `steps` is a fixed budget, so a FAT
 * pointing at itself (or past the end) can neither hang the kernel with
 * fat_fs_lock held nor steer a read off the disk. */
struct fat_chain {
    struct fat_mount *m;
    u32 clus;   /* cluster the walk sits on; FAT_EOC once it is done */
    u32 steps;  /* advances still budgeted */
};

void fat_chain_start(struct fat_chain *ch, struct fat_mount *m, u32 first);
bool fat_chain_more(struct fat_chain *ch);
void fat_chain_advance(struct fat_chain *ch);
/* Free every cluster of the chain starting at `first`, one entry at a time.
 * Returns 0 or LNXRM_EIO (already reported): an entry that cannot be cleared
 * leaks its cluster and stops the walk right there, so two files can never
 * share it.  Read-then-free ordering is the caller's own contract. */
int fat_free_chain(struct fat_mount *m, u32 first);

static inline u64 cluster_lba(struct fat_mount *m, u32 clus)
{ return m->data_start_lba + (u64)(clus - 2) * m->sectors_per_cluster; }

static inline void *cluster_buf_alloc(struct fat_mount *m)
{ return kmalloc(m->cluster_size ? m->cluster_size : 4096); }

static inline bool entry_used(const struct fat_dirent *e)
{ return e->name[0] != ENT_END && e->name[0] != ENT_E5; }

/* Compare an 8.3 alias against a wanted short name, byte 0 case-insensitive:
 * a stored 0x05 is the spec's escape for a literal 0xE5, so an entry written
 * with the escape still matches what the user typed. */
static inline bool fat_alias_eq(const u8 stored[11], const u8 want[11])
{
    for (int i = 0; i < 11; i++) {
        u8 a = stored[i], b = want[i];
        if (i == 0) {
            if (a == 0x05) a = 0xE5;
            if (b == 0x05) b = 0xE5;
        }
        if (a != b) return false;
    }
    return true;
}

/* Cluster / FAT table access (fat32.c). */
/* The FAT is paged (fat_pages, see struct fat_mount): these three helpers are
 * the ONLY way driver or ktest code touches a FAT entry or sector.  Byte
 * offset `off` maps to frame off / PAGE_SIZE at offset off & (PAGE_SIZE-1);
 * entries and sectors both divide PAGE_SIZE, so neither straddles a frame. */
static inline u8 *fat_page_at(struct fat_mount *m, u64 off)
{ return m->fat_pages[off / PAGE_SIZE]; }
/* Raw 32-bit entry at cluster `clus` (all four bytes, high nibble intact);
 * mask with 0x0FFFFFFF where the caller means "the cluster it names". */
u32 fat_entry_get(struct fat_mount *m, u32 clus);
/* Raw 32-bit store at cluster `clus` -- the in-memory half of fat_set_entry,
 * no journal, no write-back.  Callers own the masking. */
void fat_entry_put_raw(struct fat_mount *m, u32 clus, u32 val);
/* Pointer to the sector-aligned byte offset `off` inside the cached FAT, for
 * the journal's whole-sector memcpy.  `off` must be sector-aligned. */
void *fat_sector_ptr(struct fat_mount *m, u64 off);
/* Release every FAT frame and the page array.  Safe on a partially built or
 * all-NULL mount (mount failure paths call it). */
void fat_pages_free(struct fat_mount *m);

u32 fat_next_cluster(struct fat_mount *m, u32 clus);
/* Write one FAT entry (memory copy + the sector backing it).
 * Returns 0 or LNXRM_EIO.  On failure the in-memory entry still changes: the
 * mounted copy is what keeps the next scan from handing the same cluster out
 * twice, so a cluster is leaked to the disk rather than cross-linked. */
int fat_set_entry(struct fat_mount *m, u32 clus, u32 val);
u32 fat_find_free_cluster(struct fat_mount *m);
/* Rate-limited I/O failure report (fat32.c): a dying disk can fail every
 * transfer, and an unbounded console flood is its own denial of service. */
void fat_warn_io(struct fat_mount *m, const char *op, u64 lba);

/* Directory scan + path resolution (fat32_dir.c). */
/* dirclus/off locate the directory entry physically, which is what makes a
 * stable inode number out of a FAT32 record. */
typedef int (*dirent_cb)(const char *name, const struct fat_dirent *e, u32 dirclus, u32 off,
                         void *ctx);
struct find_ctx {
    const char *want;
    struct fat_dirent found;
    u32 dirclus, off; /* where the match physically sits (C21 write-back) */
};
struct resolve {
    u32 dir_cluster; /* parent's cluster */
    u32 ent_clus, ent_off; /* where the 8.3 record physically sits:
                            * the containing cluster and the byte offset
                            * inside it.  C21: write-back addresses this
                            * position directly -- never a name scan that
                            * two files sharing one 8.3 alias can both win */
    char name[56];
    struct fat_dirent de; /* valid if found */
    bool found;
};
/* Inode number: (containing cluster << 32) | offset inside that cluster --
 * unique across the volume and stable across reboots.  Shared by the directory
 * scan (getdent) and fat_lookup_impl (stat) so both doors hand out the SAME
 * number for the same record. */
static inline u64 fat_ino(u32 dirclus, u32 off)
{ return ((u64)dirclus << 32) | off; }

/* Walk a directory chain, calling `cb` per live entry.
 * T-005: 1 when a callback stopped the walk (matched), 0 when the directory
 * was walked to its end, negative when a sector could not be read.  A failed
 * read is never folded into "0 = not found": the caller would answer
 * ENOENT/empty for data it never got to see. */
int fat_scan_dir(struct fat_mount *m, u32 dirclus, dirent_cb cb, void *ctx);
/* C30: same walk started at the getdent cursor `resume` -- (cluster << 32) |
 * offset of the next record to examine.  All-ones or garbage answers "ended";
 * clusters before the cursor cost no device read.  Readdir path only; name
 * lookup keeps calling fat_scan_dir(). */
#define DIRIT_START 0ULL
#define DIRIT_END   ((u64)-1)
int fat_scan_dir_at(struct fat_mount *m, u32 dirclus, u64 resume, dirent_cb cb, void *ctx);
int fat_scan_find_cb(const char *name, const struct fat_dirent *e, u32 dirclus, u32 off,
                     void *ctx);
/* resolve a path to {parent_dir_cluster, dirent copy, name}.
 * 1 = resolved, 0 = some component is missing, negative = a directory could
 * not be read (report that as EIO, never as ENOENT). */
int fat_resolve_path(struct fat_mount *m, const char *path, struct resolve *r);
/* One readdir step: 1 = `d` filled, 0 = no more entries, negative = the
 * directory could not be read.  fs_ops->getdent hands that shape straight to
 * dir_getdent(), which must tell "finished" from "failed" (T-005). */
int fat_dir_iter(struct dir_iter *it, struct dirent_out *d);
int fat32_getdent_impl(void *mnt, void *dirnode, u64 *cookie, struct dirent_out *d);

/* Locked fs_ops implementations (fat32_file.c / fat32_meta.c). */
int fat_lookup_impl(void *mnt, const char *path, struct vnode *out);
/* Mount the volume behind `dev` (NULL when the BPB is refused).  Boot-only
 * entry point; the ktest boundary cases forge volumes and exercise it
 * directly, so it is exported here, not in a public header. */
void *fat_mount(struct blkdev *dev);
int fat_read_impl(void *mnt, void *node, u64 off, void *ubuf, size_t n);
u64 fat_size_impl(void *mnt, void *node);
void fat_close_node_impl(void *mnt, void *node);
int fat_write_impl(void *mnt, void *node, u64 off, const void *buf, size_t n);
int fat_create_impl(void *mnt, const char *path);
int fat_mkdir_impl(void *mnt, const char *path);
int fat_rmdir_impl(void *mnt, const char *path);
int fat_unlink_impl(void *mnt, const char *path);
int fat_rename_impl(void *mnt, const char *oldpath, const char *newpath);

#endif /* LNXRM_FAT32_H */
