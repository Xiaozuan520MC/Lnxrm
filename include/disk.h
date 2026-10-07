/* Block layer: MBR partition parsing and sector cache.
 * Merges the old mbr.h + blk_cache.h. */
#pragma once
#include <types.h>
#include <sys/vfs.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- MBR partition table ---------------- */

/* MBR Partition Table Entry */
struct mbr_entry {
    u8 status;         /* 0x80 = active/bootable */
    u8 chs_first[3];   /* CHS of first sector */
    u8 type;           /* partition type */
    u8 chs_last[3];    /* CHS of last sector */
    u32 lba_first;     /* LBA of first sector */
    u32 sectors_count; /* number of sectors */
} __attribute__((packed));

/* MBR Signature */
#define MBR_SIGNATURE              0xAA55
#define MBR_PARTITION_TABLE_OFFSET 446
#define MBR_PARTITION_ENTRY_SIZE   16

/* Partition Types */
#define PART_TYPE_NONE       0x00
#define PART_TYPE_FAT12      0x01
#define PART_TYPE_FAT16_SM   0x04 /* <32MB */
#define PART_TYPE_EXTENDED   0x05
#define PART_TYPE_FAT16      0x06
#define PART_TYPE_FAT32      0x0B
#define PART_TYPE_FAT32_LBA  0x0C
#define PART_TYPE_FAT16_LBA  0x0E
#define PART_TYPE_EXT_LBA    0x0F
#define PART_TYPE_LINUX      0x83 /* ext2/ext3/ext4 */
#define PART_TYPE_LINUX_SWAP 0x82

/* Max partitions we track */
#define MBR_MAX_PARTITIONS 4

struct mbr_info {
    struct blkdev *dev;
    u32 total_sectors;
    u8 boot_ind; /* active partition index or -1 */
    struct {
        u8 type;
        u32 lba_first;
        u32 sectors_count;
        bool is_extended;
    } parts[MBR_MAX_PARTITIONS];
    int part_count;
};

/* Parse MBR from a block device. Returns 0 on success. */
int mbr_parse(struct blkdev *dev, struct mbr_info *out);

/* Get partition as a virtual block device (for reading partition contents) */
int mbr_get_partition(struct mbr_info *mbr, int index, struct blkdev *out_dev,
                      struct blkdev *parent);

/* Get string name for partition type */
const char *mbr_type_name(u8 type);

/* ---------------- raw device access (T-005) ---------------- */

/* Every kernel block transfer is supposed to go through blk_io_read() /
 * blk_io_write() instead of calling blkdev->read / blkdev->write directly.
 * Two reasons, both about failures:
 *   - one place that validates the request before it reaches the driver;
 *   - one place a test can break: blk_inject_io() makes transfers to a
 *     chosen LBA fail exactly as a dying disk would, so the filesystem's
 *     error paths can be driven on a machine whose disk is perfectly fine.
 *     Injecting at this level (rather than in the driver) means a cached
 *     sector cannot hide the fault: the filesystem sees the error whether
 *     or not the cache happens to hold the LBA.
 * Return value: 0 on success, nonzero on failure -- LNXRM_EIO from an
 * injected fault, or whatever the driver answers on a real one. */
#define BLK_IO_READ  0x1
#define BLK_IO_WRITE 0x2

int blk_io_read(struct blkdev *dev, u64 lba, u32 count, void *buf);
int blk_io_write(struct blkdev *dev, u64 lba, u32 count, const void *buf);

/* ktest only: while armed, transfers of either direction that cover `lba`
 * on `dev` fail with LNXRM_EIO.  Every caller must turn it back off before
 * asserting -- and before the next test runs, or it poisons unrelated I/O. */
void blk_inject_io(struct blkdev *dev, u64 lba, int ops);
void blk_inject_io_off(void);

/* ---------------- sector cache ---------------- */

#define CACHE_SECTOR_SIZE 512
#define CACHE_ENTRIES     64 /* Number of cached sectors */

struct cache_entry {
    u64 lba; /* Logical Block Address */
    u8 data[CACHE_SECTOR_SIZE];
    bool valid;         /* Is this entry valid? */
    bool dirty;         /* Does this entry need writeback? */
    struct blkdev *dev; /* Associated block device */
    /* Recency stamp (T-007): bumped on every hit, fill and stage, so the
     * victim -- the resident entry with the smallest stamp -- is the least
     * recently used one.  This replaced a single `used` bit that a hit set
     * and nothing ever cleared per entry: once all 64 slots were "used" the
     * only move left was to clear every bit and take slot 0, the same slot no
     * matter how recently it had been touched (C24).  There is no second
     * bookkeeping bit left to disagree with the stamp. */
    u64 stamp;
};

/* Initialize the cache system */
void blk_cache_init(void);

/* Read a sector through the cache */
int blk_cache_read(struct blkdev *dev, u64 lba, void *buf);

/* Write a sector through the cache */
int blk_cache_write(struct blkdev *dev, u64 lba, const void *buf);

/* Flush all dirty entries for a device */
int blk_cache_flush(struct blkdev *dev);

/* ---- counters, introspection and a known starting state (T-007/T-008) ----
 * "The cache is thrashing" and "reads see stale data" are claims about
 * behaviour no one can see from the outside, so the cache keeps a few
 * counters and answers one question directly: is this sector resident? */

struct blk_cache_stats {
    u64 hits;      /* lookup answered from a resident sector */
    u64 misses;    /* lookup that had to go to the device */
    u64 evictions; /* resident sectors dropped to make room for another */
    u64 writebacks;/* dirty sectors pushed out (eviction or flush) */
    u64 dev_reads;  /* blk_io_read transfers that reached a driver */
    u64 dev_writes; /* blk_io_write transfers that reached a driver */
};
/* Copy the counters out.  Counters only move in the self-test or under a
 * single writer; a torn 64-bit read is not worth a seqlock here. */
void blk_cache_stats_get(struct blk_cache_stats *out);
/* Zero them (measurement start line). */
void blk_cache_stats_reset(void);

/* Slot index holding (dev, lba), or -1 when it is not resident.  Lets a
 * test observe *which* sector was evicted instead of inferring it. */
int blk_cache_lookup(struct blkdev *dev, u64 lba);

/* Write back every dirty entry of `dev`, then drop all of its entries, so
 * the next lookup is guaranteed to reach the device.  Returns the flush
 * status (0 = everything reached the device).  Test/tooling only: nothing
 * in production wants to throw away a warm cache. */
int blk_cache_purge(struct blkdev *dev);

#ifdef __cplusplus
}
#endif
