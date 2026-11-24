// SPDX-License-Identifier: Apache-2.0

/* RAM block device for ktest cases that must drive mount/partition code
 * against a volume they forge byte by byte.  The backing store covers only
 * the first `backed` sectors; a forged BPB may claim a giant geometry the
 * RAM never carries -- reads beyond it answer zeros, so code that checks
 * bounds before touching data still sees the shapes its checks act on. */
#ifndef LNXRM_KBLK_H
#define LNXRM_KBLK_H

#include <sys/vfs.h>
#include <types.h>
#include <console.h> /* memset */

struct kblk {
    struct blkdev dev;
    u32 reads, writes; /* blk_io_* calls that reached us */
    u32 backed;        /* sectors covered by data[] */
    u8 *data;
};

struct kblk *kblk_make(const char *name, u64 sectors, u32 backed);
/* Pointer to a forged sector, or NULL when outside the backing store
 * (test cases assert the pointer before writing). */
u8 *kblk_sector(struct kblk *k, u64 lba);
void kblk_free(struct kblk *k);

/* One 16-byte MBR partition entry, written where a real disk would carry
 * it: status byte, type byte, LBA and sector count. */
static inline void put_entry(u8 *st, u8 status, u8 type, u32 lba, u32 count)
{
    memset(st, 0, 16);
    st[0] = status;
    st[4] = type;
    *(u32 *)&st[8] = lba;
    *(u32 *)&st[12] = count;
}

/* A boot sector fat_mount() accepts: 512 B/sector, `spc` sectors per
 * cluster (power of two), `reserved` reserved sectors (the journal lives in
 * 8..reserved-1), `numfats` FATs of `fatsz32` sectors, `totsec` sectors all
 * in and `rootclk` as the root directory cluster.  Everything the driver
 * does not read stays zero, which is what makes a forged volume's root
 * directory read back as empty. */
void kblk_forge_fat32_bpb(u8 *s, u32 spc, u32 reserved, u32 numfats, u32 fatsz32,
                          u32 totsec, u32 rootclk);

/* A whole FAT32 volume, not just its boot sector: the BPB above with a
 * mirrored FAT header (root cluster marked EOC) and the entire 2 MiB
 * writable -- the seed ktest_seed_fat_volume() mounts so the FAT cases have
 * a volume when "/" is ext2. */
struct kblk *kblk_forge_fat32_volume(const char *name);

/* A whole ext2 volume: 4 KiB blocks over three groups, one root directory,
 * bitmaps and free counts that agree with each other -- what ext2_mount()
 * and the t_ext2 cases drive the driver against. */
struct kblk *kblk_forge_ext2(const char *name);

#endif
