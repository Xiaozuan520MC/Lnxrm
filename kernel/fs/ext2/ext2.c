// SPDX-License-Identifier: Apache-2.0

/* ext2 namespace and file layer: path resolution, directory-entry editing,
 * the twelve fs_ops callbacks, and the mount that validates a volume.
 *
 * Shape of the driver, and why:
 *
 *  - One global lock (ext2_fs_lock, fat32.h's fat_fs_lock in another
 *    spelling) wraps every callback.  Allocation, directory editing and the
 *    inode table all move together; without the lock two creates could pick
 *    the same free bit.
 *
 *  - A node is only "{ino, is_dir}".  Every operation re-reads the inode
 *    from disk, which is what makes two descriptors of one file agree about
 *    its length -- the coherence rule fat_size_impl() already follows.
 *
 *  - Each MUTATING callback ends in ext2_finish(): persist the free-space
 *    counters if anything was allocated.  ext2's block layer writes straight
 *    to the device, so a successful callback has no staged tail to flush;
 *    FAT32 gets the same guarantee from its journal commit, which is where
 *    this shape comes from.
 *
 * Directory entries follow Linux's ext2/dir.c exactly, because e2fsck is the
 * judge of everything here: rec_len splits when an entry is claimed and
 * merges into the previous entry when one is deleted, so the chain in a
 * block always lands exactly on the block's last byte. */
#include <ext2.h>
#include <sys/rtc.h> /* wall_clock(): the timestamps a fresh inode gets */

spinlock_t ext2_fs_lock = SPINLOCK_INIT;

/* The ext2 volume that owns "/", or NULL -- see ext2.h for why it exists.
 * vfs.c assigns it when an ext2 mount takes the root slot. */
void *ext2_priv;

/* ======================= small helpers ======================= */

/* Split "parent/name" the way every mutation needs it.  Paths arrive from
 * vfs with the mount prefix and leading slashes already removed, so "" is
 * the root (handled by the caller) and "st_d/x" splits to "" + "st_d/x". */
struct ext2_split {
    char parent[128];
    const char *name; /* points into the caller's buffer */
    u32 nlen;
};

static int ext2_split_path(const char *path, struct ext2_split *sp)
{
    while (*path == '/') path++;
    /* Trailing slashes are punctuation, not name: "st_d/" names "st_d".
     * Measured against `end` rather than a terminator, because sp->name
     * points into the caller's string, which must not be written to. */
    size_t end = strlen(path);
    while (end && path[end - 1] == '/') end--;
    if (!end) return LNXRM_EINVAL; /* the root is not a name */

    const char *slash = NULL;
    for (size_t i = 0; i < end; i++)
        if (path[i] == '/') slash = path + i;

    if (slash) {
        size_t pl = (size_t)(slash - path);
        if (pl >= sizeof(sp->parent)) return LNXRM_ENAMETOOLONG;
        memcpy(sp->parent, path, pl);
        sp->parent[pl] = 0;
        sp->name = slash + 1;
    } else {
        sp->parent[0] = 0;
        sp->name = path;
    }
    sp->nlen = (u32)(end - (size_t)(sp->name - path));
    if (!sp->nlen) return LNXRM_EINVAL;      /* "a//b" carries no last name */
    if (sp->nlen > 255) return LNXRM_ENAMETOOLONG;
    return 0;
}

/* "." and ".." are entries every directory already carries, so lookup
 * answers them straight from the dot records -- but a mutation that created,
 * removed or renamed one would rewrite those very records, and POSIX says
 * EINVAL for it.  Every mutating callback runs this after the split. */
static bool ext2_split_is_dot(const struct ext2_split *sp)
{
    if (sp->nlen == 1) return sp->name[0] == '.';
    if (sp->nlen == 2) return sp->name[0] == '.' && sp->name[1] == '.';
    return false;
}

static u32 now_secs(void)
{
    i64 sec = 0, usec = 0;
    return wall_clock(&sec, &usec) == 0 ? (u32)sec : 0;
}

/* Directory entry's file_type byte -> the DT_* number vfs hands out.  The
 * mount refuses a volume without FILETYPE, so a 0 here is a foreign write
 * and reads as a plain file rather than as "unknown". */
static u8 dt_of_ft(u8 ft)
{
    if (ft == EXT2_FT_DIR) return 4;
    if (ft == EXT2_FT_CHR) return 3;
    return 8;
}

static u8 ft_of_mode(u16 mode)
{
    if ((mode & EXT2_S_IFMT) == EXT2_S_IFDIR) return EXT2_FT_DIR;
    if ((mode & EXT2_S_IFMT) == EXT2_S_IFCHR) return EXT2_FT_CHR;
    return EXT2_FT_REG;
}

static bool name_is(const u8 *p, u32 nlen, const char *s, u32 slen)
{
    return nlen == slen && !memcmp(p, s, slen);
}

static bool is_dot(const u8 *p, u32 nlen) { return name_is(p, nlen, ".", 1); }
static bool is_dotdot(const u8 *p, u32 nlen) { return name_is(p, nlen, "..", 2); }

/* Persist whatever is pending.  ext2 writes straight to the device (the
 * write-back cache belongs to FAT32 only), so a failing transfer has already
 * been reported by ext2_write_data_block(): the callback's own answer stands. */
static int ext2_finish(struct ext2_mount *fs, int rc)
{
    if (fs->space_dirty) ext2_sync_space(fs);
    return rc;
}

/* Give an inode back with no blocks left behind it.  ext2_inode_free()
 * clears the bitmap but does not know about the file's data, so anything
 * that unwinds a half-built object calls this first.  `in` is the caller's
 * latest copy of the record, which may be AHEAD of what is on disk (a mkdir
 * that failed before its second write) -- those are exactly the blocks whose
 * ownership would be lost if the disk's own copy were the only one read. */
static void ext2_inode_discard(struct ext2_mount *fs, u32 ino,
                               const struct ext2_inode *in)
{
    struct ext2_inode scratch;
    if (!in) {
        if (ext2_read_inode(fs, ino, &scratch)) {
            ext2_inode_free(fs, ino); /* the bitmap half still has to land */
            return;
        }
        in = &scratch;
    }
    if (in->i_blocks) {
        struct ext2_inode work = *in;
        if (ext2_truncate_blocks(fs, &work, 0) >= 0)
            ext2_write_inode(fs, ino, &work);
    }
    ext2_inode_free(fs, ino);
}

/* Step a directory's i_links_count by `delta`.  Every subdirectory is one
 * link on its parent -- the child's ".." -- and e2fsck derives a directory's
 * expected count from the tree, so an mkdir/rmdir/rename that did not move
 * it would leave the volume disagreeing with itself. */
static int ext2_dir_link(struct ext2_mount *fs, u32 dir_ino, int delta)
{
    struct ext2_inode in;
    int rc = ext2_read_inode(fs, dir_ino, &in);
    if (rc) return rc;
    if (delta > 0) {
        in.i_links_count = (u16)(in.i_links_count + delta);
    } else if (in.i_links_count) {
        in.i_links_count = (u16)(in.i_links_count + delta);
    } else {
        return 0; /* already at 0: not wrapping to 65535 */
    }
    in.i_mtime = now_secs();
    return ext2_write_inode(fs, dir_ino, &in);
}

/* Point a directory's ".." record at `parent` -- the one field that has to
 * follow a directory when rename() carries it to another parent. */
static int ext2_dir_set_dotdot(struct ext2_mount *fs, u32 dir_ino, u32 parent)
{
    struct ext2_inode in;
    int rc = ext2_read_inode(fs, dir_ino, &in);
    if (rc) return rc;
    if ((in.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return LNXRM_ENOTDIR;
    if (!in.i_size) return LNXRM_EIO; /* a directory with no ".." to move */

    u32 bs = fs->block_size;
    u32 pblk = 0;
    rc = ext2_bmap(fs, &in, 0, &pblk);
    if (rc) return rc;
    if (!pblk) return LNXRM_EIO;

    u8 *buf = (u8 *)kmalloc(bs);
    if (!buf) return LNXRM_ENOMEM;
    rc = ext2_read_data_block(fs, pblk, buf);
    if (rc) { kfree(buf); return rc; }

    int answer = LNXRM_EIO;
    u32 off = 0;
    while (off + 8 <= bs) {
        u32 dino;
        u16 rec;
        u32 nl = buf[off + 6];
        memcpy(&dino, buf + off, 4);
        memcpy(&rec, buf + off + 4, 2);
        if (rec < 8 || (rec & 3) || (u32)off + rec > bs ||
            EXT2_DIR_REC_LEN(nl) > rec) {
            kprintf("[ext2] directory inode %u: rec_len %u invalid at offset %u, \"..\" not moved\n",
                    dir_ino, rec, off);
            break;
        }
        if (dino && name_is(buf + off + 8, nl, "..", 2)) {
            memcpy(buf + off, &parent, 4);
            answer = ext2_write_data_block(fs, pblk, buf);
            break;
        }
        off += rec;
    }
    kfree(buf);
    return answer;
}

/* ======================= reading a directory ======================= */

/* Find `name` inside directory `dir_ino`.
 * 0 = found (in *out_ino / *out_ft / *out_off), LNXRM_ENOENT = not there,
 * negative = the directory itself would not answer.  `out_*` may be NULL. */
static int ext2_dir_find(struct ext2_mount *fs, u32 dir_ino, const char *name,
                         u32 nlen, u32 *out_ino, u8 *out_ft, u64 *out_off)
{
    struct ext2_inode in;
    int rc = ext2_read_inode(fs, dir_ino, &in);
    if (rc) return rc;
    if ((in.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return LNXRM_ENOTDIR;

    u32 bs = fs->block_size;
    u8 *buf = (u8 *)kmalloc(bs);
    if (!buf) return LNXRM_ENOMEM;

    int answer = LNXRM_ENOENT;
    u32 nblk = in.i_size / bs;
    for (u32 lblk = 0; lblk < nblk; lblk++) {
        u32 pblk = 0;
        rc = ext2_bmap(fs, &in, lblk, &pblk);
        if (rc) { answer = rc; break; }
        if (!pblk) continue; /* hole: no entries to be found there */
        rc = ext2_read_data_block(fs, pblk, buf);
        if (rc) { answer = rc; break; }

        u32 off = 0;
        while (off + 8 <= bs) {
            u32 dino;
            u16 rec;
            u32 nlen_e = buf[off + 6];
            memcpy(&dino, buf + off, 4);
            memcpy(&rec, buf + off + 4, 2);
            /* The record has to be big enough for the name it claims, or the
             * comparison below would read past the record -- and, near the
             * end of the block, past the buffer. */
            if (rec < 8 || (rec & 3) || (u32)off + rec > bs ||
                EXT2_DIR_REC_LEN(nlen_e) > rec) {
                kprintf("[ext2] directory inode %u: rec_len %u invalid at offset %u, lookup stopped\n",
                        dir_ino, rec, off);
                answer = LNXRM_EIO;
                goto done;
            }
            if (dino && nlen_e == nlen && !memcmp(buf + off + 8, name, nlen)) {
                if (out_ino) *out_ino = dino;
                if (out_ft) *out_ft = buf[off + 7];
                if (out_off) *out_off = (u64)lblk * bs + off;
                answer = 0;
                goto done;
            }
            off += rec;
        }
    }
done:
    kfree(buf);
    return answer;
}

/* One readdir step.  `*cookie` is a byte offset into the directory, so a
 * later call resumes exactly where this one stopped; "." and ".." are not
 * reported (fat32_dir.c skips them too, and ls has no use for them).
 * Returns 1 = filled, 0 = the directory ended, negative = it died. */
static int ext2_getdent_impl(void *mnt, void *node, u64 *cookie,
                             struct dirent_out *d)
{
    struct ext2_mount *fs = mnt;
    struct ext2_node *n = node;
    if (!n || !n->is_dir) return LNXRM_EINVAL;

    struct ext2_inode in;
    int rc = ext2_read_inode(fs, n->ino, &in);
    if (rc) return rc;

    u32 bs = fs->block_size;
    u64 size = in.i_size;
    u64 off = *cookie;
    u32 cur = (u32)-1;
    u8 *buf = (u8 *)kmalloc(bs);
    if (!buf) return LNXRM_ENOMEM;

    while (off < size) {
        u32 lblk = (u32)(off / bs);
        u32 boff = (u32)(off % bs);

        if (lblk != cur) {
            u32 pblk = 0;
            rc = ext2_bmap(fs, &in, lblk, &pblk);
            if (rc) goto out;
            if (!pblk) {           /* hole: an empty stretch of directory */
                off = (u64)(lblk + 1) * bs;
                continue;
            }
            rc = ext2_read_data_block(fs, pblk, buf);
            if (rc) goto out;
            cur = lblk;
        }
        if (boff + 8 > bs) {
            off = (u64)(lblk + 1) * bs;
            continue;
        }

        u32 dino;
        u16 rec;
        u8 nlen_e = buf[boff + 6];
        memcpy(&dino, buf + boff, 4);
        memcpy(&rec, buf + boff + 4, 2);

        /* A record that cannot describe itself stops the scan.  Saying so
         * and returning "end of directory" is deliberate: advancing by a
         * rec_len of 0 would spin forever, and an error would make readdir
         * fail after already handing out every good entry above. */
        if (rec < 8 || (rec & 3) || (u32)boff + rec > bs ||
            rec < EXT2_DIR_REC_LEN(nlen_e)) {
            kprintf("[ext2] directory inode %u: rec_len %u invalid at offset %llu, scan stopped\n",
                    n->ino, rec, (unsigned long long)off);
            rc = 0;
            goto out;
        }

        u64 next = off + rec;
        if (dino && nlen_e && !is_dot(buf + boff + 8, nlen_e) &&
            !is_dotdot(buf + boff + 8, nlen_e)) {
            memset(d, 0, sizeof(*d));
            u32 cp = (u32)nlen_e < (u32)sizeof(d->name) - 1 ? (u32)nlen_e
                                                            : (u32)sizeof(d->name) - 1;
            memcpy(d->name, buf + boff + 8, cp);
            d->name[cp] = 0;
            d->type = dt_of_ft(buf[boff + 7]);
            d->ino = dino;
            *cookie = next;
            kfree(buf);
            return 1;
        }
        off = next;
    }
    rc = 0;
out:
    *cookie = off;
    kfree(buf);
    return rc;
}

/* ======================= editing a directory ======================= */

/* Claim room for `name` in directory `dir_ino`, creating a new directory
 * block when the ones already there are full.  Mirrors ext2_add_link():
 * a free slot is taken whole, an occupied one is split when its slack
 * covers the new record, and a fresh block starts with one record whose
 * rec_len is the whole block. */
static int ext2_dir_add(struct ext2_mount *fs, u32 dir_ino, const char *name,
                        u32 nlen, u32 child_ino, u8 ft)
{
    if (!nlen || nlen > 255) return LNXRM_ENAMETOOLONG;
    u32 reclen = EXT2_DIR_REC_LEN(nlen);

    struct ext2_inode in;
    int rc = ext2_read_inode(fs, dir_ino, &in);
    if (rc) return rc;
    if ((in.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return LNXRM_ENOTDIR;

    u32 bs = fs->block_size;
    u8 *buf = (u8 *)kmalloc(bs);
    if (!buf) return LNXRM_ENOMEM;

    u32 nblk = in.i_size / bs;
    u32 blocks0 = in.i_blocks; /* what the record already claimed, for below */
    int answer = LNXRM_ENOSPC; /* cleared only by a completed claim */
    u32 lblk = 0;

    /* Bounded by nblk: a block at or past i_size is always fresh and always
     * has room, so the loop can never need a second one of those. */
    for (; lblk <= nblk; lblk++) {
        u32 pblk = 0;
        bool grown = (lblk >= nblk);
        bool fresh = false; /* the block was just taken: no entries yet */

        if (grown) {
            /* Past the end of the directory: take a block and start it with
             * one free record spanning it, which is what ext2_add_link()
             * does when it walks off i_size. */
            rc = ext2_bmap_alloc(fs, &in, lblk, &pblk);
            if (rc) { answer = rc; goto done; }
            fresh = true;
        } else {
            rc = ext2_bmap(fs, &in, lblk, &pblk);
            if (rc) { answer = rc; goto done; }
            if (!pblk) {
                /* A hole the directory's i_size already spans.  The block is
                 * not optional: pblk would otherwise stay 0, and block 0 is
                 * where the superblock lives. */
                rc = ext2_bmap_alloc(fs, &in, lblk, &pblk);
                if (rc) { answer = rc; goto done; }
                fresh = true;
            } else {
                rc = ext2_read_data_block(fs, pblk, buf);
                if (rc) { answer = rc; goto done; }
            }
        }
        if (fresh) {
            /* A brand-new block starts as one free record spanning all of it:
             * rec_len at offset 4, inode left 0. */
            memset(buf, 0, bs);
            u16 whole = (u16)bs;
            memcpy(buf + 4, &whole, 2);
        }

        u32 off = 0;
        while (off + 8 <= bs) {
            u32 dino;
            u16 rec;
            u32 old_nl = buf[off + 6];
            memcpy(&dino, buf + off, 4);
            memcpy(&rec, buf + off + 4, 2);
            /* Enough room for the header, 4-byte aligned, inside the block,
             * and at least as long as the name already in it claims: the last
             * of those is what keeps the comparison below inside the record. */
            if (rec < 8 || (rec & 3) || (u32)off + rec > bs ||
                EXT2_DIR_REC_LEN(old_nl) > rec) {
                kprintf("[ext2] directory inode %u: rec_len %u invalid at offset %u, add refused\n",
                        dir_ino, rec, off);
                answer = LNXRM_EIO;
                goto done;
            }
            if (dino && old_nl == nlen && !memcmp(buf + off + 8, name, nlen)) {
                answer = LNXRM_EEXIST;
                goto done;
            }

            u32 need = EXT2_DIR_REC_LEN(old_nl);
            if ((!dino && rec >= reclen) || (u32)rec >= need + reclen) {
                u32 pos = off;
                u16 newrec = rec;
                if (dino) {
                    /* Split: the old entry keeps exactly what it needs and
                     * the newcomer takes the rest of the record. */
                    u16 keep = (u16)need;
                    memcpy(buf + off + 4, &keep, 2);
                    pos = off + need;
                    newrec = (u16)(rec - need);
                }
                u16 rl = newrec;
                memcpy(buf + pos, &child_ino, 4);
                memcpy(buf + pos + 4, &rl, 2);
                buf[pos + 6] = (u8)nlen;
                buf[pos + 7] = ft;
                memcpy(buf + pos + 8, name, nlen);

                rc = ext2_write_data_block(fs, pblk, buf);
                if (rc) { answer = rc; goto done; }
                if (grown) {
                    /* The directory just grew by one block; i_size is a
                     * whole number of blocks and e2fsck checks it. */
                    in.i_size = (u64)(lblk + 1) * bs;
                    rc = ext2_write_inode(fs, dir_ino, &in);
                    if (rc) { answer = rc; goto done; }
                }
                answer = 0;
                goto done;
            }
            off += rec;
        }
        /* A block with no room moves on to the next one; a fresh one never
         * gets here, its first record spans the whole block and claims. */
    }

done:
    /* Whatever the loop claimed has to be named by the record before this
     * returns, even on the way out with an error: a block the bitmap owns
     * that no inode points at is exactly what e2fsck reports.  Growing i_size
     * is part of that -- a claimed block past the end of the directory is
     * still a block the directory now owns. */
    if (answer < 0 && in.i_blocks != blocks0) {
        if (in.i_size < (u64)(lblk + 1) * bs) in.i_size = (u64)(lblk + 1) * bs;
        ext2_write_inode(fs, dir_ino, &in);
    }
    kfree(buf);
    return answer;
}

/* Unlink the record at byte offset `off`: its rec_len is folded into the
 * previous record of the same block (ext2_delete_entry()'s merge), which is
 * what keeps the chain landing exactly on the block's last byte. */
static int ext2_dir_del_at(struct ext2_mount *fs, u32 dir_ino, u64 off)
{
    u32 bs = fs->block_size;
    u32 lblk = (u32)(off / bs);
    u32 boff = (u32)(off % bs);

    struct ext2_inode in;
    int rc = ext2_read_inode(fs, dir_ino, &in);
    if (rc) return rc;
    u32 pblk = 0;
    rc = ext2_bmap(fs, &in, lblk, &pblk);
    if (rc) return rc;
    if (!pblk) return LNXRM_EIO;

    u8 *buf = (u8 *)kmalloc(bs);
    if (!buf) return LNXRM_ENOMEM;
    rc = ext2_read_data_block(fs, pblk, buf);
    if (rc) { kfree(buf); return rc; }

    u16 rec;
    /* Bounds first: the header bytes are only read once boff+8 is known to
     * be inside the block. */
    if (boff + 8 > bs) {
        kprintf("[ext2] directory inode %u: offset %u past the block, delete refused\n",
                dir_ino, boff);
        kfree(buf);
        return LNXRM_EIO;
    }
    memcpy(&rec, buf + boff + 4, 2);
    if (rec < 8 || (rec & 3) || (u32)boff + rec > bs ||
        EXT2_DIR_REC_LEN(buf[boff + 6]) > rec) {
        kprintf("[ext2] directory inode %u: rec_len %u invalid at offset %u, delete refused\n",
                dir_ino, rec, boff);
        kfree(buf);
        return LNXRM_EIO;
    }

    u32 prev = 0;
    bool has_prev = false;
    for (u32 o = 0; o < boff;) {
        u16 r;
        memcpy(&r, buf + o + 4, 2);
        if (r < 8 || (u32)o + r > bs) {
            kfree(buf);
            return LNXRM_EIO;
        }
        prev = o;
        has_prev = true;
        o += r;
    }
    if (has_prev) {
        u16 merged = (u16)(boff + rec - prev);
        memcpy(buf + prev + 4, &merged, 2);
    }
    u32 zero = 0;
    memcpy(buf + boff, &zero, 4); /* inode 0 = the slot is free */

    rc = ext2_write_data_block(fs, pblk, buf);
    kfree(buf);
    return rc;
}

/* Point an EXISTING record at another inode (rename's replace half). */
static int ext2_dir_set_at(struct ext2_mount *fs, u32 dir_ino, u64 off,
                           u32 child_ino, u8 ft)
{
    u32 bs = fs->block_size;
    u32 lblk = (u32)(off / bs);
    u32 boff = (u32)(off % bs);

    struct ext2_inode in;
    int rc = ext2_read_inode(fs, dir_ino, &in);
    if (rc) return rc;
    u32 pblk = 0;
    rc = ext2_bmap(fs, &in, lblk, &pblk);
    if (rc) return rc;
    if (!pblk) return LNXRM_EIO;

    u8 *buf = (u8 *)kmalloc(bs);
    if (!buf) return LNXRM_ENOMEM;
    rc = ext2_read_data_block(fs, pblk, buf);
    if (!rc) {
        /* Bounds first: the record header is read only once it is known to
         * be inside the block, the same order the other walkers use. */
        if (boff + 8 > bs) {
            rc = LNXRM_EIO;
        } else {
            u16 rec;
            memcpy(&rec, buf + boff + 4, 2);
            u32 nl = buf[boff + 6];
            if (rec < 8 || (rec & 3) || (u32)boff + rec > bs ||
                EXT2_DIR_REC_LEN(nl) > rec || !buf[boff]) {
                kprintf("[ext2] directory inode %u: rec_len %u invalid at offset %u, set refused\n",
                        dir_ino, rec, boff);
                rc = LNXRM_EIO;
            } else {
                memcpy(buf + boff, &child_ino, 4);
                buf[boff + 7] = ft;
                rc = ext2_write_data_block(fs, pblk, buf);
            }
        }
    }
    kfree(buf);
    return rc;
}

/* rmdir's emptiness test: only "." and ".." may remain.  A directory that
 * will not scan counts as not empty, because freeing it would be the worse
 * mistake (ext2_empty_dir() answers the same way on a read error). */
static int ext2_dir_is_empty(struct ext2_mount *fs, u32 dir_ino)
{
    struct ext2_inode in;
    int rc = ext2_read_inode(fs, dir_ino, &in);
    if (rc) return rc;

    u32 bs = fs->block_size;
    u8 *buf = (u8 *)kmalloc(bs);
    if (!buf) return LNXRM_ENOMEM;

    int answer = 1;
    u32 nblk = in.i_size / bs;
    for (u32 lblk = 0; lblk < nblk && answer == 1; lblk++) {
        u32 pblk = 0;
        rc = ext2_bmap(fs, &in, lblk, &pblk);
        if (rc) { answer = rc; break; }
        if (!pblk) continue;
        rc = ext2_read_data_block(fs, pblk, buf);
        if (rc) { answer = rc; break; }

        u32 off = 0;
        while (off + 8 <= bs) {
            u32 dino;
            u16 rec;
            u32 nlen_e = buf[off + 6];
            memcpy(&dino, buf + off, 4);
            memcpy(&rec, buf + off + 4, 2);
            if (rec < 8 || (rec & 3) || (u32)off + rec > bs ||
                EXT2_DIR_REC_LEN(nlen_e) > rec) {
                answer = LNXRM_EIO;
                break;
            }
            if (dino && !is_dot(buf + off + 8, nlen_e) &&
                !is_dotdot(buf + off + 8, nlen_e)) {
                answer = 0;
                break;
            }
            off += rec;
        }
    }
    kfree(buf);
    return answer;
}

/* Lay out a new directory's first block: "." then "..", exactly as
 * ext2_make_empty() does -- 12 bytes for the first record, the rest of the
 * block for the second.  `in` is the not-yet-written inode; bmap_alloc()
 * charges the block into its i_blocks, and i_size becomes one block. */
static int ext2_dir_make_empty(struct ext2_mount *fs, u32 dir_ino,
                               u32 parent_ino, struct ext2_inode *in)
{
    u32 bs = fs->block_size;
    /* The buffer is taken first: once the block below is claimed, every
     * remaining step has to be able to run, or the bitmap owns a block no
     * record will ever mention. */
    u8 *buf = (u8 *)kmalloc(bs);
    if (!buf) return LNXRM_ENOMEM;

    u32 pblk = 0;
    int rc = ext2_bmap_alloc(fs, in, 0, &pblk);
    if (rc) { kfree(buf); return rc; }
    memset(buf, 0, bs);

    u16 rec0 = (u16)EXT2_DIR_REC_LEN(1);
    u16 rec1 = (u16)(bs - EXT2_DIR_REC_LEN(1));
    memcpy(buf, &dir_ino, 4);
    memcpy(buf + 4, &rec0, 2);
    buf[6] = 1;
    buf[7] = EXT2_FT_DIR;
    buf[8] = '.';

    memcpy(buf + 12, &parent_ino, 4);
    memcpy(buf + 16, &rec1, 2);
    buf[18] = 2;
    buf[19] = EXT2_FT_DIR;
    buf[20] = '.';
    buf[21] = '.';

    rc = ext2_write_data_block(fs, pblk, buf);
    kfree(buf);
    if (rc) {
        /* Undo the claim: this block is in no record yet, so nothing else
         * is ever going to give it back. */
        ext2_block_free(fs, pblk);
        in->i_block[0] = 0;
        in->i_blocks -= fs->sectors_per_block;
        return rc;
    }
    in->i_size = bs;
    return 0;
}

/* ======================= vnode plumbing ======================= */

static int ext2_fill_vnode(struct ext2_mount *fs, u32 ino, struct vnode *out)
{
    struct ext2_inode in;
    int rc = ext2_read_inode(fs, ino, &in);
    if (rc) return rc;

    struct ext2_node *n = (struct ext2_node *)kmalloc(sizeof(*n));
    if (!n) return LNXRM_ENOMEM;
    n->ino = ino;
    n->is_dir = (in.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR;

    out->type = n->is_dir ? V_DIR : V_REG;
    out->size = n->is_dir ? 0 : in.i_size;
    out->ino = ino;
    /* st_mode's permission bits come from the file's own mode: a 0444 file
     * on an ext2 volume really is read-only, which FAT32's ATTR_READONLY
     * used to be the only way to express. */
    out->ro = (in.i_mode & 0222) == 0;
    out->ops = &ext2_ops;
    out->mnt_data = fs;
    out->fs_data = n;
    return 0;
}

/* Walk `path` as a chain of directories, returning its inode.  "" is the
 * mount root; a component that is not a directory is ENOTDIR. */
static int ext2_resolve_dir(struct ext2_mount *fs, const char *path, u32 *ino)
{
    while (*path == '/') path++;
    u32 cur = EXT2_ROOT_INO;

    const char *p = path;
    while (*p) {
        const char *e = p;
        while (*e && *e != '/') e++;
        u32 nlen = (u32)(e - p);
        if (nlen) {
            if (nlen > 255) return LNXRM_ENAMETOOLONG;
            u32 next;
            u8 ft;
            int rc = ext2_dir_find(fs, cur, p, nlen, &next, &ft, NULL);
            if (rc) return rc;
            if (ft == EXT2_FT_UNKNOWN) {
                /* A tool that left the type byte empty must not make a real
                 * directory unopenable: the inode's own mode is the answer. */
                struct ext2_inode child;
                rc = ext2_read_inode(fs, next, &child);
                if (rc) return rc;
                if ((child.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR)
                    return LNXRM_ENOTDIR;
            } else if (ft != EXT2_FT_DIR) {
                return LNXRM_ENOTDIR;
            }
            cur = next;
        }
        p = e;
        while (*p == '/') p++;
    }
    *ino = cur;
    return 0;
}

/* ======================= fs_ops callbacks ======================= */

static int ext2_lookup(void *mnt, const char *path, struct vnode *out)
{
    EXT2_ENTER();
    struct ext2_mount *fs = mnt;
    int rc;

    while (*path == '/') path++;
    if (!path[0]) {
        rc = ext2_fill_vnode(fs, EXT2_ROOT_INO, out);
        EXT2_LEAVE();
        return rc;
    }

    struct ext2_split sp;
    rc = ext2_split_path(path, &sp);
    if (rc) { EXT2_LEAVE(); return rc; }

    u32 dir_ino;
    rc = ext2_resolve_dir(fs, sp.parent, &dir_ino);
    if (rc) { EXT2_LEAVE(); return rc; }

    u32 ino;
    rc = ext2_dir_find(fs, dir_ino, sp.name, sp.nlen, &ino, NULL, NULL);
    if (rc) { EXT2_LEAVE(); return rc; }

    rc = ext2_fill_vnode(fs, ino, out);
    EXT2_LEAVE();
    return rc;
}

static int ext2_getdent(void *mnt, void *dirnode, u64 *cookie, struct dirent_out *d)
{
    EXT2_ENTER();
    int r = ext2_getdent_impl(mnt, dirnode, cookie, d);
    EXT2_LEAVE();
    return r;
}

/* Read `n` bytes at `off`.  Short at the end of the file, 0 past it, and a
 * hole reads as zeroes -- a sparse file has no bytes on disk to return, and
 * the alternative (stale block contents) would be a disclosure. */
static int ext2_read(void *mnt, void *node, u64 off, void *ubuf, size_t n)
{
    EXT2_ENTER();
    struct ext2_mount *fs = mnt;
    struct ext2_node *nd = node;
    int rc;

    if (!nd || nd->is_dir) { rc = LNXRM_EFAIL; goto out; }
    if (!ubuf && n) { rc = LNXRM_EFAIL; goto out; }
    if (!n) { rc = 0; goto out; } /* a zero-length read never reaches the disk */

    struct ext2_inode in;
    rc = ext2_read_inode(fs, nd->ino, &in);
    if (rc) goto out;

    u64 size = in.i_size;
    if (off >= size) { rc = 0; goto out; }
    if (off + n > size) n = (size_t)(size - off);
    if (n > 1024 * 1024) n = 1024 * 1024; /* one call never carries more */

    u32 bs = fs->block_size;
    u8 *tmp = (u8 *)kmalloc(bs);
    if (!tmp) { rc = LNXRM_ENOMEM; goto out; }

    size_t done = 0;
    int err = 0;
    while (done < n) {
        u64 pos = off + done;
        u32 lblk = (u32)(pos / bs);
        u32 boff = (u32)(pos % bs);
        size_t chunk = bs - boff;
        if (chunk > n - done) chunk = n - done;

        u32 pblk = 0;
        rc = ext2_bmap(fs, &in, lblk, &pblk);
        if (rc) { err = rc; break; }
        if (!pblk) {
            memset((u8 *)ubuf + done, 0, chunk);
        } else {
            rc = ext2_read_data_block(fs, pblk, tmp);
            if (rc) { err = rc; break; }
            memcpy((u8 *)ubuf + done, tmp + boff, chunk);
        }
        done += chunk;
    }
    kfree(tmp);
    /* T-005: a read that died is not EOF.  `done` bytes already handed over
     * stay handed over; with nothing delivered the error is the answer. */
    rc = err ? (done ? (int)done : err) : (int)done;
out:
    EXT2_LEAVE();
    return rc;
}

/* Write `n` bytes at `off`, growing the file (and allocating the blocks on
 * the way) as needed.  The two special shapes vfs depends on:
 *   buf == NULL && n == 0  -> O_TRUNC: empty the file (vfs_lookup's path)
 *   ENOSPC                  -> -28, never a short count the caller would
 *                               read as "everything was written" */
static int ext2_write(void *mnt, void *node, u64 off, const void *ubuf, size_t n)
{
    EXT2_ENTER();
    struct ext2_mount *fs = mnt;
    struct ext2_node *nd = node;
    int rc;

    if (!nd || nd->is_dir) { rc = LNXRM_EFAIL; goto out; }
    if (!ubuf && n) { rc = LNXRM_EFAIL; goto out; }
    if (!ubuf) { /* n == 0: the O_TRUNC contract */
        struct ext2_inode in;
        rc = ext2_read_inode(fs, nd->ino, &in);
        if (rc) goto out;
        int r = ext2_truncate_blocks(fs, &in, 0);
        if (r < 0) { rc = r; goto out; }
        in.i_size = 0;
        rc = ext2_write_inode(fs, nd->ino, &in);
        /* truncate_blocks() moved the free counters and set space_dirty --
         * the same door every other mutating callback walks through, so the
         * reclaimed blocks are counted on disk before this returns.  Going
         * straight to `out` here left the volume reporting a full disk to the
         * next writer, which is how test_enospc's O_TRUNC refiller starved
         * the mmap test that runs after it. */
        goto out;
    }
    if (off + n > 0xFFFFFFFFull) { rc = LNXRM_EINVAL; goto out; }

    struct ext2_inode in;
    rc = ext2_read_inode(fs, nd->ino, &in);
    if (rc) goto out;
    u64 old_size = in.i_size;

    u32 bs = fs->block_size;
    u8 *tmp = (u8 *)kmalloc(bs);
    if (!tmp) { rc = LNXRM_ENOMEM; goto out; }

    size_t done = 0;
    int err = 0;
    while (done < n) {
        u64 pos = off + done;
        u32 lblk = (u32)(pos / bs);
        u32 boff = (u32)(pos % bs);
        size_t chunk = bs - boff;
        if (chunk > n - done) chunk = n - done;

        u32 pblk = 0;
        rc = ext2_bmap_alloc(fs, &in, lblk, &pblk);
        if (rc) { err = rc; break; } /* ENOSPC arrives here */

        if (chunk == bs) {
            rc = ext2_write_data_block(fs, pblk, (const u8 *)ubuf + done);
            if (rc) { err = rc; break; }
        } else {
            rc = ext2_read_data_block(fs, pblk, tmp);
            if (!rc) {
                memcpy(tmp + boff, (const u8 *)ubuf + done, chunk);
                rc = ext2_write_data_block(fs, pblk, tmp);
            }
            if (rc) { err = rc; break; }
        }
        done += chunk;
    }
    kfree(tmp);

    /* The inode carries i_blocks for every block the loop just took, so it
     * must be written on the failure path too: an allocated block nobody
     * accounts for is the leak e2fsck calls "block bitmap differs".  A write
     * that delivered nothing (n == 0, or ENOSPC on the first block) leaves
     * the length alone -- only real bytes may push EOF forward. */
    u64 final = off + done;
    if (done && final > old_size) in.i_size = (u32)final;
    int wrc = ext2_write_inode(fs, nd->ino, &in);
    if (err) { rc = err; goto out; }
    if (wrc) { rc = wrc; goto out; }
    rc = (int)done;
out:
    rc = ext2_finish(fs, rc);
    EXT2_LEAVE();
    return rc;
}

static u64 ext2_size(void *mnt, void *node)
{
    EXT2_ENTER();
    struct ext2_mount *fs = mnt;
    struct ext2_node *nd = node;
    u64 r = 0;
    if (nd && !nd->is_dir) {
        struct ext2_inode in;
        if (ext2_read_inode(fs, nd->ino, &in) == 0) r = in.i_size;
    }
    EXT2_LEAVE();
    return r;
}

static void ext2_close_node(void *mnt, void *node)
{
    (void)mnt;
    kfree(node); /* a node is {ino, is_dir}: nothing on disk to release */
}

static u64 ext2_freespace(void *mnt)
{
    EXT2_ENTER();
    struct ext2_mount *fs = mnt;
    u64 r = (u64)fs->free_blocks * fs->block_size;
    EXT2_LEAVE();
    return r;
}

/* ======================= mutations ======================= */

static int ext2_create(void *mnt, const char *path)
{
    EXT2_ENTER();
    struct ext2_mount *fs = mnt;
    struct ext2_split sp;
    int rc = ext2_split_path(path, &sp);
    if (rc) goto out;
    if (ext2_split_is_dot(&sp)) { rc = LNXRM_EINVAL; goto out; }

    u32 dir_ino;
    rc = ext2_resolve_dir(fs, sp.parent, &dir_ino);
    if (rc) goto out;

    u32 exist;
    rc = ext2_dir_find(fs, dir_ino, sp.name, sp.nlen, &exist, NULL, NULL);
    if (rc == 0) { rc = LNXRM_EEXIST; goto out; }
    if (rc != LNXRM_ENOENT) goto out;

    u32 ino;
    rc = ext2_inode_alloc(fs, &ino);
    if (rc) goto out;

    struct ext2_inode in;
    memset(&in, 0, sizeof(in));
    in.i_mode = EXT2_S_IFREG | 0664; /* debugfs' own mode for a written file */
    in.i_links_count = 1;
    in.i_atime = in.i_ctime = in.i_mtime = now_secs();
    rc = ext2_write_inode(fs, ino, &in);
    if (rc) { ext2_inode_free(fs, ino); goto out; }

    rc = ext2_dir_add(fs, dir_ino, sp.name, sp.nlen, ino, ft_of_mode(in.i_mode));
    if (rc) { ext2_inode_free(fs, ino); goto out; }
    rc = 0;
out:
    rc = ext2_finish(fs, rc);
    EXT2_LEAVE();
    return rc;
}

static int ext2_mkdir(void *mnt, const char *path)
{
    EXT2_ENTER();
    struct ext2_mount *fs = mnt;
    struct ext2_split sp;
    int rc = ext2_split_path(path, &sp);
    if (rc) goto out;
    if (ext2_split_is_dot(&sp)) { rc = LNXRM_EINVAL; goto out; }

    u32 dir_ino;
    rc = ext2_resolve_dir(fs, sp.parent, &dir_ino);
    if (rc) goto out;

    u32 exist;
    rc = ext2_dir_find(fs, dir_ino, sp.name, sp.nlen, &exist, NULL, NULL);
    if (rc == 0) { rc = LNXRM_EEXIST; goto out; }
    if (rc != LNXRM_ENOENT) goto out;

    /* The parent gains a subdirectory link before the child exists, the way
     * ext2_mkdir() does it -- a later failure walks it back. */
    struct ext2_inode pin;
    rc = ext2_read_inode(fs, dir_ino, &pin);
    if (rc) goto out;
    pin.i_links_count++;
    pin.i_mtime = now_secs();
    rc = ext2_write_inode(fs, dir_ino, &pin);
    if (rc) goto out;

    u32 ino;
    rc = ext2_inode_alloc(fs, &ino);
    if (rc) { pin.i_links_count--; ext2_write_inode(fs, dir_ino, &pin); goto out; }

    struct ext2_inode in;
    memset(&in, 0, sizeof(in));
    in.i_mode = EXT2_S_IFDIR | 0755;
    in.i_links_count = 2; /* "." and the parent's entry */
    in.i_atime = in.i_ctime = in.i_mtime = now_secs();
    rc = ext2_write_inode(fs, ino, &in);
    if (rc) { ext2_inode_free(fs, ino); goto undo_parent; }

    /* The record is a directory now, so its group counts it as one.  This
     * runs before any failure path that could free the inode again, because
     * ext2_inode_free() steps the same counter down when the record it frees
     * says "directory" -- an increment after those paths would have nothing
     * to undo and the count would drift. */
    fs->gd[(ino - 1) / fs->inodes_per_group].bg_used_dirs_count++;
    fs->space_dirty = true;

    rc = ext2_dir_make_empty(fs, ino, dir_ino, &in);
    if (rc) { ext2_inode_discard(fs, ino, &in); goto undo_parent; }
    rc = ext2_write_inode(fs, ino, &in);
    if (rc) { ext2_inode_discard(fs, ino, &in); goto undo_parent; }

    rc = ext2_dir_add(fs, dir_ino, sp.name, sp.nlen, ino, ft_of_mode(in.i_mode));
    if (rc) { ext2_inode_discard(fs, ino, &in); goto undo_parent; }
    rc = 0;
    goto out;

undo_parent:
    if (pin.i_links_count) pin.i_links_count--;
    pin.i_mtime = now_secs();
    ext2_write_inode(fs, dir_ino, &pin);
out:
    rc = ext2_finish(fs, rc);
    EXT2_LEAVE();
    return rc;
}

static int ext2_unlink(void *mnt, const char *path)
{
    EXT2_ENTER();
    struct ext2_mount *fs = mnt;
    struct ext2_split sp;
    int rc = ext2_split_path(path, &sp);
    if (rc) goto out;
    if (ext2_split_is_dot(&sp)) { rc = LNXRM_EINVAL; goto out; }

    u32 dir_ino;
    rc = ext2_resolve_dir(fs, sp.parent, &dir_ino);
    if (rc) goto out;

    u32 ino;
    u64 off;
    rc = ext2_dir_find(fs, dir_ino, sp.name, sp.nlen, &ino, NULL, &off);
    if (rc) goto out;

    struct ext2_inode in;
    rc = ext2_read_inode(fs, ino, &in);
    if (rc) goto out;
    if ((in.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) {
        rc = LNXRM_EFAIL; /* a directory goes through rmdir, not unlink */
        goto out;
    }

    rc = ext2_dir_del_at(fs, dir_ino, off);
    if (rc) goto out;

    if (in.i_links_count) in.i_links_count--;
    if (in.i_links_count) {
        rc = ext2_write_inode(fs, ino, &in);
    } else {
        /* Last reference gone: its blocks go with it, or the bitmap would
         * keep bytes nobody can reach any more. */
        int r = ext2_truncate_blocks(fs, &in, 0);
        if (r < 0) { rc = r; goto out; }
        in.i_size = 0;
        ext2_inode_free(fs, ino); /* wipes the record, links_count and all */
        rc = 0;
    }
out:
    rc = ext2_finish(fs, rc);
    EXT2_LEAVE();
    return rc;
}

static int ext2_rmdir(void *mnt, const char *path)
{
    EXT2_ENTER();
    struct ext2_mount *fs = mnt;
    struct ext2_split sp;
    int rc = ext2_split_path(path, &sp);
    if (rc) goto out;
    if (ext2_split_is_dot(&sp)) { rc = LNXRM_EINVAL; goto out; }

    u32 dir_ino;
    rc = ext2_resolve_dir(fs, sp.parent, &dir_ino);
    if (rc) goto out;

    u32 ino;
    u64 off;
    rc = ext2_dir_find(fs, dir_ino, sp.name, sp.nlen, &ino, NULL, &off);
    if (rc) goto out;

    struct ext2_inode in;
    rc = ext2_read_inode(fs, ino, &in);
    if (rc) goto out;
    if ((in.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) {
        rc = LNXRM_ENOTDIR;
        goto out;
    }

    rc = ext2_dir_is_empty(fs, ino);
    if (rc <= 0) { rc = rc < 0 ? rc : LNXRM_ENOTEMPTY; goto out; }

    rc = ext2_dir_del_at(fs, dir_ino, off);
    if (rc) goto out;

    /* ext2_rmdir(): the entry goes, the child's two links go (its "." and
     * the parent's), the parent loses one, and the empty block it still
     * holds goes too. */
    int r = ext2_truncate_blocks(fs, &in, 0);
    if (r < 0) { rc = r; goto out; }
    ext2_inode_free(fs, ino); /* the group's used-dir count follows it */

    struct ext2_inode pin;
    rc = ext2_read_inode(fs, dir_ino, &pin);
    if (rc) goto out;
    if (pin.i_links_count) pin.i_links_count--;
    pin.i_mtime = now_secs();
    rc = ext2_write_inode(fs, dir_ino, &pin);
out:
    rc = ext2_finish(fs, rc);
    EXT2_LEAVE();
    return rc;
}

/* rename() may not put a directory inside its own subtree: renaming /a to
 * /a/b would hang the move point off itself with no way back out.
 * vfs_rename_path() compares only the two mounts (EXDEV for a cross-volume
 * move), so this check belongs to the filesystem.  Walking ".." from the
 * destination parent answers it on resolved inodes -- a string-prefix test
 * cannot, because abs_path() does not resolve "." and ".." and the same move
 * written as "a" -> "./a/b" would slip past one.  Sets *out when `candidate`
 * is `dir` itself or one of its ancestors. */
static int ext2_is_ancestor(struct ext2_mount *fs, u32 candidate, u32 dir,
                            bool *out)
{
    u32 probe = dir;
    /* Every directory on the volume can appear at most once on the way to
     * the root, so a walk longer than the inode count is a cycle in a
     * corrupt tree, not a legitimately deep one. */
    for (u32 steps = 0; steps < fs->inodes_count; steps++) {
        if (probe == candidate) { *out = true; return 0; }
        u32 parent;
        int rc = ext2_dir_find(fs, probe, "..", 2, &parent, NULL, NULL);
        if (rc == LNXRM_ENOENT) rc = LNXRM_EIO; /* every directory owns a ".." */
        if (rc) return rc;
        if (parent == probe) { *out = false; return 0; } /* reached the root */
        probe = parent;
    }
    return LNXRM_EIO; /* the chain never reached the root */
}

/* ext2_rename(), in the order Linux runs it (fs/ext2/namei.c):
 *   1. reject a move of a directory into itself,
 *   2. either retarget the destination record at the source (replacement)
 *      or add the source under the new name,
 *   3. stamp the source's ctime,
 *   4. delete the source's old record,
 *   5. for a directory: repoint ".." at the new parent, then step both
 *      parents' link counts -- new parent +1 only in the add branch (a
 *      replacement trades its victim's link for the source's), old parent
 *      -1 either way, which cancels when both ends share a parent. */
static int ext2_rename(void *mnt, const char *oldpath, const char *newpath)
{
    EXT2_ENTER();
    struct ext2_mount *fs = mnt;
    struct ext2_split os, ns;
    int rc;

    if (!strcmp(oldpath, newpath)) { rc = 0; goto out; } /* nothing to do */

    rc = ext2_split_path(oldpath, &os);
    if (rc) goto out;
    if (ext2_split_is_dot(&os)) { rc = LNXRM_EINVAL; goto out; }
    rc = ext2_split_path(newpath, &ns);
    if (rc) goto out;
    if (ext2_split_is_dot(&ns)) { rc = LNXRM_EINVAL; goto out; }

    u32 old_dir;
    rc = ext2_resolve_dir(fs, os.parent, &old_dir);
    if (rc) goto out;

    u32 old_ino;
    u8 old_ft;
    u64 old_off;
    rc = ext2_dir_find(fs, old_dir, os.name, os.nlen, &old_ino, &old_ft, &old_off);
    if (rc) goto out;

    struct ext2_inode old_in;
    rc = ext2_read_inode(fs, old_ino, &old_in);
    if (rc) goto out;
    bool old_is_dir = (old_in.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR;

    u32 new_dir;
    rc = ext2_resolve_dir(fs, ns.parent, &new_dir);
    if (rc) goto out;

    if (old_is_dir) {
        bool inside = false;
        rc = ext2_is_ancestor(fs, old_ino, new_dir, &inside);
        if (rc) goto out;
        if (inside) { rc = LNXRM_EINVAL; goto out; }
    }

    u32 new_ino = 0;
    u64 new_off = 0;
    rc = ext2_dir_find(fs, new_dir, ns.name, ns.nlen, &new_ino, NULL, &new_off);
    if (rc && rc != LNXRM_ENOENT) goto out;
    bool replacing = (rc == 0);
    rc = 0;
    /* Two names already pointing at one inode: rename between them changes
     * nothing.  The VFS answers 0 for that before Linux's ext2_rename() runs;
     * vfs_rename_path() does not, and without the guard the replacement half
     * below would free the source's own blocks. */
    if (replacing && new_ino == old_ino) { rc = 0; goto out; }

    if (replacing) {
        struct ext2_inode new_in;
        rc = ext2_read_inode(fs, new_ino, &new_in);
        if (rc) goto out;
        bool new_is_dir = (new_in.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR;
        /* Linux has vfs_rename() reject a type mismatch with EISDIR/ENOTDIR
         * first; there is no EISDIR in this errno table, so both directions
         * answer ENOTDIR. */
        if (new_is_dir != old_is_dir) { rc = LNXRM_ENOTDIR; goto out; }
        if (old_is_dir) {
            rc = ext2_dir_is_empty(fs, new_ino);
            if (rc <= 0) { rc = rc < 0 ? rc : LNXRM_ENOTEMPTY; goto out; }
            rc = 0;
        }

        /* Retarget the victim's own record rather than deleting it and
         * re-adding: the new name exists at every instant, so no failure in
         * between can leave the source unnamed. */
        rc = ext2_dir_set_at(fs, new_dir, new_off, old_ino, old_ft);
        if (rc) goto out;

        /* The victim loses the link its record was, and -- being a
         * directory -- the one its "." held: two steps for a directory, one
         * for a file, exactly drop_nlink() + inode_dec_link_count(). */
        new_in.i_ctime = now_secs();
        if (old_is_dir && new_in.i_links_count) new_in.i_links_count--;
        if (new_in.i_links_count) new_in.i_links_count--;
        if (new_in.i_links_count) {
            rc = ext2_write_inode(fs, new_ino, &new_in);
            if (rc) goto out;
        } else {
            /* Last link gone: the blocks go before the record does, or the
             * bitmap would keep bytes no inode can reach.  truncate answers
             * with the number of blocks it freed, so -- as in unlink and
             * rmdir -- only a negative value is a failure. */
            if (new_in.i_blocks) {
                int r = ext2_truncate_blocks(fs, &new_in, 0);
                if (r < 0) { rc = r; goto out; }
            }
            ext2_inode_free(fs, new_ino); /* wipes the record; used-dir count follows it */
        }
    } else {
        /* The new name is born pointing at the source.  Adding before
         * deleting keeps the source reachable if this fails: a rename that
         * runs out of space may leave an extra name, never no name. */
        rc = ext2_dir_add(fs, new_dir, ns.name, ns.nlen, old_ino, old_ft);
        if (rc) goto out;
        if (old_is_dir) {
            rc = ext2_dir_link(fs, new_dir, +1);
            if (rc) goto out;
        }
    }

    /* Linux stamps the source's ctime on every rename.  Re-read first: the
     * copy taken at the top predates the record surgery above. */
    struct ext2_inode stamp;
    rc = ext2_read_inode(fs, old_ino, &stamp);
    if (rc) goto out;
    stamp.i_ctime = now_secs();
    rc = ext2_write_inode(fs, old_ino, &stamp);
    if (rc) goto out;

    rc = ext2_dir_del_at(fs, old_dir, old_off);
    if (rc) goto out;

    if (old_is_dir) {
        if (old_dir != new_dir) {
            /* The moved directory's ".." follows it to the new parent. */
            rc = ext2_dir_set_dotdot(fs, old_ino, new_dir);
            if (rc) goto out;
        }
        rc = ext2_dir_link(fs, old_dir, -1);
        if (rc) goto out;
    }
out:
    rc = ext2_finish(fs, rc);
    EXT2_LEAVE();
    return rc;
}

/* ======================= mount ======================= */

void *ext2_mount(struct blkdev *dev)
{
    u8 raw[1024];
    /* The primary superblock starts at byte 1024, i.e. sectors 2 and 3. */
    if (blk_io_read(dev, 2, 1, raw) || blk_io_read(dev, 3, 1, raw + 512))
        return NULL;

    struct ext2_super_block *sb = (struct ext2_super_block *)raw;
    if (sb->s_magic != EXT2_SUPER_MAGIC) return NULL; /* not ext2: say nothing */

    if (sb->s_feature_compat & EXT2_COMPAT_HAS_JOURNAL) {
        kprintf("[ext2] refusing volume: it has a journal (ext3/4), which this driver does not replay\n");
        return NULL;
    }
    if (sb->s_feature_incompat & ~EXT2_INCOMPAT_SUPP) {
        kprintf("[ext2] refusing volume: unsupported incompat features 0x%x\n",
                sb->s_feature_incompat & ~EXT2_INCOMPAT_SUPP);
        return NULL;
    }
    if (sb->s_log_block_size > 2) {
        kprintf("[ext2] refusing volume: %u-byte blocks (driver serves 1024/2048/4096)\n",
                1024u << sb->s_log_block_size);
        return NULL;
    }
    if (!sb->s_blocks_count || !sb->s_inodes_count ||
        !sb->s_blocks_per_group || !sb->s_inodes_per_group) {
        kprintf("[ext2] refusing volume: zero geometry\n");
        return NULL;
    }

    struct ext2_mount *fs = (struct ext2_mount *)kmalloc(sizeof(*fs));
    if (!fs) return NULL;
    memset(fs, 0, sizeof(*fs));
    fs->dev = dev;
    fs->block_size = 1024u << sb->s_log_block_size;
    fs->sectors_per_block = fs->block_size / CACHE_SECTOR_SIZE;
    fs->blocks_count = sb->s_blocks_count;
    fs->inodes_count = sb->s_inodes_count;
    fs->first_data_block = sb->s_first_data_block;
    fs->blocks_per_group = sb->s_blocks_per_group;
    fs->inodes_per_group = sb->s_inodes_per_group;
    fs->inode_size = sb->s_rev_level ? sb->s_inode_size : 128;
    fs->first_ino = sb->s_rev_level ? sb->s_first_ino : 11;
    fs->free_blocks = sb->s_free_blocks_count;
    fs->free_inodes = sb->s_free_inodes_count;

    if (fs->inode_size < 128 || fs->block_size % fs->inode_size) {
        kprintf("[ext2] refusing volume: inode size %u does not fit %u-byte blocks\n",
                fs->inode_size, fs->block_size);
        kfree(fs);
        return NULL;
    }
    if (!fs->first_ino) fs->first_ino = 11;

    /* Never read past the device: a lying superblock would otherwise send
     * every lookup at sectors the disk does not have. */
    u64 dev_bytes = (u64)dev->num_sectors * dev->sector_size;
    if ((u64)fs->blocks_count * fs->block_size > dev_bytes) {
        kprintf("[ext2] refusing volume: %u blocks of %u B exceed the device\n",
                fs->blocks_count, fs->block_size);
        kfree(fs);
        return NULL;
    }

    u64 span = (u64)fs->blocks_count - fs->first_data_block;
    fs->groups = (u32)((span + fs->blocks_per_group - 1) / fs->blocks_per_group);
    if (!fs->groups) {
        kprintf("[ext2] refusing volume: no block groups\n");
        kfree(fs);
        return NULL;
    }

    /* The descriptor table sits in the block right after the superblock:
     * block 1 on a 4 KiB volume, block 2 on a 1 KiB one (super at block 1). */
    u32 gd_bytes = fs->groups * sizeof(struct ext2_group_desc);
    u32 gd_blocks = (gd_bytes + fs->block_size - 1) / fs->block_size;
    fs->gd = (struct ext2_group_desc *)kmalloc(gd_blocks * fs->block_size);
    if (!fs->gd) { kfree(fs); return NULL; }
    memset(fs->gd, 0, gd_blocks * fs->block_size);

    u32 gd_start = fs->first_data_block + 1;
    for (u32 b = 0; b < gd_blocks; b++) {
        if (ext2_read_data_block(fs, gd_start + b,
                                 (u8 *)fs->gd + (size_t)b * fs->block_size)) {
            kprintf("[ext2] refusing volume: group descriptor block %u unreadable\n",
                    gd_start + b);
            kfree(fs->gd);
            kfree(fs);
            return NULL;
        }
    }
    for (u32 g = 0; g < fs->groups; g++) {
        struct ext2_group_desc *d = &fs->gd[g];
        if (d->bg_block_bitmap >= fs->blocks_count ||
            d->bg_inode_bitmap >= fs->blocks_count ||
            d->bg_inode_table >= fs->blocks_count) {
            kprintf("[ext2] refusing volume: group %u descriptor points outside it\n", g);
            kfree(fs->gd);
            kfree(fs);
            return NULL;
        }
    }

    kprintf("[ext2] volume: %u blocks x %u B, %u inodes (%u B), %u group(s), %u free blocks\n",
            fs->blocks_count, fs->block_size, fs->inodes_count, fs->inode_size,
            fs->groups, fs->free_blocks);
    return fs;
}

struct fs_ops ext2_ops = {
    .lookup = ext2_lookup,
    .getdent = ext2_getdent,
    .read = ext2_read,
    .write = ext2_write,
    .size = ext2_size,
    .close_node = ext2_close_node,
    .create = ext2_create,
    .mkdir = ext2_mkdir,
    .unlink = ext2_unlink,
    .rmdir = ext2_rmdir,
    .rename = ext2_rename,
    .freespace = ext2_freespace,
};
