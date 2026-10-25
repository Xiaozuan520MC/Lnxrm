/* FAT32 file layer: path lookup plus read / write-grow-truncate of
 * regular files (cluster chains are extended through alloc_chain). */
#include "fat32_priv.h"

int fat_lookup_impl(void *mnt, const char *path, struct vnode *out)
{
    struct fat_mount *m = fat_priv;
    struct resolve r;
    while (*path == '/') path++;
    if (!*path) {
        /* mount root directory */
        out->type = V_DIR;
        out->size = 0;
        out->ops = &fat32_ops;
        out->mnt_data = (void *)1;
        out->fs_data = kmalloc(sizeof(struct resolve));
        if (!out->fs_data) return LNXRM_ENOMEM; /* T-004: NULL is real now */
        memset(out->fs_data, 0, sizeof(struct resolve));
        ((struct resolve *)out->fs_data)->dir_cluster = m->root_cluster;
        ((struct resolve *)out->fs_data)->de.attr = ATTR_DIR;
        ((struct resolve *)out->fs_data)->de.fstcluslo = m->root_cluster & 0xFFFF;
        ((struct resolve *)out->fs_data)->de.fstclushi = m->root_cluster >> 16;
        return 0;
    }
    int rc = fat_resolve_path(m, path, &r);
    if (rc < 0) return rc;   /* T-005: an unreadable directory is EIO */
    if (!r.found) return LNXRM_EFAIL;
    out->fs_data = kmalloc(sizeof(struct resolve));
    if (!out->fs_data) return LNXRM_ENOMEM;
    memcpy(out->fs_data, &r, sizeof(r));
    out->ops = &fat32_ops;
    out->mnt_data = (void *)1;
    if (r.de.attr & ATTR_DIR) {
        out->type = V_DIR;
        out->size = 0;
    } else {
        out->type = V_REG;
        out->size = r.de.filesize;
    }
    return 0;
}

int fat_read_impl(void *mnt, void *node, u64 off, void *ubuf, size_t n)
{
    struct fat_mount *m = fat_priv;
    struct resolve *r = node;
    if (r->de.attr & ATTR_DIR) return LNXRM_EFAIL;

    /* Validate buffer pointer */
    if (!ubuf && n > 0) return LNXRM_EFAIL;

    u32 clus = ((u32)r->de.fstclushi << 16) | r->de.fstcluslo;
    u64 size = r->de.filesize;
    if (off >= size) return 0;
    if (off + n > size) n = size - off;

    /* Limit read size to prevent buffer overflow */
    if (n > 1024 * 1024) /* Max 1MB per read */
        n = 1024 * 1024;

    u8 tmp[512];
    u64 done = 0;
    /* skip to offset; one chain cursor with one shared step budget covers
     * both the skip and the read, so a forged chain cannot walk the skip
     * and then walk the read for free */
    u64 skip = off;
    struct fat_chain ch;
    fat_chain_start(&ch, m, clus);
    while (skip >= m->cluster_size && fat_chain_more(&ch)) {
        skip -= m->cluster_size;
        fat_chain_advance(&ch);
    }
    u32 in_clus = skip % m->cluster_size;
    while (done < n && fat_chain_more(&ch)) {
        clus = ch.clus;
        u64 sec_off = in_clus % m->bytes_per_sector;
        u64 chunk = MIN(n - done, m->bytes_per_sector - sec_off);

        /* T-005: a failed read is not EOF.  Returning `done` (0) here would
         * tell the caller the file ends where the disk stopped answering. */
        u64 lba = cluster_lba(m, clus) + in_clus / m->bytes_per_sector;
        if (fat_read_sector(m, lba, tmp)) {
            fat_warn_io(m, "read data", lba);
            return done ? (int)done : LNXRM_EIO;
        }
        memcpy((u8 *)ubuf + done, tmp + sec_off, chunk);
        done += chunk;
        in_clus += chunk;
        /* advance to the next cluster only after the current one is
         * fully consumed -- never skip its remaining sectors */
        if (in_clus >= m->cluster_size) {
            in_clus = 0;
            fat_chain_advance(&ch);
        }
    }
    return done;
}

/* grow `first` so it covers need_bytes; returns (possibly new) head.
 * *rc tells the two failures apart: LNXRM_ENOSPC when the volume is full,
 * LNXRM_EIO when a FAT update could not reach the disk.  Either way nothing
 * half-built is published: clusters linked so far stay linked (they are
 * valid chain tail, just more of the file than was asked for) and clusters
 * whose link-up failed are leaked, never cross-linked.
 * *grown tells the caller which clusters this call linked on (the first
 * one freshly allocated -- 0 when nothing grew); *tail the cluster that
 * used to end the existing chain (0 when the chain was fresh) and *tail_old
 * what its FAT entry held before this call touched it.  Together they are
 * what rollback_growth() needs to undo the extension.
 * C16: *rc is LNXRM_ENOSPC both when the volume is full and when the demand
 * reaches past the biggest chain this volume can legally hold (refused
 * before any allocation). */
static u32 alloc_chain(struct fat_mount *m, u32 first, u64 need_bytes, int *rc, u32 *grown,
                       u32 *tail, u32 *tail_old)
{
    /* C16: the demand is u64 and the ceiling keeps the quotient inside
     * u32 -- a 2 TiB+ offset used to wrap nclus into a tiny count and
     * silently corrupt the allocation decision. */
    if (need_bytes > fat_chain_cap_bytes(m)) {
        *rc = LNXRM_ENOSPC;
        return 0;
    }
    u32 nclus = (u32)((need_bytes + m->cluster_size - 1) / m->cluster_size);
    u32 head = first, prev = 0, cnt = 0;
    *rc = 0;
    *grown = 0;
    *tail = 0;
    *tail_old = 0;

    if (first && cluster_is_eoc(first))
        ;
    else if (first) {
        struct fat_chain ch;
        fat_chain_start(&ch, m, first);
        while (fat_chain_more(&ch)) {
            prev = ch.clus;
            fat_chain_advance(&ch);
            cnt++;
        }
        /* whatever sat at the tail is what the caller restores on failure --
         * normally the end marker, but a tail left free (its record died)
         * belongs back at free, not marked as used-and-unclaimed */
        if (prev) *tail_old = *(u32 *)&m->fat[prev * 4];
    } else
        cnt = 0; /* fresh chain */

    while (cnt < nclus) {
        u32 nc = fat_find_free_cluster(m);
        if (!nc) {
            *rc = LNXRM_ENOSPC;
            return 0; /* disk full */
        }
        if (fat_set_entry(m, nc, 0x0FFFFFFF)) {
            *rc = LNXRM_EIO;
            return 0;
        }
        if (!*grown) {
            *grown = nc;
            *tail = prev;
        }
        if (prev) {
            if (fat_set_entry(m, prev, nc)) {
                *rc = LNXRM_EIO;
                return 0;
            }
        } else
            head = nc;
        prev = nc;
        cnt++;
    }
    return head;
}

/* C20: a write whose directory record could not be verified or rewritten
 * must not leave the clusters it booked dangling behind an entry nobody
 * re-found: put the old chain back the way alloc_chain() moved it (restore
 * the tail's previous answer, unlink this call's growth), then return every
 * new cluster to the free pool.  Frees, never cross-links -- a dirent that
 * still names the old head keeps pointing at exactly the data it published.
 * Best effort, like every other failure-path cleanup: m->fat is the
 * authority the next allocation scan reads, and each step is reported by
 * fat_set_entry() on its own. */
static void rollback_growth(struct fat_mount *m, u32 grown, u32 tail, u32 tail_old)
{
    if (!grown) return;
    if (tail) fat_set_entry(m, tail, tail_old);
    struct fat_chain ch;
    for (fat_chain_start(&ch, m, grown); fat_chain_more(&ch);) {
        u32 clus = ch.clus;
        fat_chain_advance(&ch); /* next hop must be read before this one is freed */
        fat_set_entry(m, clus, 0);
    }
}

/* C21: rewrite the record `r` resolves to exactly where it physically sits
 * -- one direct read-modify-write at (containing cluster, byte offset),
 * never a directory scan that two files sharing an 8.3 alias can both win.
 * The creation stamp beyond the 11-byte alias is the incarnation check: an
 * unlink followed by a re-create of the same name hands the position to a
 * stranger, and a record that is no longer ours must not be rewritten.
 * With `trunc` the record is emptied instead of carrying the new size (C19).
 * Returns 0 or LNXRM_EIO. */
static int dirent_writeback_at(struct fat_mount *m, const struct resolve *r, bool trunc)
{
    if (r->ent_clus < 2 || r->ent_off >= m->cluster_size) {
        kprintf("[fat32] open file without a dirent position: write refused\n");
        return LNXRM_EIO;
    }
    u64 lba = cluster_lba(m, r->ent_clus) + r->ent_off / m->bytes_per_sector;
    u32 doff = r->ent_off % m->bytes_per_sector;
    u8 buf[512];
    if (fat_read_sector(m, lba, buf)) {
        fat_warn_io(m, "read dir", lba);
        return LNXRM_EIO;
    }
    struct fat_dirent *e = (struct fat_dirent *)(buf + doff);
    if (!entry_used(e) || memcmp(e->name, r->de.name, 11) ||
        e->crttime != r->de.crttime || e->crtdate != r->de.crtdate ||
        e->crttenth != r->de.crttenth) {
        kprintf("[fat32] record under an open file is not the one that was "
                "resolved (renamed, unlinked or re-created), LBA %llu: write refused\n",
                lba);
        return LNXRM_EIO;
    }
    if (trunc) {
        e->filesize = 0;
        e->fstclushi = 0;
        e->fstcluslo = 0;
    } else {
        e->filesize = r->de.filesize;
        e->fstclushi = r->de.fstclushi;
        e->fstcluslo = r->de.fstcluslo;
    }
    e->attr |= ATTR_ARCHIVE;
    if (fat_write_sector(m, lba, buf)) {
        fat_warn_io(m, "write dir", lba);
        return LNXRM_EIO;
    }
    return 0;
}

int fat_write_impl(void *mnt, void *node, u64 off, const void *buf, size_t n)
{
    struct fat_mount *m = fat_priv;
    struct resolve *r = node;
    if (r->de.attr & ATTR_DIR) return LNXRM_EFAIL;
    if (!buf && !n) {
        /* C19: O_TRUNC used to zero only the in-memory copy -- the on-disk
         * record kept its size and its chain, so a re-open found the old
         * content still sitting in permanently allocated clusters.  Now
         * the record on disk is emptied first and the chain is freed
         * second -- dirent-first is unlink's own order, so a failure here
         * can only leak clusters, never cross-link them -- all inside the
         * transaction fat_write() opened, so a crash between the two is
         * replayed by the journal, not split. */
        u32 first = ((u32)r->de.fstclushi << 16) | r->de.fstcluslo;
        int rc = dirent_writeback_at(m, r, true);
        if (rc) return rc;
        struct fat_chain ch;
        for (fat_chain_start(&ch, m, first); fat_chain_more(&ch);) {
            u32 clus = ch.clus;
            fat_chain_advance(&ch); /* next hop must be read before this one is freed */
            if (fat_set_entry(m, clus, 0))
                return LNXRM_EIO; /* record already emptied: clusters leak */
        }
        r->de.filesize = 0;
        r->de.fstclushi = 0;
        r->de.fstcluslo = 0;
        return 0;
    }

    /* Validate buffer pointer */
    if (!buf && n > 0) return LNXRM_EFAIL;

    /* C35: a zero-byte write to an empty file used to walk alloc_chain and
     * answer EIO ("no clusters" mistaken for disk full); zero bytes never
     * need a chain and never change the record. */
    if (n == 0) return 0;

    /* Limit write size to prevent buffer overflow */
    if (n > 1024 * 1024) /* Max 1MB per write */
        n = 1024 * 1024;

    u32 clus = ((u32)r->de.fstclushi << 16) | r->de.fstcluslo;
    /* C16: combine the demand with an unsigned-wrap check -- `off + n`
     * wrapping to a small number would slip a huge write past the
     * allocation ceiling disguised as a tiny one. */
    u64 end = off + n; /* unsigned wrap is defined; caught next */
    if (end < off) return LNXRM_ENOSPC;
    u64 need = MAX(end, r->de.filesize);
    u32 old_hi = r->de.fstclushi, old_lo = r->de.fstcluslo;
    u64 old_size = r->de.filesize;
    u32 grown = 0, tail = 0, tail_old = 0;
    int rc = 0;
    clus = alloc_chain(m, clus, need, &rc, &grown, &tail, &tail_old);
    if (!clus) return rc ? rc : LNXRM_EIO;
    r->de.fstclushi = clus >> 16;
    r->de.fstcluslo = clus & 0xFFFF;

    u64 done = 0;
    u64 skip = off;
    u32 c = clus;
    struct fat_chain ch;
    fat_chain_start(&ch, m, c);
    while (skip >= m->cluster_size && fat_chain_more(&ch)) {
        skip -= m->cluster_size;
        fat_chain_advance(&ch);
    }
    u32 in_clus = skip % m->cluster_size;
    u8 tmp[512];
    while (done < n && fat_chain_more(&ch)) {
        c = ch.clus;
        u64 lba = cluster_lba(m, c) + in_clus / m->bytes_per_sector;
        u64 sec_off = in_clus % m->bytes_per_sector;
        u64 chunk = MIN(n - done, m->bytes_per_sector - sec_off);

        /* Read-modify-write of a partial sector.  T-005: the read is not
         * optional -- merging the new bytes into an unread buffer writes
         * whatever this stack held (C15: uninitialised kernel stack bytes
         * committed as file data), and an unwritten "successful" sector is
         * silent data loss. */
        if (fat_read_sector(m, lba, tmp)) {
            fat_warn_io(m, "read data", lba);
            rc = LNXRM_EIO;
            break;
        }
        memcpy(tmp + sec_off, (const u8 *)buf + done, chunk);
        /* File data, not metadata: this bypasses the journal and goes to
         * the write-back cache, which fat_journal_commit() flushes *before*
         * it stamps the commit -- ordered mode.  Logging data bytes too
         * would double every write and buy no structural consistency; the
         * FAT chain and the dirent below are what keep the file coherent. */
        if (fat_write_sector_raw(m, lba, tmp)) {
            fat_warn_io(m, "write data", lba);
            rc = LNXRM_EIO;
            break;
        }

        done += chunk;
        in_clus += chunk;
        if (in_clus >= m->cluster_size) {
            in_clus = 0;
            fat_chain_advance(&ch);
        }
    }

    /* Not one byte landed: the growth booked above is handed back and the
     * vnode keeps its pre-write shape -- never publish what did not land. */
    if (rc) {
        rollback_growth(m, grown, tail, tail_old);
        r->de.fstclushi = old_hi;
        r->de.fstcluslo = old_lo;
        return rc;
    }

    /* The size published above is derived from `done`, never from the
     * request: a write that stopped half way must not claim the rest. */
    if (off + done > r->de.filesize) r->de.filesize = off + done;

    /* C21: address the record where it physically sits (position + creation
     * stamp), not by an 8.3 alias two files can share.  C20: a record that
     * is no longer ours (unlinked or re-created under this open file) or
     * that cannot be rewritten is an error -- the caller is told instead of
     * being handed a success over data whose metadata never appeared, and
     * this write's growth is rolled back so nothing ends up unreachable. */
    int prc = dirent_writeback_at(m, r, false);
    if (prc) {
        rollback_growth(m, grown, tail, tail_old);
        r->de.fstclushi = old_hi;
        r->de.fstcluslo = old_lo;
        r->de.filesize = old_size;
        return prc;
    }
    return (int)done;
}

/* C41: O_APPEND appends to the size the record has NOW, not the one the
 * open saw: the dirent at the resolved position is the truth.  A record
 * that cannot be read answers the snapshot -- the caller can still write
 * with what it knows. */
u64 fat_size_impl(void *mnt, void *node)
{
    struct fat_mount *m = fat_priv;
    struct resolve *r = node;
    u8 buf[512];
    u64 lba = cluster_lba(m, r->ent_clus) + r->ent_off / m->bytes_per_sector;
    if (fat_read_sector(m, lba, buf)) return r->de.filesize;
    struct fat_dirent *e = (struct fat_dirent *)(buf + r->ent_off % m->bytes_per_sector);
    return e->filesize;
}

/* C40: the fs node a lookup handed out is the fs's to release -- the VFS
 * close path calls this instead of guessing at heap ownership (an
 * initramfs node, for instance, is a slice of one array). */
void fat_close_node_impl(void *mnt, void *node)
{
    (void)mnt;
    kfree(node);
}
