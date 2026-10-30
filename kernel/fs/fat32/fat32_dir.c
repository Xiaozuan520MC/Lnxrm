/* FAT32 directory layer: 8.3/LFN name handling, directory scanning,
 * path resolution and readdir (fat_dir_iter / getdent). */
#include "fat32_priv.h"

void fat_short_to_name(const u8 *sn, u8 ntres, char out[13])
{
    /* The NT bits -- not a fixed case -- decide each half: "README  MD " with
     * LOWER_EXT is "README.md", "BIN" with LOWER_BASE is "bin".  Old code
     * lowercased unconditionally and threw the bits away, so every file that
     * had no long-name record came out in lower case. */
    bool lower_base = (ntres & FAT_NTRES_LOWER_BASE) != 0;
    bool lower_ext = (ntres & FAT_NTRES_LOWER_EXT) != 0;
    /* A stored first byte of 0x05 carries a literal 0xE5 (spec's 0x05
     * escape); deliver the real character, never the raw byte. */
    u8 snbuf[11];
    if (sn[0] == 0x05) {
        memcpy(snbuf, sn, 11);
        snbuf[0] = 0xE5;
        sn = snbuf;
    }
    int o = 0;
    for (int i = 0; i < 8 && sn[i] != ' '; i++)
        out[o++] = lower_base ? ascii_tolower((char)sn[i]) : (char)sn[i];
    for (int i = 8; i < 11 && sn[i] != ' '; i++) {
        if (i == 8) out[o++] = '.';
        out[o++] = lower_ext ? ascii_tolower((char)sn[i]) : (char)sn[i];
    }
    out[o] = 0;
}

bool fat_name_eq_short(const char *want, const struct fat_dirent *e)
{
    u8 s[11];
    name_to_short(want, s);
    /* Compare as u8 on both sides: e->name[] holds raw bytes, and casting a
     * byte >= 0x80 to (signed) char before ascii_toupper() makes the left
     * side negative while s[i] holds the same byte as u8 -- so a name with
     * any high-bit byte (a non-ASCII 8.3 alias) never matched itself.
     * fat_alias_eq then unwraps the 0x05 escape on both sides. */
    u8 a[11];
    for (int i = 0; i < 11; i++)
        a[i] = (u8)ascii_toupper((char)e->name[i]);
    return fat_alias_eq(a, s);
}

/* iterate dirents of a directory chain with LFN reconstruction.
 * fn returns nonzero to stop. ctx passed through. */
#define LFN_SEQ_MAX 20 /* FAT caps a long name at 20 * 13 = 260 chars */
struct lfn_state {
    /* C31: 261 = 260 name bytes (20 full pieces) + the terminator.  The
     * old 256 made the last piece of a full run unwritable, so names in
     * [257, 260] silently fell back to the 8.3 alias. */
    char longname[261];
    int total;    /* seq of the marked record == number of pieces */
    int expect;   /* seq number the next unmarked record must carry */
    int name_end; /* highest byte written: the real name length */
    u8 sum;       /* alias checksum every record of the run must carry */
};

/* Forget a partially assembled long name.  The whole buffer is zeroed:
 * pieces fill it by (seq-1)*13 offsets, so with the old three-field reset
 * a run broken mid-assembly left stack bytes inside it that the next
 * (shorter) run then read out as name text (C17). */
static void lfn_reset(struct lfn_state *lf)
{
    memset(lf->longname, 0, sizeof(lf->longname));
    lf->total = 0;
    lf->expect = 0;
    lf->name_end = 0;
    lf->sum = 0;
}

/* Directory cursor handed back in the getdent cookie (C30):
 * (cluster << 32) | offset of the *next* record to examine.  DIRIT_START =
 * from the beginning of the directory, DIRIT_END = the listing has ended.
 * The old cookie was a sequential index and made every getdent rescan the
 * directory from its first cluster -- O(n²) device traffic per ls. */

int fat_scan_dir_at(struct fat_mount *m, u32 dirclus, u64 resume, dirent_cb cb, void *ctx)
{
    u8 *buf = cluster_buf_alloc(m);
    if (!buf) return LNXRM_ENOMEM; /* T-004: a dry heap ends the scan */
    struct lfn_state lf;
    lfn_reset(&lf);

    /* C30: sectors, not clusters.  A 32-byte dirent never crosses a
     * 512-byte sector, so the scan steps sector by sector -- a resume
     * reads only the sectors the cursor has not served yet, one lookup
     * each, instead of re-reading whole clusters per call.  m->fat is in
     * RAM, so chain advancement is arithmetic: skipped clusters cost no
     * device read.  LFN state starts fresh at a resume, which is exact,
     * because a long-name run always sits contiguously right before its
     * own 8.3 record -- the next entry's pieces can never lie at or
     * before the cursor. */
    u32 start_clus = (u32)(resume >> 32);
    u32 start_off = (u32)resume;
    bool skipping = resume != DIRIT_START;
    u32 eps = m->bytes_per_sector / DIRENT_SIZE;

    struct fat_chain ch;
    for (fat_chain_start(&ch, m, dirclus); fat_chain_more(&ch); fat_chain_advance(&ch)) {
        u32 c = ch.clus;
        u32 from_sec = 0, first_i = 0;
        if (skipping) {
            if (c != start_clus) continue; /* O(1): no read, no scan */
            skipping = false;
            /* The cursor names a byte offset, so the first sector it still
             * wants is only entered at its own slot -- resuming from 32
             * (second entry) must NOT rescan the sector's first entry, or
             * a 1-sector cluster re-delivers entry 0 forever and never
             * reaches the second one. */
            from_sec = start_off / m->bytes_per_sector; /* may be the end */
            first_i = (start_off % m->bytes_per_sector) / DIRENT_SIZE;
        }
        /* T-005: an unreadable sector ends the scan with an error.  Reading
         * on would walk whatever the buffer held before (uninitialised heap
         * on the first sector) and hand it to `cb` as directory entries --
         * "not found" out of data that was never read. */
        u64 clba = cluster_lba(m, c);
        for (u32 sec = from_sec; sec < m->sectors_per_cluster; sec++) {
            int rc = fat_read_sector(m, clba + sec, buf);
            if (rc) {
                fat_warn_io(m, "read dir", clba + sec);
                kfree(buf);
                return rc;
            }
            for (u32 i = (sec == from_sec ? first_i : 0); i < eps; i++) {
                u32 off = (sec * eps + i) * DIRENT_SIZE;
                struct fat_dirent *e = (struct fat_dirent *)(buf + i * DIRENT_SIZE);
                /* 0x00 terminates the directory (only ever in the tail); a
                 * fifth long-name piece opens with 0x05 and must scan on. */
                if (e->name[0] == ENT_END) goto done;
                if (!entry_used(e)) {
                    /* Deleted entry: any LFN run around it is orphaned. */
                    lfn_reset(&lf);
                    continue;
                }
                if ((e->attr & ATTR_LFN) == ATTR_LFN) {
                    /* LFN entry: assemble UTF16 pieces (stored in reverse
                     * order); access through a raw byte pointer because the
                     * pieces span past the 11-byte short-name array */
                    const u8 *raw = (const u8 *)e;
                    int seq = raw[0] & 0x1F;
                    bool marked = (raw[0] & 0x40) != 0;
                    if (marked) {
                        /* First record of a run (highest seq).  An
                         * unterminated run from before is dropped, never
                         * extended into it. */
                        if (lf.total) lfn_reset(&lf);
                        if (seq >= 1 && seq <= LFN_SEQ_MAX) {
                            lf.total = seq;
                            lf.expect = seq - 1;
                            lf.sum = raw[13];
                        }
                    } else {
                        /* C17: a run only continues with the same alias
                         * checksum and strictly descending sequence
                         * numbers; a hole (or a checksum of a stranger) is
                         * corruption: reset and let the 8.3 record fall
                         * back to its alias. */
                        if (!lf.total || seq < 1 || seq != lf.expect ||
                            lf.sum != raw[13]) {
                            lfn_reset(&lf);
                            continue;
                        }
                        lf.expect = seq - 1;
                    }
                    if (!lf.total) continue;
                    char piece[14];
                    int pi = 0;
                    static const int poff[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
                    for (int k = 0; k < 13; k++) {
                        u16 chr = (u16)(raw[poff[k]] | (raw[poff[k] + 1] << 8));
                        /* 0xFFFF (and 0x0000) pad the final piece. */
                        if (chr == 0xFFFF || chr == 0) break;
                        piece[pi++] = (char)chr;
                    }
                    piece[pi] = 0;
                    size_t base = (size_t)(seq - 1) * 13;
                    if (base + pi >= sizeof(lf.longname)) {
                        lfn_reset(&lf); /* longer than the buffer: use the alias */
                        continue;
                    }
                    memcpy(&lf.longname[base], piece, pi);
                    if ((int)(base + pi) > lf.name_end) lf.name_end = (int)(base + pi);
                    continue;
                }
                if (e->attr & ATTR_VOLUME) continue;

                char shortn[13];
                fat_short_to_name(e->name, e->ntres, shortn);
                const char *nm = shortn;
                char assembled[261]; /* C31: same bound as the piece buffer */
                /* C17: the long name is only trusted when the run is
                 * complete and its checksum matches this very record's
                 * alias. */
                if (lf.total && lf.expect == 0 && lf.name_end > 0 &&
                    lf.sum == fat_lfn_checksum(e->name)) {
                    memcpy(assembled, lf.longname, (size_t)lf.name_end);
                    assembled[lf.name_end] = 0;
                    nm = assembled;
                }
                lfn_reset(&lf);
                if (cb(nm, e, c, off, ctx)) {
                    kfree(buf);
                    return 1;
                }
            }
        }
    }
done:
    kfree(buf);
    return 0;
}

/* Full scan from the directory start -- the name-lookup path. */
int fat_scan_dir(struct fat_mount *m, u32 dirclus, dirent_cb cb, void *ctx)
{ return fat_scan_dir_at(m, dirclus, DIRIT_START, cb, ctx); }

int fat_scan_find_cb(const char *name, const struct fat_dirent *e, u32 dirclus, u32 off,
                     void *ctx)
{
    struct find_ctx *f = ctx;
    if (!strcasecmp(name, f->want) || fat_name_eq_short(f->want, e)) {
        f->found = *e;
        f->dirclus = dirclus;
        f->off = off;
        return 1;
    }
    return 0;
}

/* resolve a path to {parent_dir_cluster, dirent copy, name}.
 * Returns 1 resolved / 0 not found / negative when a directory could not be
 * read -- the third answer did not exist before T-005, and without it an I/O
 * error was indistinguishable from a missing file. */
int fat_resolve_path(struct fat_mount *m, const char *path, struct resolve *r)
{
    /* C34: zero the whole answer up front -- callers pass stack structs and
     * used to be on the hook for clearing them (one of five did), so the
     * uninitialised tail of the name copy leaked caller-stack bytes into
     * every resolve. */
    memset(r, 0, sizeof(*r));
    r->dir_cluster = m->root_cluster;
    while (*path == '/') path++;
    if (!*path) return 1; /* root itself: no record, dir_cluster says home */

    char comp[56];
    const char *p = path;
    u32 cur = m->root_cluster;
    for (;;) {
        const char *sl = strchr(p, '/');
        size_t len = sl ? (size_t)(sl - p) : strlen(p);
        if (len >= sizeof(comp)) return 0;
        memcpy(comp, p, len);
        comp[len] = 0;
        bool last = !sl;

        struct find_ctx fc = {.want = comp};
        /* <= 0 covers "not found" and the error a failed scan returns: a
         * negative value must never be read as "the callback matched" */
        int rc = fat_scan_dir(m, cur, fat_scan_find_cb, &fc);
        if (rc < 0) return rc; /* I/O error: not "missing" */
        if (!rc) return 0;
        if (last) {
            r->de = fc.found;
            r->dir_cluster = cur;
            r->ent_clus = fc.dirclus;
            r->ent_off = fc.off;
            /* C34: full copy, terminator included -- comp was built with
             * one, and len can be exactly sizeof(name)-1 long. */
            memcpy(r->name, comp, len + 1);
            r->found = true;
            return 1;
        }
        if (!(fc.found.attr & ATTR_DIR)) return 0;
        cur = ((u32)fc.found.fstclushi << 16) | fc.found.fstcluslo;
        p = sl + 1;
        while (*p == '/') p++;
        if (!*p) { /* trailing slash: it's this dir */
            r->de = fc.found;
            r->dir_cluster = cur;
            r->ent_clus = fc.dirclus;
            r->ent_off = fc.off;
            r->found = true;
            memcpy(r->name, comp, len + 1); /* C34: terminator included */
            return 1;
        }
    }
}

struct iter_ctx {
    char name[56];
    u8 type;
    u64 ino;
    u32 dirclus, off; /* C30: where the delivered record physically sits */
    bool hit;
};

/* Inode number: (containing cluster << 32) | offset inside that cluster.
 * A FAT32 record lives in exactly one physical cluster at one offset, so
 * the pair is unique across the volume and stable across reboots. */
static u64 fat_ino(u32 dirclus, u32 off)
{ return ((u64)dirclus << 32) | off; }

static int scan_iter_cb(const char *name, const struct fat_dirent *e, u32 dirclus, u32 off,
                        void *v)
{
    struct iter_ctx *c = v;
    if (name[0] == '.') return 0;
    strncpy(c->name, name, sizeof(c->name) - 1);
    c->name[sizeof(c->name) - 1] = 0;
    c->type = (e->attr & ATTR_DIR) ? 4 : 8;
    c->ino = fat_ino(dirclus, off);
    c->dirclus = dirclus;
    c->off = off;
    c->hit = true;
    return 1;
}

/* One readdir step over the physical cursor.  T-005 contract (same
 * three-way answer as fat_scan_dir): 1 = `d` filled, 0 = the directory is
 * exhausted, negative = the read failed and the cookie stays put, so the
 * next call re-reports instead of claiming a clean end. */
int fat_dir_iter(struct dir_iter *it, struct dirent_out *d)
{
    struct fat_mount *m = fat_priv;
    struct resolve *r = it->node;
    u32 dc = ((u32)r->de.fstclushi << 16) | r->de.fstcluslo;
    u64 resume = it->cookie;

    if (resume == DIRIT_END) return 0;
    /* Only a cursor this writer could have produced may resume.  The value
     * never crosses the syscall ABI, but refusing a malformed one is
     * cheaper than inventing directory data from it. */
    if (resume != DIRIT_START &&
        ((u32)resume % DIRENT_SIZE || (u32)resume > m->cluster_size)) {
        it->cookie = DIRIT_END;
        return 0;
    }

    struct iter_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    int rc = fat_scan_dir_at(m, dc, resume, scan_iter_cb, &ctx);
    if (rc < 0) return rc; /* T-005: I/O error, not "end of directory" */
    if (!ctx.hit) {
        it->cookie = DIRIT_END; /* walked the remainder: sticky end */
        return 0;
    }
    /* The cursor opens the door *after* the delivered record.  The next
     * entry's LFN pieces (if any) start exactly there. */
    it->cookie = ((u64)ctx.dirclus << 32) | (ctx.off + DIRENT_SIZE);
    strncpy(d->name, ctx.name, sizeof(d->name) - 1);
    d->type = ctx.type;
    d->ino = ctx.ino;
    d->name[sizeof(d->name) - 1] = 0;
    return 1;
}

/* 4-arg wrapper matching fs_ops->getdent signature */
int fat32_getdent_impl(void *mnt, void *dirnode, u64 *cookie, struct dirent_out *d)
{
    (void)mnt;
    struct dir_iter it = {.node = dirnode, .cookie = *cookie};
    int r = fat_dir_iter(&it, d);
    *cookie = it.cookie;
    return r;
}
