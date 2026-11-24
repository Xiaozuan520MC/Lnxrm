// SPDX-License-Identifier: Apache-2.0

/* The device half of kblk.h.  Raw blkdev, no cache, no injection: this is
 * the lowest layer the parser and mount code see. */
#include "kblk.h"
#include <ext2.h> /* the structures kblk_forge_ext2() fills in */
#include <mm/mm.h>
#include <string.h>

static int kblk_read(struct blkdev *dev, u64 lba, u32 count, void *buf)
{
    struct kblk *k = dev->drv;
    if (lba + count > dev->num_sectors) return LNXRM_EIO;
    k->reads++;
    u8 *dst = buf;
    for (u32 s = 0; s < count; s++, lba++) {
        if (lba < k->backed)
            memcpy(dst, k->data + lba * 512, 512);
        else
            memset(dst, 0, 512);
        dst += 512;
    }
    return 0;
}

static int kblk_write(struct blkdev *dev, u64 lba, u32 count, const void *buf)
{
    struct kblk *k = dev->drv;
    if (lba + count > dev->num_sectors || lba + count > k->backed) return LNXRM_EIO;
    k->writes++;
    memcpy(k->data + lba * 512, buf, (size_t)count * 512);
    return 0;
}

struct kblk *kblk_make(const char *name, u64 sectors, u32 backed)
{
    struct kblk *k = kmalloc(sizeof(*k));
    if (!k) return NULL;
    memset(k, 0, sizeof(*k));
    k->backed = backed;
    k->data = kmalloc((size_t)backed * 512);
    if (!k->data) {
        kfree(k);
        return NULL;
    }
    memset(k->data, 0, (size_t)backed * 512);
    k->dev.name = name;
    k->dev.sector_size = 512;
    k->dev.num_sectors = sectors;
    k->dev.read = kblk_read;
    k->dev.write = kblk_write;
    k->dev.drv = k;
    return k;
}

u8 *kblk_sector(struct kblk *k, u64 lba)
{
    /* NULL instead of K_ASSERT: the helper can return a value, so the
     * loud refusal happens at the call site's K_ASSERT(ptr). */
    if (lba >= k->backed) return NULL;
    return k->data + lba * 512;
}

void kblk_forge_fat32_bpb(u8 *s, u32 spc, u32 reserved, u32 numfats, u32 fatsz32,
                          u32 totsec, u32 rootclk)
{
    memset(s, 0, 512);
    s[0] = 0xEB, s[1] = 0x3C, s[2] = 0x90;
    *(u16 *)&s[11] = 512;
    s[13] = (u8)spc;
    *(u16 *)&s[14] = (u16)reserved;
    s[16] = (u8)numfats;
    *(u32 *)&s[32] = totsec;
    *(u32 *)&s[36] = fatsz32;
    *(u32 *)&s[44] = rootclk;
    *(u16 *)&s[510] = 0xAA55;
    memcpy(&s[82], "FAT32", 5);
}

struct kblk *kblk_forge_fat32_volume(const char *name)
{
    /* The seed volume ktest_seed_fat_volume() mounts when "/" is ext2.
     * 4096 sectors (2 MiB) give every FAT case room to create its files,
     * two 32-sector FATs cover the 3968 data clusters exactly, and
     * reserved = 64 puts fat_journal_mount()'s log at LBA 8 with 56 sectors
     * -- the same journal shape the boot log has always printed.  The whole
     * volume is backed: these cases write here, and a write past the
     * backing store answers EIO instead of storing anything. */
    enum { SECTORS = 4096, RESERVED = 64, FATSZ = 32 };
    struct kblk *k = kblk_make(name, SECTORS, SECTORS);
    if (!k) return NULL;

    u8 *s0 = kblk_sector(k, 0);
    if (!s0) { kblk_free(k); return NULL; }
    kblk_forge_fat32_bpb(s0, 1, RESERVED, 2, FATSZ, SECTORS, 2);

    /* FAT header: media/dirty cells at 0 and 1, and FAT[2] = EOC for the
     * root directory cluster -- a zero there reads as a free cell, so every
     * directory walk would open with a corruption warning.  FAT2 mirrors
     * FAT1 the way mkfs leaves it. */
    u8 *fat1 = kblk_sector(k, RESERVED);
    u8 *fat2 = kblk_sector(k, RESERVED + FATSZ);
    if (!fat1 || !fat2) { kblk_free(k); return NULL; }
    memset(fat1, 0, (size_t)FATSZ * 512);
    u32 media = 0x0FFFFFF8, eoc = 0x0FFFFFFF;
    memcpy(fat1, &media, 4);
    memcpy(fat1 + 4, &eoc, 4);
    memcpy(fat1 + 8, &eoc, 4);
    memcpy(fat2, fat1, (size_t)FATSZ * 512);
    return k;
}

/* Bits [from, to) of a bitmap block. */
static void forge_set_bits(u8 *bm, u32 from, u32 to)
{
    for (u32 b = from; b < to; b++) bm[b / 8] |= (u8)(1u << (b % 8));
}

/* Pointer to the first sector of block `b` of a 4 KiB-block forge (the
 * backing store is contiguous, so a whole block can be written from it). */
static u8 *forge_block(struct kblk *k, u32 b)
{
    return kblk_sector(k, (u64)b * 8);
}

struct kblk *kblk_forge_ext2(const char *name)
{
    /* 4 KiB blocks, 192 of them (768 KiB) over three 64-block groups, 192
     * inodes of 256 bytes (64 per group).  The shipped image is one group;
     * three are the point of this one -- an allocator that only ever finds
     * room in group 0 passes every root-volume test and is still wrong on
     * the first disk that fills up.  The volume stays under 1 MiB because
     * its whole backing store is one kmalloc, and the heap is at its most
     * carved-up by the time the late cases run.
     *
     * Metadata placement follows mke2fs: group 0 hangs its off the
     * superblock (block 0 carries the superblock at byte 1024, block 1 the
     * descriptors, then block bitmap, inode bitmap, four inode-table
     * blocks), while groups 1 and 2 put their own three metadata blocks
     * first in their own range.  Root is inode 2, its directory block 8 --
     * the first data block of group 0.  Every free count below is the
     * number of clear bits in the bitmap beside it, because ext2_mount()
     * takes them as-is and the cases assert what the driver does with them. */
    enum {
        BLOCKS = 192,
        BPG = 64,
        INODES = 192,
        IPG = 64,
        INOSZ = 256,
        SECTORS = BLOCKS * 8 /* 4 KiB blocks on a 512-byte device */
    };
    struct kblk *k = kblk_make(name, SECTORS, SECTORS);
    if (!k) return NULL;

    struct ext2_super_block *sb = (struct ext2_super_block *)kblk_sector(k, 2);
    struct ext2_group_desc *gd = (struct ext2_group_desc *)forge_block(k, 1);
    if (!sb || !gd) { kblk_free(k); return NULL; }

    /* -- superblock (byte 1024 of block 0; the boot block stays zero) -- */
    memset(sb, 0, sizeof(*sb));
    sb->s_inodes_count = INODES;
    sb->s_blocks_count = BLOCKS;
    sb->s_free_blocks_count = 55 + 58 + 58; /* per-group, below */
    sb->s_free_inodes_count = 54 + 64 + 64;
    sb->s_first_data_block = 0;
    sb->s_log_block_size = 2; /* 4096 = 1024 << 2 */
    sb->s_blocks_per_group = BPG;
    sb->s_frags_per_group = BPG;
    sb->s_inodes_per_group = IPG;
    sb->s_magic = EXT2_SUPER_MAGIC;
    sb->s_state = 1; /* valid */
    sb->s_errors = 1; /* continue past errors */
    sb->s_rev_level = 1; /* dynamic: first_ino/inode_size fields below count */
    sb->s_first_ino = 11;
    sb->s_inode_size = INOSZ;
    /* The compat set mke2fs -t ext2 writes (EXT_ATTR|RESIZE_INODE|DIR_INDEX,
     * i.e. 0x38): only HAS_JOURNAL is ever inspected, and it is not one. */
    sb->s_feature_compat = 0x38;
    sb->s_feature_incompat = EXT2_INCOMPAT_FILETYPE;
    sb->s_feature_ro_compat = 0x3; /* SPARSE_SUPER | LARGE_FILE, as mke2fs */

    /* -- group descriptors (block 1) -- */
    memset(gd, 0, 4096); /* one block holds all three plus padding */
    gd[0].bg_block_bitmap = 2;
    gd[0].bg_inode_bitmap = 3;
    gd[0].bg_inode_table = 4;
    gd[0].bg_free_blocks_count = 55; /* 64 - super/GDT/bmaps/itable/root */
    gd[0].bg_free_inodes_count = 54; /* 64 - the ten reserved inodes */
    gd[0].bg_used_dirs_count = 1;    /* root */
    gd[1].bg_block_bitmap = 64;
    gd[1].bg_inode_bitmap = 65;
    gd[1].bg_inode_table = 66;
    gd[1].bg_free_blocks_count = 58; /* 64 - bmap/imap/4 table blocks */
    gd[1].bg_free_inodes_count = 64;
    gd[2].bg_block_bitmap = 128;
    gd[2].bg_inode_bitmap = 129;
    gd[2].bg_inode_table = 130;
    gd[2].bg_free_blocks_count = 58;
    gd[2].bg_free_inodes_count = 64;

    /* -- bitmaps: exactly the blocks and inodes named above are taken -- */
    u8 *bm = forge_block(k, 2); /* group 0 block bitmap: blocks 0..8 */
    if (!bm) { kblk_free(k); return NULL; }
    forge_set_bits(bm, 0, 9);
    bm = forge_block(k, 3); /* group 0 inode bitmap: inodes 1..10 reserved */
    if (!bm) { kblk_free(k); return NULL; }
    forge_set_bits(bm, 0, 10);
    bm = forge_block(k, 64); /* group 1: its six metadata blocks */
    if (!bm) { kblk_free(k); return NULL; }
    forge_set_bits(bm, 0, 6);
    bm = forge_block(k, 128); /* group 2: same shape */
    if (!bm) { kblk_free(k); return NULL; }
    forge_set_bits(bm, 0, 6);
    /* group 1/2 inode bitmaps (blocks 65/129) stay zero: all 64 inodes of
     * each are free, which is what the descriptors above claim. */

    /* -- root inode: inode 2, first record of group 0's table + 256 B -- */
    u8 *itable0 = forge_block(k, 4);
    if (!itable0) { kblk_free(k); return NULL; }
    struct ext2_inode *root = (struct ext2_inode *)(itable0 + 256);
    memset(root, 0, INOSZ); /* the 128-byte tail is part of the record too */
    root->i_mode = EXT2_S_IFDIR | 0755;
    root->i_size = 4096;
    /* One fixed timestamp: the forge is byte-identical between runs. */
    root->i_atime = root->i_ctime = root->i_mtime = 1700000000;
    root->i_links_count = 2; /* "." and no subdirectory yet */
    root->i_blocks = 8;      /* 4096 bytes / 512 */
    root->i_block[0] = 8;

    /* -- root directory (block 8): "." then ".." spanning the block -- */
    u8 *dir = forge_block(k, 8);
    if (!dir) { kblk_free(k); return NULL; }
    memset(dir, 0, 4096);
    u32 ino2 = EXT2_ROOT_INO;
    u16 rec = 12;
    memcpy(dir + 0, &ino2, 4);
    memcpy(dir + 4, &rec, 2);
    dir[6] = 1;
    dir[7] = EXT2_FT_DIR;
    dir[8] = '.';
    rec = (u16)(4096 - 12); /* the chain lands exactly on the block's end */
    memcpy(dir + 12, &ino2, 4);
    memcpy(dir + 16, &rec, 2);
    dir[18] = 2;
    dir[19] = EXT2_FT_DIR;
    dir[20] = '.';
    dir[21] = '.';

    return k;
}

void kblk_free(struct kblk *k)
{
    if (!k) return;
    kfree(k->data);
    kfree(k);
}
