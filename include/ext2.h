// SPDX-License-Identifier: Apache-2.0

/* Shared surface of the ext2 driver: on-disk structures, mount state and
 * the cross-file entry points of ext2.c / block/block.c / inode/inode.c.
 * The root volume is an ext2 superfloppy since S2 (2026-11-19), so this
 * header is the sibling of fat32.h -- both are consumed by vfs.c (mount
 * probing) and by the boot self-test (ktest/t_ext2.c).
 *
 * Scope: a plain ext2 (rev 0 or rev 1) with no journal -- ext2 by
 * definition, not ext3/4.  The incompat features we understand are FILETYPE
 * only; anything else (extents, 64-bit, MMP, ...) is refused at mount with
 * a printed reason rather than misread as data. */
#ifndef LNXRM_EXT2_H
#define LNXRM_EXT2_H

#include <types.h>
#include <sys/vfs.h>
#include <console.h> /* kprintf + memset/memcpy, the driver's whole toolkit */
#include <mm/mm.h>   /* kmalloc/kfree: bitmap and indirect-block buffers */
#include <disk.h>
#include <sys/spinlock.h>

#define EXT2_SUPER_MAGIC   0xEF53
#define EXT2_SUPER_OFFSET  1024 /* byte offset of the primary superblock */

/* s_feature_incompat bits, in the positions the spec puts them.  FILETYPE
 * means directory entries carry a file_type byte at offset 7; without it
 * that byte is the first byte of the name. */
#define EXT2_INCOMPAT_FILETYPE   0x0002
#define EXT2_INCOMPAT_SUPP       (EXT2_INCOMPAT_FILETYPE)
/* compat bit we actively refuse: a journal means ext3 semantics that this
 * driver does not run, so mounting it as ext2 would read a log it never
 * replays. */
#define EXT2_COMPAT_HAS_JOURNAL  0x0004

/* file_type byte values (FILETYPE incompat) -> the DT_* numbers the VFS
 * hands to user space (4 = dir, 8 = reg, 3 = chr, matching fat32_dir.c). */
#define EXT2_FT_UNKNOWN 0
#define EXT2_FT_REG     1
#define EXT2_FT_DIR     2
#define EXT2_FT_CHR     3

/* i_mode type bits (the high nibble of st_mode's S_IFMT). */
#define EXT2_S_IFMT   0xF000
#define EXT2_S_IFREG  0x8000
#define EXT2_S_IFDIR  0x4000
#define EXT2_S_IFCHR  0x2000

/* The root directory's inode number -- inode 1 is the bad-blocks record and
 * inode 2 is defined to be the root on every ext2 volume. */
#define EXT2_ROOT_INO 2u

/* Directory-entry record length: 8 bytes of header padded out to a multiple
 * of 4 (spec: EXT2_DIR_REC_LEN).  A name of n bytes therefore occupies
 * ((n + 11) & ~3) bytes, and 12 is the smallest record any entry can have. */
#define EXT2_DIR_PAD    4
#define EXT2_DIR_ROUND  (EXT2_DIR_PAD - 1)
#define EXT2_DIR_REC_LEN(n) \
    ((((u32)(n)) + 8u + EXT2_DIR_ROUND) & ~(u32)EXT2_DIR_ROUND)

/* ---- on-disk structures (spec: ext2.txt, all little-endian) ---------- */

struct ext2_super_block {
    u32 s_inodes_count;      /*  0 */
    u32 s_blocks_count;      /*  4 */
    u32 s_r_blocks_count;    /*  8: reserved for root, nobody enforces it */
    u32 s_free_blocks_count; /* 12: kept in step with the bitmaps */
    u32 s_free_inodes_count; /* 16 */
    u32 s_first_data_block;  /* 20: 1 for 1 KiB blocks, 0 otherwise */
    u32 s_log_block_size;    /* 24: block_size = 1024 << this */
    u32 s_log_frag_size;     /* 28: fragments do not exist in ext2 */
    u32 s_blocks_per_group;  /* 32 */
    u32 s_frags_per_group;   /* 36 */
    u32 s_inodes_per_group;  /* 40 */
    u32 s_mtime;             /* 44 */
    u32 s_wtime;             /* 48 */
    u16 s_mnt_count;         /* 52 */
    u16 s_max_mnt_count;     /* 54 */
    u16 s_magic;             /* 56 */
    u16 s_state;             /* 58: 1 = valid, 2 = errors detected */
    u16 s_errors;            /* 60: behaviour on error */
    u16 s_minor_rev_level;   /* 62 */
    u32 s_lastcheck;         /* 64 */
    u32 s_checkinterval;     /* 68 */
    u32 s_creator_os;        /* 72: 0 = Linux */
    u32 s_rev_level;         /* 76: 0 = static, 1 = dynamic */
    u16 s_def_resuid;        /* 80 */
    u16 s_def_resgid;        /* 82 */
    /* -- dynamic rev (rev 1) fields; 0 on a rev 0 volume -- */
    u32 s_first_ino;         /* 84: first non-reserved inode (11) */
    u16 s_inode_size;        /* 88: 128 on rev 0, 256 with mke2fs defaults */
    u16 s_block_group_nr;    /* 90 */
    u32 s_feature_compat;    /* 92 */
    u32 s_feature_incompat;  /* 96 */
    u32 s_feature_ro_compat; /* 100 */
    u8  s_uuid[16];          /* 104 */
    char s_volume_name[16];  /* 120 */
    char s_last_mounted[64]; /* 136 */
    u32 s_algo_bitmap;       /* 200 */
    /* The rest (prealloc, journal uuid, extra_isize, ...) is never read:
     * every consumer below takes its facts from the fields above.  Pad so
     * the struct is the 1024 bytes the primary superblock occupies. */
    u8  s_pad[1024 - 204];
} __attribute__((packed));
LNXRM_STATIC_ASSERT(sizeof(struct ext2_super_block) == 1024, "ext2 superblock");

/* Group descriptor: 32 bytes on disk (rev 0 size; rev 1 adds a checksum
 * field we neither write nor verify -- mke2fs -t ext2 does not set the
 * metadata_csum feature). */
struct ext2_group_desc {
    u32 bg_block_bitmap;      /*  0: absolute block number */
    u32 bg_inode_bitmap;      /*  4 */
    u32 bg_inode_table;       /*  8 */
    u16 bg_free_blocks_count; /* 12 */
    u16 bg_free_inodes_count; /* 14 */
    u16 bg_used_dirs_count;   /* 16 */
    u16 bg_pad;               /* 18 */
    u32 bg_reserved[3];       /* 20 */
} __attribute__((packed));
LNXRM_STATIC_ASSERT(sizeof(struct ext2_group_desc) == 32, "ext2 group descriptor");

/* The 128-byte inode body.  A volume with s_inode_size = 256 (mke2fs's
 * default) stores 128 more bytes after this -- extended attributes and
 * i_extra_isize -- which this driver never touches: it reads and rewrites
 * exactly the first 128 bytes and leaves the tail alone (read-modify-write
 * through the block cache). */
struct ext2_inode {
    u16 i_mode;         /*  0 */
    u16 i_uid;          /*  2 */
    u32 i_size;         /*  4: bytes (i_dir_acl is the high half for files
                                * rev >= 1 with LARGE_FILE -- not used) */
    u32 i_atime;        /*  8 */
    u32 i_ctime;        /* 12 */
    u32 i_mtime;        /* 16 */
    u32 i_dtime;        /* 20: deletion time, 0 while alive */
    u16 i_gid;          /* 24 */
    u16 i_links_count;  /* 26 */
    u32 i_blocks;       /* 28: allocated bytes / 512, INDIRECT BLOCKS COUNTED */
    u32 i_flags;        /* 32 */
    u32 i_osd1;         /* 36 */
    u32 i_block[15];    /* 40: 12 direct, [12] single, [13] double, [14] triple */
    u32 i_generation;   /* 100 */
    u32 i_file_acl;     /* 104 */
    u32 i_dir_acl;      /* 108 */
    u32 i_faddr;        /* 112 */
    u8  i_osd2[12];     /* 116 */
} __attribute__((packed));
LNXRM_STATIC_ASSERT(sizeof(struct ext2_inode) == 128, "ext2 inode");

/* Directory entry.  rec_len is the distance to the next entry (or to the
 * end of the block for the last one), name_len is a u8 so a component is
 * at most 255 bytes.  file_type only exists when the FILETYPE incompat
 * feature is on -- this driver refuses volumes without it, so it always
 * reads the byte at offset 7 as a type. */
struct ext2_dirent {
    u32 inode;     /*  0: 0 = the slot is free */
    u16 rec_len;   /*  4: >= 8 and 4-byte aligned, always */
    u8  name_len;  /*  6 */
    u8  file_type; /*  7 */
    char name[256];/*  8 */
} __attribute__((packed));

/* ---- mount state ------------------------------------------------------ */

struct ext2_mount {
    struct blkdev *dev;
    /* geometry, copied out of the superblock at mount */
    u32 blocks_count;
    u32 inodes_count;
    u32 first_data_block;
    u32 blocks_per_group;
    u32 inodes_per_group;
    u32 block_size;      /* bytes: 1024 << s_log_block_size */
    u32 sectors_per_block; /* block_size / 512 */
    u32 inode_size;      /* bytes: 128 on rev 0 */
    u32 first_ino;       /* first allocatable inode */
    u32 groups;          /* ceil((blocks_count - first_data_block) / bpg) */
    /* live free-space counters: the superblock's mirror, updated under the
     * fs lock on every alloc/free and written back with it (e2fsck compares
     * them against the bitmaps, so they must never drift). */
    u32 free_blocks;
    u32 free_inodes;
    struct ext2_group_desc *gd; /* `groups` descriptors, loaded at mount */
    /* Set by every allocator and released by ext2_sync_space(): a cheap
     * "the counters on disk are stale" flag, so an operation that allocated
     * nothing does not pay for a superblock and descriptor rewrite. */
    bool space_dirty;
};

/* The single filesystem lock, in fat32.h's shape: serialises allocation,
 * directory editing and the inode table against each other.  Never taken
 * from IRQ context and never held across a schedule. */
extern spinlock_t ext2_fs_lock;
#define EXT2_ENTER() spin_lock_irqon(&ext2_fs_lock)
#define EXT2_LEAVE() spin_unlock_irqon(&ext2_fs_lock)

/* The fs_ops table (defined in ext2.c) and the mount entry vfs's
 * find_volume() probes.  ext2_mount() returns NULL (with a printed reason)
 * for anything that is not a volume this driver can serve honestly. */
extern struct fs_ops ext2_ops;
void *ext2_mount(struct blkdev *dev);

/* The ext2 mount that owns "/", or NULL when the root is not ext2.  Set by
 * vfs_try_mount_one() at the moment an ext2 volume takes the root slot, and
 * never overwritten -- "/" is assigned once.  Unlike fat_priv there is no
 * "else the first mounted" rule to keep: every ext2 entry point takes its
 * mount as an argument, so the pointer exists for the boot self-test, which
 * compares a root vnode's mnt_data against the mount that answered. */
extern void *ext2_priv;

/* The fs node lookup() hands out: just who it is, because every operation
 * re-reads the inode from disk (the same coherence rule fat_size_impl()
 * follows -- a second open descriptor must see the first one's writes). */
struct ext2_node {
    u32 ino;
    u8 is_dir;
};

/* ---- block/block.c: data blocks --------------------------------------- *
 * The four S2 capabilities live here and in ext2.c: read/write one
 * PHYSICAL block, and the file-level read/write the fs_ops table exposes.
 * All I/O goes through blk_cache_read()/blk_cache_write(), never the raw
 * device -- a raw read would miss staged writes and serve stale bytes. */

/* Read/write one whole block at `blk` (block_size bytes in `buf`).  A block
 * number past the volume is refused with LNXRM_EIO and said out loud: that
 * is the forged-image case (a corrupted i_block), and writing one would
 * hand the device bytes it does not own. */
int ext2_read_data_block(struct ext2_mount *fs, u32 blk, void *buf);
int ext2_write_data_block(struct ext2_mount *fs, u32 blk, const void *buf);

/* Logical -> physical mapping.  ext2_bmap() never allocates: a hole answers
 * *pblk = 0 (the caller zero-fills), an unreadable or out-of-volume block
 * answers LNXRM_EIO.  ext2_bmap_alloc() is the growing half: it allocates
 * every missing piece on the path (data block, single/double indirect
 * block) and counts it into i_blocks.  Both expect the fs lock held. */
int ext2_bmap(struct ext2_mount *fs, const struct ext2_inode *in, u32 lblk, u32 *pblk);
int ext2_bmap_alloc(struct ext2_mount *fs, struct ext2_inode *in, u32 lblk, u32 *pblk);

/* Free every block the mapping still names at or beyond `keep_lblk`
 * (truncate), updating i_blocks and the free counters.  Returns the number
 * of blocks freed, or LNXRM_EIO on a read failure. */
int ext2_truncate_blocks(struct ext2_mount *fs, struct ext2_inode *in, u32 keep_lblk);

/* Raw block-bitmap allocation (used by bmap_alloc): one free block of the
 * volume, LNXRM_ENOSPC when there is none.  ext2_block_free() gives it
 * back.  The fs lock is the caller's. */
int ext2_block_alloc(struct ext2_mount *fs, u32 *out);
void ext2_block_free(struct ext2_mount *fs, u32 blk);

/* ---- inode/inode.c: the inode table ----------------------------------- */

/* Read/write the 128-byte inode body of `ino` (1-based).  A write is a
 * read-modify-write of the containing block so the 256-byte tail (extended
 * attributes this driver does not own) survives untouched. */
int ext2_read_inode(struct ext2_mount *fs, u32 ino, struct ext2_inode *out);
int ext2_write_inode(struct ext2_mount *fs, u32 ino, const struct ext2_inode *in);

/* Allocate a free inode (bitmap scan starting at first_ino), initialising
 * it to zeroes with the counters stepped; ext2_inode_free() clears it
 * again.  ENOSPC when the volume is out. */
int ext2_inode_alloc(struct ext2_mount *fs, u32 *ino);
void ext2_inode_free(struct ext2_mount *fs, u32 ino);

/* Persist the superblock and every group descriptor (free counts, used-dir
 * counts).  Called after any allocation/free so the on-disk accounting an
 * e2fsck reads matches the bitmaps. */
void ext2_sync_space(struct ext2_mount *fs);

#endif /* LNXRM_EXT2_H */
