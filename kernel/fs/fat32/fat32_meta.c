/* FAT32 namespace layer: create / mkdir / rmdir / unlink, including the
 * parent-directory split and the free-dirent claim helper. */
#include "fat32_priv.h"

/* Split "parent/name" for a create, resolve the parent directory cluster
 * and reject the name if it is already taken.
 * pd->name points into pd->full, so the struct must outlive its users. */
struct parent_dir {
    char full[128];
    const char *name;
    u32 cluster;
};

static int parent_dir_init(struct fat_mount *m, const char *path, struct parent_dir *pd)
{
    strncpy(pd->full, path, sizeof(pd->full) - 1);
    pd->full[sizeof(pd->full) - 1] = 0;

    /* find parent directory cluster */
    char parent[128] = "";
    char *slash = strrchr(pd->full, '/');
    pd->name = slash ? slash + 1 : pd->full;
    if (slash) {
        size_t pl = slash - pd->full;
        if (pl >= sizeof(parent)) return LNXRM_EFAIL;
        memcpy(parent, pd->full, pl);
        parent[pl] = 0;
    }

    if (!parent[0]) {
        pd->cluster = m->root_cluster;
    } else {
        struct resolve rr;
        int rc = fat_resolve_path(m, parent, &rr);
        if (rc < 0) return rc; /* T-005: unreadable parent, not "missing" */
        if (!rr.found || !(rr.de.attr & ATTR_DIR)) return LNXRM_EFAIL;
        pd->cluster = ((u32)rr.de.fstclushi << 16) | rr.de.fstcluslo;
    }
    if (!pd->name[0]) return LNXRM_EFAIL;

    /* refuse duplicates */
    struct find_ctx fc = {.want = pd->name};
    int seen = fat_scan_dir(m, pd->cluster, fat_scan_find_cb, &fc);
    if (seen < 0) return seen; /* ENOMEM from a dry heap, not "duplicate" */
    if (seen) return LNXRM_EEXIST;
    return 0;
}

/* Claim a free dirent slot in the parent chain and fill it in.
 * Returns 0, LNXRM_EFAIL when the directory is full, or LNXRM_EIO when a
 * sector could not be read or written (T-005): the buffer handed to the
 * writer must always be a sector that was really read, or the entry written
 * is whatever happened to be on this stack. */
static int parent_entry_add(struct fat_mount *m, u32 pdir, const char *name, u8 attr,
                            u32 first_clus)
{
    u8 pbuf[512];

    struct fat_chain ch;
    for (fat_chain_start(&ch, m, pdir); fat_chain_more(&ch); fat_chain_advance(&ch)) {
        u32 cc = ch.clus;
        for (u32 s = 0; s < m->sectors_per_cluster; s++) {
            u64 lba = cluster_lba(m, cc) + s;
            int rc = fat_read_sector(m, lba, pbuf);
            if (rc) {
                fat_warn_io(m, "read dir", lba);
                return rc;
            }
            for (u32 doff = 0; doff <= m->bytes_per_sector - DIRENT_SIZE; doff += DIRENT_SIZE) {
                struct fat_dirent *e = (struct fat_dirent *)(pbuf + doff);
                if (e->name[0] != ENT_END && e->name[0] != ENT_FREE && e->name[0] != ENT_E5)
                    continue;
                memset(e, 0, DIRENT_SIZE);
                name_to_short(name, e->name);
                /* Case for the alias we just wrote: without it a file created
                 * as "st_f" would come back as "ST_F" now that the reader
                 * honours the bits instead of lowercasing everything. */
                e->ntres = fat_short_ntres(name);
                e->attr = attr;
                e->crttime = 0x6000;
                /* FAT date 0x5A21 = 2025-01-01: the old 0x5A00 encoded the
                 * month and day as 0 (invalid), which mtools shows as
                 * "2025-00-00" on every file this kernel creates.  Time stays
                 * 12:00:00 (0x6000). */
                e->crtdate = 0x5A21;
                e->wrttime = 0x6000;
                e->wrtdate = 0x5A21;
                e->fstcluslo = first_clus & 0xFFFF;
                e->fstclushi = first_clus >> 16;
                e->filesize = 0;

                rc = fat_write_sector(m, lba, pbuf);
                if (rc) {
                    fat_warn_io(m, "write dir", lba);
                    return rc;
                }
                return 0;
            }
        }
    }
    return LNXRM_EFAIL;
}

/* ---------------- directory record primitives ---------------- */

#define LFN_ENTRIES_MAX 20 /* FAT caps a name at 20 * 13 = 260 chars */

struct dirent_pos {
    u64 lba; /* sector holding the record */
    u32 off; /* byte offset of the record inside that sector */
};

/* Where a name lives in a directory: its 8.3 record plus the long-name
 * records stacked in front of it (lfn[n-1] carries sequence number 1). */
struct dirent_loc {
    struct dirent_pos pos;
    struct fat_dirent de;
    int n_lfn;
    struct dirent_pos lfn[LFN_ENTRIES_MAX];
};

/* Read-modify-write a single 32-byte directory record.
 * T-005: every step is checked and reported as LNXRM_EIO -- this is the
 * exact shape of the C15 defect (ignore the read, then commit the buffer),
 * so a failure here must stop the caller rather than publish the sector's
 * uninitialised twin. */
static int dirent_write(struct fat_mount *m, struct dirent_pos p, const struct fat_dirent *de)
{
    u8 buf[512];
    int rc = fat_read_sector(m, p.lba, buf);
    if (rc) {
        fat_warn_io(m, "read dir", p.lba);
        return rc;
    }
    memcpy(buf + p.off, de, DIRENT_SIZE);
    rc = fat_write_sector(m, p.lba, buf);
    if (rc) fat_warn_io(m, "write dir", p.lba);
    return rc;
}

static int dirent_free(struct fat_mount *m, struct dirent_pos p)
{
    u8 buf[512];
    int rc = fat_read_sector(m, p.lba, buf);
    if (rc) {
        fat_warn_io(m, "read dir", p.lba);
        return rc;
    }
    buf[p.off] = ENT_E5;
    rc = fat_write_sector(m, p.lba, buf);
    if (rc) fat_warn_io(m, "write dir", p.lba);
    return rc;
}

/* Drop a name together with the long-name records in front of it; leaving
 * those behind would make the *next* entry of the directory inherit them.
 * Stops at the first failure: writing E5 over the records after a failed
 * one would strand a half-deleted name on disk, and the caller is told so
 * it can leave the cluster chain alone (an entry that still exists with a
 * stale chain is recoverable; a freed chain behind a live entry is not). */
static int free_dirent_record(struct fat_mount *m, const struct dirent_loc *loc)
{
    int rc = 0;
    for (int i = 0; i < loc->n_lfn && !rc; i++) rc = dirent_free(m, loc->lfn[i]);
    if (!rc) rc = dirent_free(m, loc->pos);
    return rc;
}

/* Find the 8.3 record `sn` in `dirclus` and remember the LFN records in
 * front of it.  Returns 1 when found, 0 when the directory does not contain
 * it, or a negative LNXRM error.  T-005: an unreadable cluster must not be
 * reported as "no such name" -- every caller turns that answer into a
 * destructive action (unlink/rmdir/rename) or into ENOENT for a file that
 * is still there. */
static int dirent_locate(struct fat_mount *m, u32 dirclus, const u8 sn[11],
                         struct dirent_loc *out)
{
    u8 *buf = cluster_buf_alloc(m);
    if (!buf) return LNXRM_ENOMEM;
    struct dirent_pos run[LFN_ENTRIES_MAX];
    int nrun = 0;
    bool found = false;
    int rc = 0;

    memset(out, 0, sizeof(*out));
    struct fat_chain ch;
    for (fat_chain_start(&ch, m, dirclus); !found && fat_chain_more(&ch); fat_chain_advance(&ch)) {
        u32 c = ch.clus;
        u64 clba = cluster_lba(m, c);
        rc = fat_read_cluster(m, clba, buf);
        if (rc) {
            fat_warn_io(m, "read dir", clba);
            goto out;
        }
        for (u32 off = 0; off < m->cluster_size; off += DIRENT_SIZE) {
            struct fat_dirent *e = (struct fat_dirent *)(buf + off);
            if (e->name[0] == ENT_FREE) goto out;
            if (!entry_used(e)) {
                nrun = 0; /* deleted record: any run around it is broken */
                continue;
            }
            struct dirent_pos p = {clba + off / m->bytes_per_sector,
                                   off % m->bytes_per_sector};
            if ((e->attr & ATTR_LFN) == ATTR_LFN) {
                if (nrun < LFN_ENTRIES_MAX) run[nrun] = p;
                nrun++;
                continue;
            }
            if (!memcmp(e->name, sn, 11)) {
                out->pos = p;
                out->de = *e;
                out->n_lfn = nrun > LFN_ENTRIES_MAX ? LFN_ENTRIES_MAX : nrun;
                memcpy(out->lfn, run, (size_t)out->n_lfn * sizeof(run[0]));
                found = true;
                break;
            }
            nrun = 0;
        }
    }
out:
    kfree(buf);
    if (rc) return rc;
    return found ? 1 : 0;
}

/* Claim `need` consecutive free records inside `dirclus`.  Reused 0xE5
 * slots first, then the 0x00 tail past the end-of-directory marker.
 * This driver never grows a directory chain, so a full directory reports
 * ENOSPC (the same limit parent_entry_add() hits). */
static int dir_claim_run(struct fat_mount *m, u32 dirclus, int need, struct dirent_pos *out)
{
    if (need <= 0 || need > LFN_ENTRIES_MAX + 1) return LNXRM_EINVAL;
    u8 *buf = cluster_buf_alloc(m);
    if (!buf) return LNXRM_ENOMEM;

    int nrun = 0;
    bool in_tail = false;
    struct fat_chain ch;
    for (fat_chain_start(&ch, m, dirclus);
         nrun < need && fat_chain_more(&ch);
         fat_chain_advance(&ch)) {
        u32 c = ch.clus;
        u64 clba = cluster_lba(m, c);
        int rc = fat_read_cluster(m, clba, buf);
        if (rc) { /* T-005: never claim slots out of an unread cluster */
            fat_warn_io(m, "read dir", clba);
            kfree(buf);
            return rc;
        }
        for (u32 off = 0; off < m->cluster_size && nrun < need; off += DIRENT_SIZE) {
            struct fat_dirent *e = (struct fat_dirent *)(buf + off);
            if (in_tail) {
                if (e->name[0] != ENT_FREE) {
                    kfree(buf);
                    return LNXRM_ENOSPC;
                }
            } else if (e->name[0] == ENT_FREE) {
                in_tail = true; /* everything from here on is free space */
            } else if (e->name[0] != ENT_E5) {
                nrun = 0; /* a live record ends the run */
                continue;
            }
            if (nrun < need)
                out[nrun] = (struct dirent_pos){clba + off / m->bytes_per_sector,
                                                off % m->bytes_per_sector};
            nrun++;
        }
    }
    kfree(buf);
    return nrun >= need ? 0 : LNXRM_ENOSPC;
}

int fat_create_impl(void *mnt, const char *path)
{
    struct fat_mount *m = fat_priv;
    struct parent_dir pd;
    int rc = parent_dir_init(m, path, &pd);
    if (rc) return rc;

    rc = parent_entry_add(m, pd.cluster, pd.name, ATTR_ARCHIVE, 0);
    if (rc) {
        /* said out loud which of the two it was: "no slot" is the caller's
         * fault, an I/O error is the disk's, and hiding the difference is
         * what made this return success over a write that never happened */
        kprintf("[fat32] create '%s': %s\n", pd.name,
                rc == LNXRM_EIO ? "I/O error, no entry written" : "no free dirent slot");
        return rc;
    }
    return 0;
}
/* Value a ".." record stores for `parent`: the root is recorded as cluster
 * 0, not as the root's real cluster.  mtools' mmd writes 0 and fsck.fat
 * demands it -- storing the root cluster (2) instead gets every mkdir'ed
 * directory flagged with "Invalid '..' entry in the second slot".  Readers
 * are unaffected: dir_dotdot()/dir_contains() already treat 0 as "top of
 * the chain". */
static u32 dotdot_store(struct fat_mount *m, u32 parent)
{
    return parent == m->root_cluster ? 0 : parent;
}

int fat_mkdir_impl(void *mnt, const char *path)
{
    struct fat_mount *m = fat_priv;
    struct parent_dir pd;
    int rc = parent_dir_init(m, path, &pd);
    if (rc) return rc;

    /* allocate a cluster for the new directory content */
    u32 new_clus = fat_find_free_cluster(m);
    if (!new_clus) return LNXRM_ENOSPC; /* ENOSPC */
    if (fat_set_entry(m, new_clus, 0x0FFFFFFF)) /* mark end of chain */
        return LNXRM_EIO;

    /* write "." and ".." entries to the new cluster */
    u8 dir_buf[512];
    memset(dir_buf, 0, sizeof(dir_buf));
    struct fat_dirent *dot = (struct fat_dirent *)dir_buf;
    /* "." entry */
    dot->name[0] = '.';
    memset(dot->name + 1, ' ', 10);
    dot->attr = ATTR_DIR;
    dot->fstcluslo = new_clus & 0xFFFF;
    dot->fstclushi = new_clus >> 16;
    dot->filesize = 0;
    /* ".." entry */
    struct fat_dirent *dotdot = (struct fat_dirent *)(dir_buf + DIRENT_SIZE);
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';
    memset(dotdot->name + 2, ' ', 9);
    dotdot->attr = ATTR_DIR;
    u32 dd = dotdot_store(m, pd.cluster);
    dotdot->fstcluslo = dd & 0xFFFF;
    dotdot->fstclushi = dd >> 16;
    dotdot->filesize = 0;

    /* write the first sector of the new directory cluster */
    u64 new_lba = cluster_lba(m, new_clus);
    if (fat_write_sector(m, new_lba, dir_buf)) {
        fat_warn_io(m, "write dir", new_lba);
        goto free_cluster;
    }

    /* zero remaining sectors of the cluster */
    memset(dir_buf, 0, sizeof(dir_buf));
    for (u32 s = 1; s < m->sectors_per_cluster; s++) {
        if (fat_write_sector(m, new_lba + s, dir_buf)) {
            fat_warn_io(m, "write dir", new_lba + s);
            goto free_cluster;
        }
    }

    /* create directory entry in parent */
    rc = parent_entry_add(m, pd.cluster, pd.name, ATTR_DIR | ATTR_ARCHIVE, new_clus);
    if (rc) {
        /* no free slot, or the parent could not be written -- free the
         * cluster we allocated, or it is leaked as "used but unreachable" */
        kprintf("[fat32] mkdir '%s': %s\n", pd.name,
                rc == LNXRM_EIO ? "I/O error, no entry written" : "no free dirent slot");
        fat_set_entry(m, new_clus, 0); /* best effort: nothing points at it yet */
        return rc;
    }
    return 0;

free_cluster: /* the content of the new directory could not be written */
    fat_set_entry(m, new_clus, 0);
    return LNXRM_EIO;
}

int fat_rmdir_impl(void *mnt, const char *path)
{
    struct fat_mount *m = fat_priv;
    struct resolve r;
    int rc = fat_resolve_path(m, path, &r);
    if (rc < 0) return rc; /* T-005 */
    if (!r.found) return LNXRM_ENOENT;
    if (!(r.de.attr & ATTR_DIR)) return LNXRM_ENOTDIR; /* ENOTDIR */

    u32 dir_clus = ((u32)r.de.fstclushi << 16) | r.de.fstcluslo;
    if (dir_clus < 2) return LNXRM_EFAIL;

    /* check directory is empty (only . and ..) */
    u8 dbuf[512];
    int entry_count = 0;
    struct fat_chain ch;
    for (fat_chain_start(&ch, m, dir_clus); fat_chain_more(&ch); fat_chain_advance(&ch)) {
        u32 cc = ch.clus;
        for (u32 s = 0; s < m->sectors_per_cluster; s++) {
            u64 lba = cluster_lba(m, cc) + s;
            rc = fat_read_sector(m, lba, dbuf);
            if (rc) { /* never call a directory empty because it would not read */
                fat_warn_io(m, "read dir", lba);
                return rc;
            }
            for (u32 doff = 0; doff <= m->bytes_per_sector - DIRENT_SIZE; doff += DIRENT_SIZE) {
                struct fat_dirent *e = (struct fat_dirent *)(dbuf + doff);
                if (e->name[0] == ENT_END) goto done_check;
                if (e->name[0] == ENT_FREE || e->name[0] == ENT_E5) continue;
                entry_count++;
                /* . and .. are allowed */
                if (e->name[0] == '.' && (e->name[1] == ' ' || e->name[1] == '.')) continue;
                return LNXRM_ENOTEMPTY; /* ENOTEMPTY */
            }
        }
    }
done_check:
    if (entry_count < 2) return LNXRM_EFAIL;

    struct dirent_loc loc;
    rc = dirent_locate(m, r.dir_cluster, r.de.name, &loc);
    if (rc < 0) return rc;
    if (!rc) return LNXRM_ENOENT;

    /* Remove the parent directory entry FIRST, then free the chain --
     * see fat_unlink() for the rationale (avoids cross-linked clusters).
     * A failure here stops before the chain is touched: an entry that still
     * exists over a live chain is a normal filesystem, the reverse is not. */
    rc = free_dirent_record(m, &loc);
    if (rc) return rc;

    struct fat_chain chf;
    fat_chain_start(&chf, m, dir_clus);
    while (fat_chain_more(&chf)) {
        u32 clus = chf.clus;
        fat_chain_advance(&chf); /* next cluster must be read before this one is freed */
        if (fat_set_entry(m, clus, 0))
            return LNXRM_EIO; /* already reported; clusters leak, none cross-link */
    }
    return 0;
}

int fat_unlink_impl(void *mnt, const char *path)
{
    struct fat_mount *m = fat_priv;
    struct resolve r;
    int rc = fat_resolve_path(m, path, &r);
    if (rc < 0) return rc; /* T-005: EIO, not "already gone" */
    if (!r.found) return LNXRM_ENOENT;
    if (r.de.attr & ATTR_DIR) return LNXRM_EFAIL;

    struct dirent_loc loc;
    rc = dirent_locate(m, r.dir_cluster, r.de.name, &loc);
    if (rc < 0) return rc;
    if (!rc) return LNXRM_ENOENT;

    /* Start a transaction for atomic updates */

    /* Remove the directory entry FIRST, then free the cluster chain.
     * Reversing the order meant a failed dirent lookup left the chain
     * already freed while the dirent still pointed at it -> cross-linked
     * clusters shared by two files. */
    rc = free_dirent_record(m, &loc);
    if (rc) return rc; /* entry (partly) there, chain untouched */

    u32 first = ((u32)r.de.fstclushi << 16) | r.de.fstcluslo;
    struct fat_chain chf;
    fat_chain_start(&chf, m, first);
    while (fat_chain_more(&chf)) {
        u32 clus = chf.clus;
        fat_chain_advance(&chf); /* next cluster must be read before this one is freed */
        if (fat_set_entry(m, clus, 0))
            return LNXRM_EIO; /* already reported; clusters leak, none cross-link */
    }
    return 0;
}

/* ---------------- rename / move ---------------- */

/* How many LFN records `name` needs.  0 when the 8.3 alias plus the NT case
 * bits already render back as `name`, negative when it cannot be stored. */
static int fat_lfn_count(const char *name)
{
    size_t len = strlen(name);
    if (!len || len > 255) return -1;
    u8 alias[11];
    char back[13];
    name_to_short(name, alias);
    fat_short_to_name(alias, fat_short_ntres(name), back);
    /* Case-sensitive on purpose: 8.3 + bits can only spell a part that is
     * uniform in case, so "Hello.txt" must lose the strcmp here and take an
     * LFN record -- accepting it insensitively stored a name that read back
     * as "HELLO.txt". */
    if (!strcmp(back, name)) return 0;
    return (int)((len + 12) / 13);
}

/* FAT LFN checksum over the 11-byte alias (shared by its LFN records). */
static u8 lfn_checksum(const u8 *sn)
{
    u8 sum = 0;
    for (int i = 0; i < 11; i++) sum = (u8)(((sum & 1) << 7) + (sum >> 1) + sn[i]);
    return sum;
}

/* Fill one 32-byte long-name record.  `seq` runs from 1 (immediately in
 * front of the 8.3 record) up to `total`, which carries the 0x40 mark. */
static void lfn_fill(struct fat_dirent *e, int seq, int total, u8 sum, const char *name)
{
    static const int off[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    u8 *raw = (u8 *)e;
    size_t len = strlen(name);

    memset(raw, 0, DIRENT_SIZE);
    raw[0] = (u8)(seq == total ? (seq | 0x40) : seq);
    for (int k = 0; k < 13; k++) {
        size_t idx = (size_t)(seq - 1) * 13 + k;
        u32 ch;
        if (idx < len) ch = (u8)name[idx]; /* ASCII -> UTF-16LE */
        else if (idx == len) ch = 0;       /* terminator right after the name */
        else ch = 0xFFFF;                  /* pad the rest of the record */
        raw[off[k]] = (u8)(ch & 0xFF);
        raw[off[k] + 1] = (u8)(ch >> 8);
    }
    raw[11] = ATTR_LFN;
    raw[12] = 0;  /* type */
    raw[13] = sum; /* checksum of the alias this record belongs to */
    raw[26] = 0;
    raw[27] = 0; /* first cluster: 0 */
}

struct rename_ctx {
    const char *want;
    const u8 *skip; /* alias of the record being renamed (same directory) */
    bool hit;
};

static int rename_exists_cb(const char *name, const struct fat_dirent *e, u32 dirclus, u32 off,
                            void *ctx)
{
    struct rename_ctx *c = ctx;
    if (c->skip && !memcmp(e->name, c->skip, 11)) return 0;
    if (!strcasecmp(name, c->want) || fat_name_eq_short(c->want, e)) {
        c->hit = true;
        return 1;
    }
    return 0;
}

/* The ".." record of a directory cluster.  *out gets the parent cluster,
 * or 0 when the directory has none (the FAT32 root created by mkfs has no
 * dot entries) or when the parent *is* the root, which a ".." record
 * stores as cluster 0 (see dotdot_store).  Returns 0 or a negative LNXRM
 * error: T-005 -- an unreadable sector is *not* "this directory has no
 * parent", because the caller uses that answer to decide whether a move
 * would put a directory inside its own subtree. */
static int dir_dotdot(struct fat_mount *m, u32 clus, u32 *out)
{
    u8 buf[512];
    u64 lba = cluster_lba(m, clus);
    *out = 0;
    int rc = fat_read_sector(m, lba, buf);
    if (rc) {
        fat_warn_io(m, "read dir", lba);
        return rc;
    }
    for (u32 off = 0; off <= m->bytes_per_sector - DIRENT_SIZE; off += DIRENT_SIZE) {
        struct fat_dirent *e = (struct fat_dirent *)(buf + off);
        if (e->name[0] == ENT_FREE) break;
        if (!entry_used(e) || (e->attr & ATTR_LFN) == ATTR_LFN) continue;
        if (e->name[0] == '.' && e->name[1] == '.' && e->name[2] == ' ') {
            *out = ((u32)e->fstclushi << 16) | e->fstcluslo;
            return 0;
        }
        if (e->name[0] == '.' && e->name[1] == ' ') continue;
        break; /* "." always comes first, so there is no parent link here */
    }
    return 0;
}

/* Point a directory's ".." record at its new parent (directories only).
 * Returns 0 (updated, or there is nothing to update) or LNXRM_EIO. */
static int dir_set_dotdot(struct fat_mount *m, u32 clus, u32 parent)
{
    u8 buf[512];
    u64 lba = cluster_lba(m, clus);
    int rc = fat_read_sector(m, lba, buf);
    if (rc) {
        fat_warn_io(m, "read dir", lba);
        return rc;
    }
    for (u32 off = 0; off <= m->bytes_per_sector - DIRENT_SIZE; off += DIRENT_SIZE) {
        struct fat_dirent *e = (struct fat_dirent *)(buf + off);
        if (e->name[0] == ENT_FREE) return 0;
        if (!entry_used(e) || (e->attr & ATTR_LFN) == ATTR_LFN) continue;
        if (e->name[0] == '.' && e->name[1] == '.' && e->name[2] == ' ') {
            u32 dd = dotdot_store(m, parent);
            e->fstcluslo = dd & 0xFFFF;
            e->fstclushi = dd >> 16;
            rc = fat_write_sector(m, lba, buf);
            if (rc) fat_warn_io(m, "write dir", lba);
            return rc;
        }
        if (e->name[0] == '.' && e->name[1] == ' ') continue;
        return 0; /* first record decides ("." / ".." come first) */
    }
    return 0;
}

/* Is `node` the same directory as `dir` or nested inside it?  Walking the
 * ".." chain upward keeps a directory from being moved into its own
 * subtree, which would orphan everything below it.
 * Returns 1 yes / 0 no / negative when a ".." record could not be read --
 * the answer must not be guessed when the walk broke off halfway. */
static int dir_contains(struct fat_mount *m, u32 dir, u32 node)
{
    u32 c = node;
    for (int i = 0; i < 64 && c >= 2 && c < m->max_cluster; i++) {
        if (c == dir) return 1;
        u32 p;
        int rc = dir_dotdot(m, c, &p);
        if (rc) return rc;
        if (p == c || p < 2 || p >= m->max_cluster) return 0;
        c = p;
    }
    return 0;
}

/* Rewrite a record where it already sits, shrinking/growing its LFN run
 * to exactly n_lfn entries (all of them live in the same directory). */
static int rename_in_place(struct fat_mount *m, const struct dirent_loc *loc, int n_lfn,
                           const char *nname, const u8 alias[11], u8 sum)
{
    /* records that are no longer part of the name are deleted ... */
    for (int i = 0; i < loc->n_lfn - n_lfn; i++) {
        int rc = dirent_free(m, loc->lfn[i]);
        if (rc) return rc;
    }
    /* ... the survivors are the last n_lfn, carrying seq n_lfn .. 1 */
    for (int i = 0; i < n_lfn; i++) {
        struct fat_dirent e;
        int rc;
        lfn_fill(&e, n_lfn - i, n_lfn, sum, nname);
        rc = dirent_write(m, loc->lfn[loc->n_lfn - n_lfn + i], &e);
        if (rc) return rc;
    }
    struct fat_dirent rec = loc->de;
    memcpy(rec.name, alias, 11);
    rec.ntres = fat_short_ntres(nname); /* the old bits described the old name */
    return dirent_write(m, loc->pos, &rec);
}

static void path_trim(char *p)
{
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] == '/') p[--n] = 0;
}

/* rename(2)/mv(2) core: relocate the directory record of `oldpath` to
 * `newpath`.  The cluster chain never moves -- only the record does -- so
 * files keep their contents and directories keep their subtree; the ".."
 * link of a moved directory is repointed at the new parent. */
int fat_rename_impl(void *mnt, const char *oldpath, const char *newpath)
{
    struct fat_mount *m = fat_priv;
    (void)mnt;
    char ob[128], nb[128];
    strncpy(ob, oldpath, sizeof(ob) - 1);
    ob[sizeof(ob) - 1] = 0;
    strncpy(nb, newpath, sizeof(nb) - 1);
    nb[sizeof(nb) - 1] = 0;
    path_trim(ob);
    path_trim(nb);

    /* the source must exist (the volume root itself has no record) */
    struct resolve ro;
    memset(&ro, 0, sizeof(ro));
    int res = fat_resolve_path(m, ob, &ro);
    if (res < 0) return res; /* T-005: EIO, not "no such file" */
    if (!ro.found) return LNXRM_ENOENT;
    u32 old_dir = ro.dir_cluster;
    u32 old_clus = ((u32)ro.de.fstclushi << 16) | ro.de.fstcluslo;
    bool is_dir = (ro.de.attr & ATTR_DIR) != 0;

    /* split the destination into parent + final component */
    char pdir[128], nname[56];
    const char *slash = strrchr(nb, '/');
    const char *tail = slash ? slash + 1 : nb;
    if (slash) {
        size_t pl = (size_t)(slash - nb);
        if (pl >= sizeof(pdir)) return LNXRM_EINVAL;
        memcpy(pdir, nb, pl);
        pdir[pl] = 0;
    } else {
        pdir[0] = 0;
    }
    strncpy(nname, tail, sizeof(nname) - 1);
    nname[sizeof(nname) - 1] = 0;
    if (!nname[0]) return LNXRM_EINVAL; /* "" or a bare "/" */
    if (nname[0] == '.' && (!nname[1] || (nname[1] == '.' && !nname[2])))
        return LNXRM_EINVAL; /* "." and ".." are not rename targets */

    /* the destination directory must exist and be a directory */
    u32 new_dir = m->root_cluster;
    if (pdir[0]) {
        struct resolve rp;
        memset(&rp, 0, sizeof(rp));
        res = fat_resolve_path(m, pdir, &rp);
        if (res < 0) return res; /* T-005 */
        if (!rp.found) return LNXRM_ENOENT;
        if (!(rp.de.attr & ATTR_DIR)) return LNXRM_ENOTDIR;
        new_dir = ((u32)rp.de.fstclushi << 16) | rp.de.fstcluslo;
    }

    /* already where it should be? */
    if (old_dir == new_dir && !strcasecmp(ro.name, nname)) return 0;

    /* the destination name must be free (never counting the record we are
     * about to move, which lives in the same directory for a plain rename) */
    struct rename_ctx rc = {.want = nname, .hit = false};
    if (old_dir == new_dir) rc.skip = ro.de.name;
    int scan = fat_scan_dir(m, new_dir, rename_exists_cb, &rc);
    if (scan < 0) return scan; /* T-005: "the name is free" needs the read */
    if (rc.hit) return LNXRM_EEXIST;

    /* a directory may not be moved into itself or its own subtree */
    if (is_dir) {
        int inside = dir_contains(m, old_clus, new_dir);
        if (inside < 0) return inside; /* could not walk the ".." chain */
        if (inside) return LNXRM_EINVAL;
    }

    int n_lfn = fat_lfn_count(nname);
    if (n_lfn < 0) return LNXRM_EINVAL;
    u8 alias[11];
    name_to_short(nname, alias);
    u8 sum = lfn_checksum(alias);

    struct dirent_loc loc;
    int found = dirent_locate(m, old_dir, ro.de.name, &loc);
    if (found < 0) return found;
    if (!found) return LNXRM_ENOENT;

    /* Same directory and the existing long-name run is already big enough:
     * rewrite the record in place, no directory reshuffling needed. */
    if (old_dir == new_dir && n_lfn <= loc.n_lfn)
        return rename_in_place(m, &loc, n_lfn, nname, alias, sum);

    /* Otherwise claim a fresh run of records in the destination directory
     * (LFN records first, then the 8.3 record) and drop the old one. */
    struct dirent_pos slots[LFN_ENTRIES_MAX + 1];
    int err = dir_claim_run(m, new_dir, n_lfn + 1, slots);
    if (err) return err;

    for (int i = 0; i < n_lfn; i++) {
        struct fat_dirent e;
        lfn_fill(&e, n_lfn - i, n_lfn, sum, nname);
        err = dirent_write(m, slots[i], &e);
        if (err) return err; /* the old record is still in place: no data lost */
    }
    struct fat_dirent rec = loc.de;
    memcpy(rec.name, alias, 11);
    rec.ntres = fat_short_ntres(nname);
    err = dirent_write(m, slots[n_lfn], &rec);
    if (err) return err;

    err = free_dirent_record(m, &loc);
    if (err) return err;
    if (is_dir && old_dir != new_dir) {
        err = dir_set_dotdot(m, old_clus, new_dir);
        if (err) return err;
    }
    return 0;
}
