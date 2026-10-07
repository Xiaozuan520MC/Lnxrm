/* FAT32 driver core: cached FAT table access, mount, and the locked
 * fs_ops table.  Directory scanning lives in fat32_dir.c, file data in
 * fat32_file.c and namespace changes in fat32_meta.c; shared types are
 * in fat32_priv.h. */
#include "fat32_priv.h"

void *fat_priv; /* active mount or NULL */

/* Global FAT32 lock: serialises cluster allocation,
 * directory scans and FAT updates across CPUs.  Without it two tasks on
 * different CPUs can allocate the same free cluster (cross-linking) or
 * rewrite FAT/dirent sectors concurrently.  Lock order: fat -> cache -> driver. */
spinlock_t fat_fs_lock = SPINLOCK_INIT;

/* Corruption reporting is rate limited: a hostile image can trip these on
 * every single operation, and an unbounded console flood is its own denial
 * of service. */
#define FAT_WARN_LIMIT 16

static bool fat_warn_allowed(struct fat_mount *m)
{
    if (m->fat_warns >= FAT_WARN_LIMIT) return false;
    m->fat_warns++;
    return true;
}

static const char *fat_warn_tail(struct fat_mount *m)
{ return m->fat_warns == FAT_WARN_LIMIT ? "; further FAT warnings suppressed" : ""; }

static void fat_warn_entry(struct fat_mount *m, u32 clus, u32 val)
{
    if (!fat_warn_allowed(m)) return;
    kprintf("[fat32] corrupt FAT entry: cluster %u -> 0x%08x%s\n", clus, val, fat_warn_tail(m));
}

static void fat_warn_start(struct fat_mount *m, u32 first)
{
    if (!fat_warn_allowed(m)) return;
    kprintf("[fat32] chain starts at cluster %u, not a cluster of this volume%s\n", first,
            fat_warn_tail(m));
}

static void fat_warn_steps(struct fat_mount *m, u32 clus, u32 budget)
{
    if (!fat_warn_allowed(m)) return;
    kprintf("[fat32] chain walk hit its %u step budget at cluster %u "
            "(cyclic or hostile FAT?)%s\n",
            budget, clus, fat_warn_tail(m));
}

/* An I/O failure, named and rate-limited like the corruption reports above.
 * Published so every layer of the driver can say *which* transfer died --
 * "the write failed" is the only evidence a swallowed return value used to
 * destroy (T-005). */
void fat_warn_io(struct fat_mount *m, const char *op, u64 lba)
{
    if (!fat_warn_allowed(m)) return;
    kprintf("[fat32] I/O error %s LBA %llu%s\n", op, lba, fat_warn_tail(m));
}

/* Follow the chain out of `clus`.  Only two kinds of entry are legal in a
 * FAT: one that names a cluster of this volume, or an end-of-chain marker.
 * A free entry (0), cluster 1, the reserved range 0x0FFFFFF0-7 and any
 * cluster number past the end of the volume are all corruption, and are
 * reported instead of followed -- returning an out-of-volume number would
 * otherwise turn a forged FAT into a read past the end of the disk. */
u32 fat_next_cluster(struct fat_mount *m, u32 clus)
{
    if (clus < 2 || clus >= m->max_cluster) return FAT_EOC;
    u32 val = *(u32 *)&m->fat[clus * 4] & 0x0FFFFFFF;
    if (cluster_is_eoc(val)) return val;
    if (val >= 2 && val < m->max_cluster) return val;
    fat_warn_entry(m, clus, val);
    return FAT_EOC;
}

/* Longest walk this volume can legally need, capped by FAT_CHAIN_MAX_STEPS. */
static u32 fat_chain_limit(struct fat_mount *m)
{
    u32 data_clusters = m->max_cluster > 2 ? m->max_cluster - 2 : 0;
    return MIN(FAT_CHAIN_MAX_STEPS, data_clusters);
}

void fat_chain_start(struct fat_chain *ch, struct fat_mount *m, u32 first)
{
    ch->m = m;
    ch->steps = fat_chain_limit(m);
    ch->clus = FAT_EOC;
    if (!first) return; /* empty file or directory: no chain at all */
    if (first < 2 || first >= m->max_cluster) {
        fat_warn_start(m, first);
        return;
    }
    ch->clus = first;
}

bool fat_chain_more(struct fat_chain *ch)
{
    if (ch->clus < 2 || cluster_is_eoc(ch->clus)) return false;
    if (!ch->steps) {
        fat_warn_steps(ch->m, ch->clus, fat_chain_limit(ch->m));
        return false;
    }
    return true;
}

void fat_chain_advance(struct fat_chain *ch)
{
    if (ch->steps) ch->steps--;
    if (ch->clus >= 2 && !cluster_is_eoc(ch->clus))
        ch->clus = fat_next_cluster(ch->m, ch->clus);
}

int fat_set_entry(struct fat_mount *m, u32 clus, u32 val)
{
    if (clus >= m->max_cluster) return LNXRM_EINVAL;
    u32 old = *(u32 *)&m->fat[clus * 4];
    *(u32 *)&m->fat[clus * 4] = (old & 0xF0000000) | (val & 0x0FFFFFFF);
    /* write back the touched sector.  T-005: the result matters -- a silently
     * lost FAT update is how two files end up owning the same cluster after
     * the next remount, so the caller is told and the transfer is reported.
     * The in-memory entry stays updated either way: it is what stops the next
     * allocation scan from handing this cluster out again in the meantime. */
    u64 off = clus * 4;
    u64 lba = m->reserved_sectors + off / m->bytes_per_sector;
    void *sec = &m->fat[off & ~(u64)(m->bytes_per_sector - 1)];
    if (blk_io_write(m->dev, lba, 1, sec)) {
        fat_warn_io(m, "write FAT", lba);
        return LNXRM_EIO;
    }
    /* Mirror the same sector into FAT#2.  mkfs lays down two tables and
     * fsck.fat compares them: writing only the first leaves them differing
     * after any session that allocated or freed a cluster, and "FATs differ"
     * *alone* makes fsck exit nonzero -- failing smoke's "fsck.vfat -n"
     * gate on the next run.  m->fat is the single in-memory image (loaded
     * from FAT#1 and byte-identical to FAT#2 on a mkfs image), so the same
     * window goes to both copies from here on. */
    if (m->num_fats > 1) {
        u64 lba2 = lba + m->fatsz;
        if (blk_io_write(m->dev, lba2, 1, sec)) {
            fat_warn_io(m, "write FAT#2", lba2);
            return LNXRM_EIO;
        }
    }
    /* Track the FSInfo free-cluster summary across claim/release.  mkfs
     * seeds it and fsck.fat verifies it ("Free cluster summary wrong" is an
     * error), so never touching it turns any allocating session into a
     * failing smoke gate too.  0xFFFFFFFF means "unknown" per spec, and an
     * FSInfo sector with bad signatures is not ours to patch. */
    u32 oldv = old & 0x0FFFFFFF, newv = val & 0x0FFFFFFF;
    if ((oldv == 0) != (newv == 0) && m->reserved_sectors > 1) {
        u8 info[512];
        if (!blk_io_read(m->dev, 1, 1, info) &&
            *(u32 *)info == 0x41615252 && *(u32 *)&info[484] == 0x61417272) {
            u32 *fc = (u32 *)&info[488];
            if (*fc != 0xFFFFFFFF) {
                *fc = (oldv == 0) ? (*fc ? *fc - 1 : 0) : *fc + 1;
                if (blk_io_write(m->dev, 1, 1, info))
                    fat_warn_io(m, "write FSInfo", 1);
            }
        }
    }
    return 0;
}

u32 fat_find_free_cluster(struct fat_mount *m)
{
    for (u32 i = 2; i < m->max_cluster; i++)
        if ((*(u32 *)&m->fat[i * 4] & 0x0FFFFFFF) == 0) return i;
    return 0;
}

static int fat32_getdent_locked(void *mnt, void *dirnode, u64 *cookie, struct dirent_out *d)
{
    FAT_ENTER();
    int r = fat32_getdent_impl(mnt, dirnode, cookie, d);
    FAT_LEAVE();
    return r;
}

static u64 fat_freespace_impl(void *mnt)
{
    struct fat_mount *m = fat_priv;
    u64 free_clusters = 0;
    for (u32 i = 2; i < m->max_cluster; i++)
        if ((*(u32 *)&m->fat[i * 4] & 0x0FFFFFFF) == 0) free_clusters++;
    return free_clusters * m->cluster_size;
}

/* --- locked wrappers: single global FS lock serialises all FAT32 ops --- */
static int fat_lookup(void *mnt, const char *path, struct vnode *out)
{
    FAT_ENTER();
    int r = fat_lookup_impl(mnt, path, out);
    FAT_LEAVE();
    return r;
}

static int fat_read(void *mnt, void *node, u64 off, void *ubuf, size_t n)
{
    FAT_ENTER();
    int r = fat_read_impl(mnt, node, off, ubuf, n);
    FAT_LEAVE();
    return r;
}

/* --- mutation write-back ------------------------------------------------
 * Directory and data sectors go through the write-back cache
 * (fat_write_sector -> blk_cache_write), and *nothing* flushes it outside
 * mount time: there is no sync syscall and no shutdown path, and eviction
 * only helps if the LRU ever picks the slot -- a root-dir sector is re-hit
 * by every path lookup, so it essentially never is.  mkdir/touch/rm used to
 * die with QEMU and never reach disk.img, while fat_set_entry()'s FAT write
 * is direct (blk_io_write) and *did* land, leaving the half-updated image
 * fsck reports as "file contains a free cluster".  Flushing after every
 * mutating op, still under FAT_ENTER, commits the whole operation as one
 * unit before the next one can interleave. */
static void fat_sync(void)
{
    struct fat_mount *m = fat_priv;
    blk_cache_flush(m->dev);
}

static int fat_write(void *mnt, void *node, u64 off, const void *buf, size_t n)
{
    FAT_ENTER();
    int r = fat_write_impl(mnt, node, off, buf, n);
    fat_sync();
    FAT_LEAVE();
    return r;
}

static int fat_create(void *mnt, const char *path)
{
    FAT_ENTER();
    int r = fat_create_impl(mnt, path);
    fat_sync();
    FAT_LEAVE();
    return r;
}

static int fat_mkdir(void *mnt, const char *path)
{
    FAT_ENTER();
    int r = fat_mkdir_impl(mnt, path);
    fat_sync();
    FAT_LEAVE();
    return r;
}

static int fat_unlink(void *mnt, const char *path)
{
    FAT_ENTER();
    int r = fat_unlink_impl(mnt, path);
    fat_sync();
    FAT_LEAVE();
    return r;
}

static int fat_rmdir(void *mnt, const char *path)
{
    FAT_ENTER();
    int r = fat_rmdir_impl(mnt, path);
    fat_sync();
    FAT_LEAVE();
    return r;
}

static int fat_rename(void *mnt, const char *oldpath, const char *newpath)
{
    FAT_ENTER();
    int r = fat_rename_impl(mnt, oldpath, newpath);
    fat_sync();
    FAT_LEAVE();
    return r;
}

static u64 fat_freespace(void *mnt)
{
    FAT_ENTER();
    u64 r = fat_freespace_impl(mnt);
    FAT_LEAVE();
    return r;
}

struct fs_ops fat32_ops = {
    .lookup = fat_lookup,
    .getdent = fat32_getdent_locked,
    .read = fat_read,
    .write = fat_write,
    .create = fat_create,
    .mkdir = fat_mkdir,
    .unlink = fat_unlink,
    .rmdir = fat_rmdir,
    .rename = fat_rename,
    .freespace = fat_freespace,
};

/* ---------------- mount ---------------- */
void *fat_mount(struct blkdev *dev)
{
    u8 bpb[512];
    if (blk_io_read(dev, 0, 1, bpb)) return NULL;

    struct fat_mount *m = kmalloc(sizeof(*m));
    if (!m) return NULL; /* T-004: mount reports failure, it does not die */
    memset(m, 0, sizeof(*m));
    m->dev = dev;
    m->bytes_per_sector = *(u16 *)&bpb[11];
    m->sectors_per_cluster = bpb[13];
    m->reserved_sectors = *(u16 *)&bpb[14];
    m->num_fats = bpb[16];
    u32 fatsz16 = *(u16 *)&bpb[22];
    u32 fatsz32 = *(u32 *)&bpb[36];
    m->fatsz = fatsz16 ? fatsz16 : fatsz32;
    m->root_cluster = *(u32 *)&bpb[44];
    u32 totsec16 = *(u16 *)&bpb[19];
    u32 totsec32 = *(u32 *)&bpb[32];
    u32 totsec = totsec16 ? totsec16 : totsec32;

    if (m->bytes_per_sector != dev->sector_size || m->bytes_per_sector > 4096 ||
        !m->sectors_per_cluster || !m->fatsz || !m->reserved_sectors ||
        (m->num_fats != 1 && m->num_fats != 2) || m->root_cluster < 2 ||
        (*(u16 *)&bpb[22] && bpb[13])) {
        /* Not a valid FAT32 filesystem */
        kfree(m);
        return NULL;
    }
    u16 rootents = *(u16 *)&bpb[17];

    /* totsec must not exceed the real device; a forged BPB could make
     * cluster->LBA math run off the end of the disk. */
    if (totsec && dev->num_sectors && totsec > dev->num_sectors) totsec = (u32)dev->num_sectors;

    u32 fat_start = m->reserved_sectors;
    u64 data_start = fat_start + (u64)m->num_fats * m->fatsz +
                     (rootents * 32 + m->bytes_per_sector - 1) / m->bytes_per_sector;
    if (data_start >= totsec) {
        kfree(m);
        return NULL;
    }
    m->data_start_lba = data_start;
    m->max_cluster = totsec > data_start ? (totsec - data_start) / m->sectors_per_cluster + 2 : 2;
    m->cluster_size = m->bytes_per_sector * m->sectors_per_cluster;

    /* CRITICAL: clamp the cluster range to what the FAT table can hold.
     * Without this, chain walks / allocation scans read & WRITE past the
     * end of the cached FAT heap buffer, corrupting neighbouring objects
     * (this was the source of the "mystery" function-pointer corruption). */
    u32 fat_entries = m->fatsz * m->bytes_per_sector / 4;
    if (m->max_cluster >= fat_entries) m->max_cluster = fat_entries ? fat_entries - 1 : 2;

    /* The root directory cluster is turned into an LBA before any chain walk
     * could object to it, so a BPB that names a cluster past the end of the
     * volume must kill the mount here. */
    if (m->root_cluster >= m->max_cluster) {
        kprintf("[fat32] reject %s: root cluster %u outside volume "
                "(data clusters 2..%u)\n",
                dev->name, m->root_cluster, m->max_cluster - 1);
        kfree(m);
        return NULL;
    }

    m->fat_bytes = (u64)m->fatsz * m->bytes_per_sector;
    m->fat = kmalloc(m->fat_bytes);
    if (!m->fat || blk_io_read(dev, fat_start, m->fatsz, m->fat)) {
        kfree(m->fat);
        kfree(m);
        return NULL;
    }

    kprintf("[fat32] mounted %s: %u MiB, %u B/sector x %u/cluster, "
            "root=%u\n",
            dev->name, totsec * m->bytes_per_sector >> 20, m->bytes_per_sector,
            m->sectors_per_cluster, m->root_cluster);

    fat_priv = m;
    return m;
}
