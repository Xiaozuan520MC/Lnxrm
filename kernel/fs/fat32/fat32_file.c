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
 * whose link-up failed are leaked, never cross-linked. */
static u32 alloc_chain(struct fat_mount *m, u32 first, u64 need_bytes, int *rc)
{
    u32 nclus = (need_bytes + m->cluster_size - 1) / m->cluster_size;
    u32 head = first, prev = 0, cnt = 0;
    *rc = 0;

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

int fat_write_impl(void *mnt, void *node, u64 off, const void *buf, size_t n)
{
    struct fat_mount *m = fat_priv;
    struct resolve *r = node;
    if (r->de.attr & ATTR_DIR) return LNXRM_EFAIL;
    if (!buf && !n) { /* truncate request */
        r->de.filesize = 0;
        return 0;
    }

    /* Validate buffer pointer */
    if (!buf && n > 0) return LNXRM_EFAIL;

    /* Limit write size to prevent buffer overflow */
    if (n > 1024 * 1024) /* Max 1MB per write */
        n = 1024 * 1024;

    u32 clus = ((u32)r->de.fstclushi << 16) | r->de.fstcluslo;
    u64 need = MAX(off + n, r->de.filesize);
    int rc = 0;
    clus = alloc_chain(m, clus, need, &rc);
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
        if (fat_write_sector(m, lba, tmp)) {
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

    /* Not one byte landed: leave the metadata byte-for-byte as it was. */
    if (rc && !done) return rc;

    /* The size published above is derived from `done`, never from the
     * request: a write that stopped half way must not claim the rest. */
    if (off + done > r->de.filesize) r->de.filesize = off + done;

    /* update the on-disk dirent: rewrite parent dir sector */
    u8 dbuf[512];
    u32 dc = r->dir_cluster;
    struct fat_chain chd;
    for (fat_chain_start(&chd, m, dc); fat_chain_more(&chd); fat_chain_advance(&chd)) {
        u32 cc = chd.clus;
        for (u32 s = 0; s < m->sectors_per_cluster; s++) {
            u64 lba = cluster_lba(m, cc) + s;
            if (fat_read_sector(m, lba, dbuf)) {
                /* The data reached the disk but its new size cannot: report
                 * the failure instead of letting the caller believe the
                 * write committed. */
                fat_warn_io(m, "read dir", lba);
                return LNXRM_EIO;
            }
            for (u32 doff = 0; doff <= m->bytes_per_sector - DIRENT_SIZE; doff += DIRENT_SIZE) {
                struct fat_dirent *e = (struct fat_dirent *)(dbuf + doff);
                if (e->name[0] == ENT_END) goto done_write;
                if (!entry_used(e)) continue;
                if (!memcmp(e->name, r->de.name, 11)) {
                    e->filesize = r->de.filesize;
                    e->fstclushi = r->de.fstclushi;
                    e->fstcluslo = r->de.fstcluslo;
                    e->attr |= ATTR_ARCHIVE;

                    if (fat_write_sector(m, lba, dbuf)) {
                        fat_warn_io(m, "write dir", lba);
                        return LNXRM_EIO;
                    }
                    /* metadata matches the bytes that landed: a short count
                     * is the honest answer for the partial case above */
                    return (int)done;
                }
            }
        }
    }

done_write:
    return rc ? rc : (int)done;
}
