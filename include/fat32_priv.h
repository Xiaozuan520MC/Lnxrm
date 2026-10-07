/* Shared internals of the FAT32 driver: on-disk structures, mount state,
 * path/cluster helpers and the cross-file entry points of
 * fat32.c / fat32_dir.c / fat32_file.c / fat32_meta.c.  Module-private:
 * nothing outside kernel/fs/ includes this header -- except the boot
 * self-test (ktest/t_fs.c), which is precisely the second consumer
 * these helpers need. */
#ifndef LNXRM_FAT32_PRIV_H
#define LNXRM_FAT32_PRIV_H

#include <sys/vfs.h>
#include <console.h>
#include <mm/mm.h>
#include <disk.h>
#include <sys/spinlock.h>
extern void *fat_priv;          /* active mount or NULL */
extern struct fs_ops fat32_ops; /* defined in fat32.c */

/* Global FAT32 lock: serialises cluster allocation, directory scans and
 * FAT updates across CPUs.  Without it two tasks on different CPUs can
 * allocate the same free cluster (cross-linking) or rewrite FAT/dirent
 * sectors concurrently.  Lock order: fat -> cache -> driver. */
extern spinlock_t fat_fs_lock;

#define FAT_ENTER()                                                                                \
    u64 _fat_fl;                                                                                   \
    spin_lock_irqsave(&fat_fs_lock, &_fat_fl)
#define FAT_LEAVE() spin_unlock_irqrestore(&fat_fs_lock, _fat_fl)

static inline char ascii_toupper(char c)
{ return c >= 'a' && c <= 'z' ? c - 32 : c; }

static inline char ascii_tolower(char c)
{ return c >= 'A' && c <= 'Z' ? c + 32 : c; }

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
    u8 *fat; /* cached FAT table */
    u64 fat_bytes;
    u32 fat_warns; /* corruption reports printed so far (rate limiter) */
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

#define ATTR_VOLUME  0x08
#define ATTR_DIR     0x10
#define ATTR_ARCHIVE 0x20
#define ATTR_LFN     0x0F
#define ENT_FREE     0x00
#define ENT_E5       0xE5
#define ENT_END      0x05

/* Sector I/O through the write-back cache.  Contract (T-005): returns 0 or
 * LNXRM_EIO, never a driver-specific code -- callers propagate the value
 * unchanged, so "the disk refused" reaches user space as EIO instead of
 * being mistaken for EOF, "not found" or success.  Every one of these
 * results must be checked: an unchecked read feeds uninitialised bytes to
 * the write below, and that is how kernel stack garbage ends up in a
 * directory entry. */
static inline int fat_read_sector(struct fat_mount *m, u64 lba, void *buf)
{ return blk_cache_read(m->dev, lba, buf) ? LNXRM_EIO : 0; }

static inline int fat_write_sector(struct fat_mount *m, u64 lba, const void *buf)
{ return blk_cache_write(m->dev, lba, buf) ? LNXRM_EIO : 0; }

/* Read a whole cluster through the same write-back cache every write goes
 * through.  A raw dev->read here would bypass dirty cache lines, so an entry
 * just created (still in the cache, not yet written back) would be invisible
 * to path resolution, ls and unlink until writeback -- which is exactly how
 * mkdir/touch could "succeed" and then never appear. */
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
 * This is the *only* place FAT32 records case for a short name -- the 11 name
 * bytes are uppercase by convention -- so a driver that ignores them cannot
 * show "README.md" as anything but "readme.md". */
#define FAT_NTRES_LOWER_BASE 0x08u /* base is lower case */
#define FAT_NTRES_LOWER_EXT  0x10u /* extension is lower case */

/* 8.3 short name -> display name, honouring the NT case bits:
 *   "README  MD " + FAT_NTRES_LOWER_EXT  -> "README.md"
 *   "BIN        " + FAT_NTRES_LOWER_BASE -> "bin"
 * A clear bit hands the byte back exactly as stored (what Linux does in
 * fs/fat/dir.c: fat_shortname2uni() only lowercases when asked to). */
void fat_short_to_name(const u8 *sn, u8 ntres, char out[13]);
/* Case bits that let `name`'s own 8.3 alias render back to `name` verbatim.
 * A part whose letters are all one case needs no long-name record: mark it
 * lowercase (or leave an all-caps one alone) and the alias already spells it.
 * A mixed-case part cannot be spelled by 8.3 at all -- no bit makes it match,
 * so fat_lfn_count()'s strcmp sees the mismatch and allocates an LFN record
 * instead of storing a name that would read back in the wrong case. */
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

/* Hard ceiling on one cluster-chain walk.  A legal FAT32 chain never holds
 * more than 4 GiB / 512 B = 8,388,608 clusters (largest possible file,
 * smallest legal cluster), so a walk that outlives this budget can only be
 * a cyclic or hostile FAT -- never a valid file. */
#define FAT_CHAIN_MAX_STEPS 0x800010u /* 8,388,624 */

/* A single bounded walk over a cluster chain.  Every chain walk in the
 * driver goes through one of these instead of open-coded
 * `c = fat_next_cluster()` loops: fat_chain_start() rejects an illegal
 * first cluster, fat_chain_advance() refuses to follow an entry that is
 * neither a data cluster of this volume nor an end-of-chain marker, and
 * fat_chain_more() hands out a fixed step budget, so a FAT that points at
 * itself (or at a cluster past the end of the volume) can neither hang the
 * kernel with fat_fs_lock held nor steer a read off the disk. */
struct fat_chain {
    struct fat_mount *m;
    u32 clus;   /* cluster the walk sits on; FAT_EOC once it is done */
    u32 steps;  /* advances still budgeted */
};

void fat_chain_start(struct fat_chain *ch, struct fat_mount *m, u32 first);
bool fat_chain_more(struct fat_chain *ch);
void fat_chain_advance(struct fat_chain *ch);

static inline u64 cluster_lba(struct fat_mount *m, u32 clus)
{ return m->data_start_lba + (u64)(clus - 2) * m->sectors_per_cluster; }

static inline void *cluster_buf_alloc(struct fat_mount *m)
{ return kmalloc(m->cluster_size ? m->cluster_size : 4096); }

static inline bool entry_used(const struct fat_dirent *e)
{ return e->name[0] != ENT_FREE && e->name[0] != ENT_E5; }

/* Cluster / FAT table access (fat32.c). */
u32 fat_next_cluster(struct fat_mount *m, u32 clus);
/* Write one FAT entry (memory copy + the sector backing it).
 * Returns 0 or LNXRM_EIO.  On failure the in-memory entry still changes:
 * the mounted copy is the authority that keeps the next allocation scan
 * from handing the same cluster out twice, so a cluster is leaked to the
 * disk rather than cross-linked. */
int fat_set_entry(struct fat_mount *m, u32 clus, u32 val);
u32 fat_find_free_cluster(struct fat_mount *m);
/* Rate-limited I/O failure report (fat32.c): a dying disk can fail every
 * transfer, and an unbounded console flood is its own denial of service. */
void fat_warn_io(struct fat_mount *m, const char *op, u64 lba);

/* Directory scan + path resolution (fat32_dir.c). */
/* dirclus/off locate the directory entry physically (containing cluster
 * and byte offset inside it), which is what makes a stable inode number
 * out of a FAT32 record. */
typedef int (*dirent_cb)(const char *name, const struct fat_dirent *e, u32 dirclus, u32 off,
                         void *ctx);
struct find_ctx {
    const char *want;
    struct fat_dirent found;
};
struct resolve {
    u32 dir_cluster; /* parent's cluster */
    char name[56];
    struct fat_dirent de; /* valid if found */
    bool found;
};
/* Walk a directory chain, calling `cb` per live entry.
 * T-005: returns 1 when a callback stopped the walk (it matched), 0 when the
 * directory was walked to its end, or a negative LNXRM error when a sector
 * could not be read.  A failed read is never folded into "0 = not found":
 * the caller would answer ENOENT/empty for data it never got to see. */
int fat_scan_dir(struct fat_mount *m, u32 dirclus, dirent_cb cb, void *ctx);
int fat_scan_find_cb(const char *name, const struct fat_dirent *e, u32 dirclus, u32 off,
                     void *ctx);
/* resolve a path to {parent_dir_cluster, dirent copy, name}.
 * 1 = resolved, 0 = some component is missing, negative = a directory could
 * not be read (report that as EIO, never as ENOENT). */
int fat_resolve_path(struct fat_mount *m, const char *path, struct resolve *r);
/* One readdir step: 1 = `d` filled, 0 = no more entries, negative = the
 * directory could not be read.  fs_ops->getdent hands that shape straight to
 * dir_getdent(), which needs to tell "finished" from "failed" (T-005). */
int fat_dir_iter(struct dir_iter *it, struct dirent_out *d);
int fat32_getdent_impl(void *mnt, void *dirnode, u64 *cookie, struct dirent_out *d);

/* Locked fs_ops implementations (fat32_file.c / fat32_meta.c). */
int fat_lookup_impl(void *mnt, const char *path, struct vnode *out);
int fat_read_impl(void *mnt, void *node, u64 off, void *ubuf, size_t n);
int fat_write_impl(void *mnt, void *node, u64 off, const void *buf, size_t n);
int fat_create_impl(void *mnt, const char *path);
int fat_mkdir_impl(void *mnt, const char *path);
int fat_rmdir_impl(void *mnt, const char *path);
int fat_unlink_impl(void *mnt, const char *path);
int fat_rename_impl(void *mnt, const char *oldpath, const char *newpath);

#endif /* LNXRM_FAT32_PRIV_H */
