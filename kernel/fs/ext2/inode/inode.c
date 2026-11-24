// SPDX-License-Identifier: Apache-2.0

/* ext2 inode table: where an inode's 128-byte body lives, how to read and
 * rewrite it without disturbing the bytes this driver does not own, and the
 * inode bitmap that hands numbers out.
 *
 * Layout (mke2fs 1.47.2 defaults, both 1 KiB and 4 KiB images):
 *   inode number is 1-based; group = (ino-1) / s_inodes_per_group
 *   byte offset  = bg_inode_table * block_size + (ino-1)%ipg * s_inode_size
 * s_inode_size is 128 on rev 0 and 256 with mke2fs's default extra_isize,
 * so a write is a read-modify-write of the containing block: the 128 bytes
 * after the body (extended attributes this driver never touches) survive
 * untouched.  ext2_mount() refuses a geometry where an inode could straddle
 * a block boundary, which is what makes one block read enough.
 *
 * The free counters stepped here are the ones e2fsck -n compares against
 * both bitmaps -- superblock, group descriptor and bitmap must agree or the
 * volume does not check out after the kernel has written to it. */
#include <ext2.h>

/* Where inode `ino`'s record starts: the block that holds it and the byte
 * offset inside that block.  Refuses an inode outside the volume rather
 * than computing a location for it. */
static int inode_loc(struct ext2_mount *fs, u32 ino, u32 *blk, u32 *off)
{
    if (!ino || ino > fs->inodes_count) return LNXRM_EINVAL;
    u32 idx = ino - 1;
    u32 g = idx / fs->inodes_per_group;
    if (g >= fs->groups) return LNXRM_EINVAL;
    u32 byte = (idx % fs->inodes_per_group) * fs->inode_size;
    *blk = fs->gd[g].bg_inode_table + byte / fs->block_size;
    *off = byte % fs->block_size;
    return 0;
}

int ext2_read_inode(struct ext2_mount *fs, u32 ino, struct ext2_inode *out)
{
    u32 blk, off;
    int rc = inode_loc(fs, ino, &blk, &off);
    if (rc) return rc;

    u8 *buf = (u8 *)kmalloc(fs->block_size);
    if (!buf) return LNXRM_ENOMEM;
    rc = ext2_read_data_block(fs, blk, buf);
    if (!rc) memcpy(out, buf + off, sizeof(struct ext2_inode));
    kfree(buf);
    return rc;
}

int ext2_write_inode(struct ext2_mount *fs, u32 ino, const struct ext2_inode *in)
{
    u32 blk, off;
    int rc = inode_loc(fs, ino, &blk, &off);
    if (rc) return rc;

    u8 *buf = (u8 *)kmalloc(fs->block_size);
    if (!buf) return LNXRM_ENOMEM;
    rc = ext2_read_data_block(fs, blk, buf);
    if (!rc) {
        memcpy(buf + off, in, sizeof(struct ext2_inode));
        rc = ext2_write_data_block(fs, blk, buf);
    }
    kfree(buf);
    return rc;
}

/* Blank the whole record -- body and the 128-byte tail alike.  A freshly
 * allocated inode must not hand its next owner the previous one's block
 * pointers, and a freed one must not look live to e2fsck (which reads
 * i_links_count out of the body to decide whether the bitmap is telling the
 * truth). */
static void wipe_inode(struct ext2_mount *fs, u32 ino)
{
    u32 blk, off;
    if (inode_loc(fs, ino, &blk, &off)) return;

    u8 *buf = (u8 *)kmalloc(fs->block_size);
    if (!buf) return;
    if (!ext2_read_data_block(fs, blk, buf)) {
        memset(buf + off, 0, fs->inode_size);
        ext2_write_data_block(fs, blk, buf);
    }
    kfree(buf);
}

int ext2_inode_alloc(struct ext2_mount *fs, u32 *out)
{
    u8 *bm = (u8 *)kmalloc(fs->block_size);
    if (!bm) return LNXRM_ENOMEM;

    for (u32 g = 0; g < fs->groups; g++) {
        if (!fs->gd[g].bg_free_inodes_count) continue;
        if (ext2_read_data_block(fs, fs->gd[g].bg_inode_bitmap, bm)) {
            kfree(bm);
            return LNXRM_EIO;
        }
        for (u32 i = 0; i < fs->inodes_per_group; i++) {
            u32 ino = g * fs->inodes_per_group + i + 1;
            /* Inodes below s_first_ino belong to the reserved set (bad
             * blocks, root, acl): a file may never be handed one. */
            if (ino < fs->first_ino) continue;
            if (bm[i >> 3] & (u8)(1u << (i & 7))) continue;

            bm[i >> 3] |= (u8)(1u << (i & 7));
            if (ext2_write_data_block(fs, fs->gd[g].bg_inode_bitmap, bm)) {
                kfree(bm);
                return LNXRM_EIO;
            }
            fs->gd[g].bg_free_inodes_count--;
            fs->free_inodes--;
            fs->space_dirty = true;
            kfree(bm);

            wipe_inode(fs, ino);
            *out = ino;
            return 0;
        }
    }
    kfree(bm);
    return LNXRM_ENOSPC;
}

void ext2_inode_free(struct ext2_mount *fs, u32 ino)
{
    u32 blk, off;
    if (inode_loc(fs, ino, &blk, &off)) return;
    u32 g = (ino - 1) / fs->inodes_per_group;
    u32 i = (ino - 1) % fs->inodes_per_group;

    /* Whether this inode was a directory decides bg_used_dirs_count, and the
     * only place that fact lives is the record -- which is about to be
     * wiped.  Read it first: e2fsck counts directory inodes per group and
     * compares the two, so a free that forgot to step it down would be a
     * volume that stops checking out. */
    struct ext2_inode in;
    bool was_dir = ext2_read_inode(fs, ino, &in) == 0 &&
                   (in.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR;

    u8 *bm = (u8 *)kmalloc(fs->block_size);
    if (!bm) {
        kprintf("[ext2] inode %u free skipped: out of memory\n", ino);
        return;
    }
    if (ext2_read_data_block(fs, fs->gd[g].bg_inode_bitmap, bm)) {
        kprintf("[ext2] inode %u free skipped: bitmap unreadable\n", ino);
        kfree(bm);
        return;
    }
    if (!(bm[i >> 3] & (u8)(1u << (i & 7)))) {
        kfree(bm); /* already free */
        return;
    }
    bm[i >> 3] &= (u8)~(1u << (i & 7));
    if (ext2_write_data_block(fs, fs->gd[g].bg_inode_bitmap, bm)) {
        kprintf("[ext2] inode %u free skipped: bitmap unwritable\n", ino);
        kfree(bm);
        return;
    }
    fs->gd[g].bg_free_inodes_count++;
    fs->free_inodes++;
    fs->space_dirty = true;
    if (was_dir && fs->gd[g].bg_used_dirs_count)
        fs->gd[g].bg_used_dirs_count--;
    kfree(bm);

    /* i_links_count must read 0 while the bitmap says free, or e2fsck
     * reports the two disagreeing. */
    wipe_inode(fs, ino);
}

/* Persist the superblock's free counts and every group descriptor.  Called
 * after any allocation or free: e2fsck -n compares the descriptor counters
 * against both bitmaps, so an update that never reaches the disk is a
 * volume that stops checking out the moment the kernel has written to it. */
void ext2_sync_space(struct ext2_mount *fs)
{
    u32 bs = fs->block_size;
    u8 *buf = (u8 *)kmalloc(bs);
    if (!buf) {
        kprintf("[ext2] free-space counters not persisted: out of memory\n");
        return;
    }

    /* The primary superblock lives at byte 1024 of the volume, which is
     * block 1 on a 1 KiB filesystem and inside block 0 otherwise. */
    u32 sb_blk = EXT2_SUPER_OFFSET / bs;
    if (!ext2_read_data_block(fs, sb_blk, buf)) {
        struct ext2_super_block *sb =
            (struct ext2_super_block *)(buf + (EXT2_SUPER_OFFSET % bs));
        sb->s_free_blocks_count = fs->free_blocks;
        sb->s_free_inodes_count = fs->free_inodes;
        ext2_write_data_block(fs, sb_blk, buf);
    } else {
        kprintf("[ext2] superblock free counts not written\n");
    }

    /* The descriptor table starts in the block right after the superblock
     * (first_data_block + 1: block 1 at 4 KiB, block 2 at 1 KiB). */
    u32 perblk = bs / sizeof(struct ext2_group_desc);
    u32 ngd_blk = (fs->groups + perblk - 1) / perblk;
    u32 gd_blk = fs->first_data_block + 1;
    for (u32 b = 0; b < ngd_blk; b++) {
        if (ext2_read_data_block(fs, gd_blk + b, buf)) {
            kprintf("[ext2] group descriptors not written\n");
            break;
        }
        for (u32 j = 0; j < perblk; j++) {
            u32 idx = b * perblk + j;
            if (idx >= fs->groups) break;
            memcpy(buf + j * sizeof(struct ext2_group_desc), &fs->gd[idx],
                   sizeof(struct ext2_group_desc));
        }
        ext2_write_data_block(fs, gd_blk + b, buf);
    }

    kfree(buf);
    /* Everything on disk now says what the counters say. */
    fs->space_dirty = false;
}
