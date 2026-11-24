// SPDX-License-Identifier: Apache-2.0

/* ext2 data-block layer: physical block I/O, logical->physical mapping
 * (bmap), block allocation out of the group bitmaps, and truncation.
 *
 * Every byte of I/O goes straight to the device through blk_io_read() and
 * blk_io_write(), one request per whole block (fs->sectors_per_block sectors
 * in a single call -- see ext2_read_data_block for why that granularity is
 * load-bearing).  The write-back cache is reserved for FAT32, whose journal
 * assumes staged sectors; ext2 persists each block before its caller returns,
 * so an ext2 operation has no staged tail to flush and cannot leave a
 * half-applied operation behind on a power-loss-shaped shutdown.
 *
 * Group geometry (proved against mke2fs 1.47.2 images, both 1 KiB and 4 KiB):
 *   group_start(g) = s_first_data_block + g * s_blocks_per_group
 *   block bitmap   = group descriptor bg_block_bitmap (absolute block)
 *   bit index      = blk - group_start(g), bit 0 = group_start
 * The descriptor table lives in the block right after the superblock, i.e.
 * first_data_block + 1 (block 1 at 4 KiB, block 2 at 1 KiB).
 *
 * Allocation never touches a group whose free count is already 0, and the
 * free counters in the descriptor and the superblock are stepped in the same
 * breath as the bitmap bit -- e2fsck -n compares all three and refuses to
 * pass a volume where they disagree. */
#include <ext2.h>

/* Bounds of group `g`: the first block it owns and one past the last one it
 * may hand out.  The last group is short when the volume is not a whole
 * number of groups; blocks before s_first_data_block (block 0 on a 1 KiB
 * volume, which is where the superblock's own bytes start at 4 KiB) are
 * never in any group. */
static u32 group_start(const struct ext2_mount *fs, u32 g)
{
    return fs->first_data_block + g * fs->blocks_per_group;
}

static u32 group_end(const struct ext2_mount *fs, u32 g)
{
    u64 end = (u64)group_start(fs, g) + fs->blocks_per_group;
    return end > fs->blocks_count ? fs->blocks_count : (u32)end;
}

/* Which group owns `blk`, or fs->groups when it owns nothing (the caller
 * treats that as "not a block of this volume"). */
static u32 group_of(const struct ext2_mount *fs, u32 blk)
{
    if (blk < fs->first_data_block) return fs->groups;
    u32 g = (blk - fs->first_data_block) / fs->blocks_per_group;
    return g < fs->groups ? g : fs->groups;
}

/* ---- physical block I/O ------------------------------------------------ */

/* Read/write one whole block, as ONE device request rather than one request
 * per sector.  A block outside the volume is refused with LNXRM_EIO and said
 * out loud: that answer is the forged-image case, where a corrupted i_block
 * names a block the device does not have, and writing one would hand the
 * device bytes it does not own.
 *
 * The granularity is load-bearing, not cosmetic: ahciBlkWrite() issues an ATA
 * FLUSH CACHE (0xE7) after every blk_io_write() it is given, so the bytes are
 * on the medium when the call answers, and QEMU answers 0xE7 with a flush of
 * the image file.  Eight 512-byte writes for one 4 KiB block were therefore
 * eight flushes; ktest's enospc case fills the 64 MiB root (all 15295 free
 * blocks) and frees it again, which is half a million flushes across the two
 * loops.  The failing run's timeline puts each at just under a
 * millisecond, so these loops alone are the whole 420 s wait
 * scripts/smoke_test.py gives the boot markers.  The run expired
 * inside the unlink that hands the space back, so the image was left with an
 * orphaned /ENOSPC.T inode and a bitmap freed halfway (what e2fsck -n then
 * reported as "free block count" and "block bitmap" differences).  One
 * request per block is one flush, and the driver takes a sector count
 * (AHCI_MAX_CHUNK = 256 sectors per command), so this costs one command
 * instead of eight on reads as well -- reads never flushed, but they did pay
 * eight command round trips. */
int ext2_read_data_block(struct ext2_mount *fs, u32 blk, void *buf)
{
    if (blk < fs->first_data_block || blk >= fs->blocks_count) {
        kprintf("[ext2] block %u beyond the volume (%u blocks), read refused\n",
                blk, fs->blocks_count);
        return LNXRM_EIO;
    }
    u64 lba = (u64)blk * fs->sectors_per_block;
    if (blk_io_read(fs->dev, lba, fs->sectors_per_block, buf))
        return LNXRM_EIO;
    return 0;
}

int ext2_write_data_block(struct ext2_mount *fs, u32 blk, const void *buf)
{
    if (blk < fs->first_data_block || blk >= fs->blocks_count) {
        kprintf("[ext2] block %u beyond the volume (%u blocks), write refused\n",
                blk, fs->blocks_count);
        return LNXRM_EIO;
    }
    u64 lba = (u64)blk * fs->sectors_per_block;
    if (blk_io_write(fs->dev, lba, fs->sectors_per_block, buf))
        return LNXRM_EIO;
    return 0;
}

/* ---- block bitmap ------------------------------------------------------ */

/* Find one free bit in group `g`'s bitmap and claim it.  The new block is
 * zeroed before it is handed out: it may still hold the tail of the file that
 * owned it, and a reader of the new file would see those bytes as its own. */
int ext2_block_alloc(struct ext2_mount *fs, u32 *out)
{
    if (!fs->groups) return LNXRM_ENOSPC;
    u8 *bm = (u8 *)kmalloc(fs->block_size);
    if (!bm) return LNXRM_ENOMEM;

    for (u32 g = 0; g < fs->groups; g++) {
        /* The descriptor's own count is the cheap test; a group that says it
         * is full is never scanned. */
        if (!fs->gd[g].bg_free_blocks_count) continue;
        if (ext2_read_data_block(fs, fs->gd[g].bg_block_bitmap, bm)) {
            kfree(bm);
            return LNXRM_EIO;
        }
        u32 first = group_start(fs, g);
        u32 last = group_end(fs, g);
        for (u32 blk = first; blk < last; blk++) {
            u32 bit = blk - first;
            if (bm[bit >> 3] & (u8)(1u << (bit & 7))) continue;

            bm[bit >> 3] |= (u8)(1u << (bit & 7));
            if (ext2_write_data_block(fs, fs->gd[g].bg_block_bitmap, bm)) {
                kfree(bm);
                return LNXRM_EIO;
            }
            fs->gd[g].bg_free_blocks_count--;
            fs->free_blocks--;
            fs->space_dirty = true;

            /* Reuse the buffer as the wipe: it has served its purpose as a
             * bitmap and the block it describes is now the answer. */
            memset(bm, 0, fs->block_size);
            int rc = ext2_write_data_block(fs, blk, bm);
            kfree(bm);
            if (rc) return LNXRM_EIO;
            *out = blk;
            return 0;
        }
    }
    kfree(bm);
    return LNXRM_ENOSPC;
}

/* Give a block back.  A block outside the volume is dropped silently: the
 * caller is unwinding and there is nobody left to answer to, but the counters
 * must not move for a block we never owned. */
void ext2_block_free(struct ext2_mount *fs, u32 blk)
{
    u32 g = group_of(fs, blk);
    if (g >= fs->groups) return;
    if (blk < group_start(fs, g) || blk >= group_end(fs, g)) return;

    u8 *bm = (u8 *)kmalloc(fs->block_size);
    if (!bm) {
        /* A leak we cannot record is still a leak: say it rather than let the
         * free counter drift away from the bitmap. */
        kprintf("[ext2] block %u free skipped: out of memory\n", blk);
        return;
    }
    if (ext2_read_data_block(fs, fs->gd[g].bg_block_bitmap, bm)) {
        kprintf("[ext2] block %u free skipped: bitmap unreadable\n", blk);
        kfree(bm);
        return;
    }
    u32 bit = blk - group_start(fs, g);
    if (!(bm[bit >> 3] & (u8)(1u << (bit & 7)))) {
        kfree(bm); /* already free: nothing to move */
        return;
    }
    bm[bit >> 3] &= (u8)~(1u << (bit & 7));
    if (ext2_write_data_block(fs, fs->gd[g].bg_block_bitmap, bm)) {
        kprintf("[ext2] block %u free skipped: bitmap unwritable\n", blk);
        kfree(bm);
        return;
    }
    fs->gd[g].bg_free_blocks_count++;
    fs->free_blocks++;
    fs->space_dirty = true;
    kfree(bm);
}

/* ---- mapping ------------------------------------------------------------ */

/* Logical -> physical, read-only.  A hole answers *pblk = 0 (the caller
 * zero-fills); a logical block past the double-indirect range answers
 * LNXRM_EIO, which no reachable offset on a 64 MiB volume can hit -- 12 +
 * 1024 + 1024*1024 blocks is 4 GiB at 4 KiB a block. */
int ext2_bmap(struct ext2_mount *fs, const struct ext2_inode *in, u32 lblk, u32 *pblk)
{
    u32 per = fs->block_size / 4; /* u32 entries per indirect block */
    *pblk = 0;

    if (lblk < 12) {
        *pblk = in->i_block[lblk];
        return 0;
    }
    lblk -= 12;

    if (lblk < per) {
        if (!in->i_block[12]) return 0; /* hole */
        u32 *tab = (u32 *)kmalloc(fs->block_size);
        if (!tab) return LNXRM_ENOMEM;
        int rc = ext2_read_data_block(fs, in->i_block[12], tab);
        if (!rc) *pblk = tab[lblk];
        kfree(tab);
        return rc;
    }
    lblk -= per;

    if ((u64)lblk >= (u64)per * per) return LNXRM_EIO;
    if (!in->i_block[13]) return 0; /* hole */

    u32 *tab = (u32 *)kmalloc(fs->block_size);
    if (!tab) return LNXRM_ENOMEM;
    int rc = ext2_read_data_block(fs, in->i_block[13], tab);
    if (!rc) {
        u32 l2 = tab[lblk / per];
        if (!l2) { /* hole at the second level */
            kfree(tab);
            return 0;
        }
        rc = ext2_read_data_block(fs, l2, tab);
        if (!rc) *pblk = tab[lblk % per];
    }
    kfree(tab);
    return rc;
}

/* The growing half of bmap: allocate every missing piece on the path to
 * `lblk` -- the data block, and the single/double indirect blocks that name
 * it -- and report the physical block.  i_block[] and i_blocks move with it;
 * the caller persists the inode with ext2_write_inode(). */
int ext2_bmap_alloc(struct ext2_mount *fs, struct ext2_inode *in, u32 lblk, u32 *pblk)
{
    u32 per = fs->block_size / 4;
    u32 spb = fs->sectors_per_block; /* one block, in the i_blocks unit */

    if (lblk < 12) {
        if (!in->i_block[lblk]) {
            int rc = ext2_block_alloc(fs, &in->i_block[lblk]);
            if (rc) return rc;
            in->i_blocks += spb;
        }
        *pblk = in->i_block[lblk];
        return 0;
    }
    lblk -= 12;

    if (lblk < per) {
        if (!in->i_block[12]) {
            int rc = ext2_block_alloc(fs, &in->i_block[12]);
            if (rc) return rc;
            in->i_blocks += spb;
        }
        u32 *tab = (u32 *)kmalloc(fs->block_size);
        if (!tab) return LNXRM_ENOMEM;
        int rc = ext2_read_data_block(fs, in->i_block[12], tab);
        if (!rc && !tab[lblk]) {
            u32 nb = 0;
            rc = ext2_block_alloc(fs, &nb);
            if (!rc) {
                tab[lblk] = nb;
                in->i_blocks += spb;
                rc = ext2_write_data_block(fs, in->i_block[12], tab);
            }
        }
        if (!rc) *pblk = tab[lblk];
        kfree(tab);
        return rc;
    }
    lblk -= per;

    if ((u64)lblk >= (u64)per * per) return LNXRM_EIO;
    u32 i1 = lblk / per, i2 = lblk % per;

    if (!in->i_block[13]) {
        int rc = ext2_block_alloc(fs, &in->i_block[13]);
        if (rc) return rc;
        in->i_blocks += spb;
    }
    /* One buffer serves both levels in turn: the level-1 table is written
     * back (if it changed) before the level-2 table is read into the same
     * space, so nothing live is ever overwritten. */
    u32 *tab = (u32 *)kmalloc(fs->block_size);
    if (!tab) return LNXRM_ENOMEM;

    int rc = ext2_read_data_block(fs, in->i_block[13], tab);
    if (!rc && !tab[i1]) {
        u32 nb = 0;
        rc = ext2_block_alloc(fs, &nb);
        if (!rc) {
            tab[i1] = nb;
            in->i_blocks += spb;
            rc = ext2_write_data_block(fs, in->i_block[13], tab);
        }
    }
    u32 l2 = rc ? 0 : tab[i1];
    if (!rc) {
        rc = ext2_read_data_block(fs, l2, tab);
        if (!rc && !tab[i2]) {
            u32 nb = 0;
            rc = ext2_block_alloc(fs, &nb);
            if (!rc) {
                tab[i2] = nb;
                in->i_blocks += spb;
                rc = ext2_write_data_block(fs, l2, tab);
            }
        }
        if (!rc) *pblk = tab[i2];
    }
    kfree(tab);
    return rc;
}

/* Release the blocks reachable from the table block `*slot` whose LOGICAL
 * index is >= `keep`.  `base` is the logical index the table's entry 0 maps
 * (12 for the single-indirect table, 12+per for the double, ...), so one
 * routine serves every level: `depth` is how many table levels sit between
 * this one and the data (0 = its entries are data blocks).
 *
 * Entries wholly below `keep` are left alone -- including the table block
 * itself, which is why `*slot` is cleared only when nothing survived.  A
 * keep that falls in the MIDDLE of an entry's range (the double-indirect
 * case, where the boundary lands inside one second-level table) descends
 * into that entry instead of dropping it.
 *
 * Returns the number of blocks freed -- a table block counts once it is
 * released -- or a negative error. */
static int free_from(struct ext2_mount *fs, u32 *slot, u32 keep, int depth, u32 base)
{
    u32 tab = *slot;
    u32 per = fs->block_size / 4;
    u32 span = 1; /* logical blocks covered by one entry at this level */
    for (int i = 0; i < depth; i++) span *= per;

    u32 first = 0;
    bool straddle = false;
    if (keep > base) {
        u32 off = keep - base;
        first = off / span;
        straddle = (off % span) != 0;
        if (first >= per) return 0; /* the whole table maps below `keep` */
    }

    u32 *buf = (u32 *)kmalloc(fs->block_size);
    if (!buf) return LNXRM_ENOMEM;
    int rc = ext2_read_data_block(fs, tab, buf);
    if (rc) {
        kfree(buf);
        return rc;
    }

    int freed = 0;
    for (u32 i = first; i < per; i++) {
        if (!buf[i]) continue;
        if (!depth) {
            ext2_block_free(fs, buf[i]);
            buf[i] = 0;
            freed++;
            continue;
        }
        bool keep_table = straddle && i == first;
        int r = free_from(fs, &buf[i], keep_table ? keep : 0, depth - 1,
                          base + i * span);
        if (r < 0) {
            kfree(buf);
            return r;
        }
        freed += r; /* a released sub-table already counted itself */
    }

    /* The table survives only if it still names something. */
    bool alive = false;
    for (u32 i = 0; i < per; i++) {
        if (buf[i]) { alive = true; break; }
    }
    if (alive) {
        rc = ext2_write_data_block(fs, tab, buf);
    } else {
        ext2_block_free(fs, tab);
        *slot = 0;
        freed++;
    }
    kfree(buf);
    return rc < 0 ? rc : freed;
}

int ext2_truncate_blocks(struct ext2_mount *fs, struct ext2_inode *in, u32 keep_lblk)
{
    u32 per = fs->block_size / 4;
    u32 spb = fs->sectors_per_block;
    int freed = 0;
    int r;

    /* Direct blocks: logical 0..11, no table in between. */
    u32 dstart = keep_lblk < 12 ? keep_lblk : 12;
    for (u32 i = dstart; i < 12; i++) {
        if (!in->i_block[i]) continue;
        ext2_block_free(fs, in->i_block[i]);
        in->i_block[i] = 0;
        in->i_blocks -= spb;
        freed++;
    }

    /* Indirect tables: one call each, base = the first logical block the
     * table maps.  Every block they release -- data and table alike -- was
     * counted in i_blocks when it was allocated, so the count comes back
     * down by exactly the same unit. */
    if (in->i_block[12]) {
        r = free_from(fs, &in->i_block[12], keep_lblk, 0, 12);
        if (r < 0) return r;
        freed += r;
        in->i_blocks -= (u32)r * spb;
    }
    if (in->i_block[13]) {
        r = free_from(fs, &in->i_block[13], keep_lblk, 1, 12 + per);
        if (r < 0) return r;
        freed += r;
        in->i_blocks -= (u32)r * spb;
    }
    if (in->i_block[14]) {
        /* This driver's bmap never builds a triple-indirect chain, so one
         * only exists if another tool wrote the file.  Release it rather
         * than orphan it -- a truncate to `keep` inside its range still
         * frees everything past `keep`, which is what free_from does. */
        r = free_from(fs, &in->i_block[14], keep_lblk, 2, 12 + per + per * per);
        if (r < 0) return r;
        freed += r;
        in->i_blocks -= (u32)r * spb;
    }

    return freed;
}
