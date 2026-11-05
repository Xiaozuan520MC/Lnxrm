/* C28: the block registry is what diskinfo enumerates, and two things about
 * it were wrong.  Parsed partitions never entered it at all, so a disk with
 * a perfectly good partition table showed exactly one device and none of its
 * contents; and once the fixed table filled, a newcomer was accepted by the
 * log and dropped by the table -- the count stopped, but the "first device"
 * pointer was still moved, so two views of the registry disagreed.
 *
 * The shipped disk image is a superfloppy (mkfs.vfat writes no MBR), so the
 * partition path never runs at boot: this case forges a partition table the
 * way t_mbr.c does and drives the same two calls the mount probe makes.
 * The registry itself is parked for the duration, so nothing a case
 * registers survives into diskinfo. */
#include <sys/ktest.h>
#include <sys/vfs.h>
#include <disk.h>
#include <mm/mm.h>
#include <string.h>
#include "kblk.h"

static void put_entry(u8 *st, u8 status, u8 type, u32 lba, u32 count)
{
    memset(st, 0, 16);
    st[0] = status;
    st[4] = type;
    *(u32 *)&st[8] = lba;
    *(u32 *)&st[12] = count;
}

/* A partition reaches the registry the way any device does: mbr_parse() ->
 * mbr_get_partition() -> blk_register(), the sequence vfs_try_mount_one()
 * runs per entry.  Once it is in, what diskinfo would print for it is the
 * registry's own record -- name, sector size, sector count. */
static void test_partition_devices_reach_the_registry(void)
{
    struct kblk *k = kblk_make("kblkpart", 8192, 8);
    K_ASSERT(k != NULL);
    u8 *mbr = kblk_sector(k, 0);
    K_ASSERT(mbr != NULL);
    *(u16 *)&mbr[510] = 0xAA55;
    put_entry(mbr + 446 + 0 * 16, 0x00, PART_TYPE_FAT32, 4096, 2048);
    put_entry(mbr + 446 + 1 * 16, 0x00, PART_TYPE_EXTENDED, 1024, 1536);
    put_entry(mbr + 446 + 2 * 16, 0x00, PART_TYPE_LINUX, 6144, 1024);

    struct mbr_info mi;
    K_ASSERT_EQ(mbr_parse(&k->dev, &mi), 0);
    K_EXPECT_EQ((long)mi.part_count, 3);

    struct blkdev *p0 = kmalloc(sizeof(*p0));
    struct blkdev *p2 = kmalloc(sizeof(*p2));
    K_ASSERT(p0 != NULL && p2 != NULL);
    K_ASSERT_EQ(mbr_get_partition(&mi, 0, p0, &k->dev), 0);
    K_ASSERT_EQ(mbr_get_partition(&mi, 2, p2, &k->dev), 0);

    /* From here on the case only uses K_EXPECT: it must always reach the
     * put() at the end, or the boot disks stay parked and the registry the
     * system runs on afterwards is empty. */
    int parked = blk_registry_take();
    K_EXPECT(parked >= 1); /* the boot disk was in there */

    K_EXPECT_EQ(blk_registry_count(), 0);
    K_EXPECT_EQ(blk_register(p0), 0);
    K_EXPECT_EQ(blk_register(p2), 0);
    K_EXPECT_EQ(blk_registry_count(), 2);
    K_EXPECT_EQ((u64)blk_registry_at(0), (u64)p0);
    K_EXPECT_EQ((u64)blk_registry_at(1), (u64)p2);
    /* the 28 bytes diskinfo packs out of an entry */
    K_EXPECT(p0->name != NULL && p0->name[0] != 0);
    K_EXPECT_EQ(p0->num_sectors, 2048u);
    K_EXPECT_EQ(p2->num_sectors, 1024u);
    K_EXPECT_EQ(p2->sector_size, 512u);

    blk_registry_put();
    K_EXPECT_EQ(blk_registry_count(), parked);

    /* the registry no longer points at them, so the objects this case
     * created are its to release */
    kfree(p0->drv);
    kfree(p0);
    kfree(p2->drv);
    kfree(p2);
    kblk_free(k);
}

/* The full table is a refusal, not a partial accept: the count and the last
 * entry are exactly as they were, and the newcomer is reported.  (What used
 * to happen was worse than a silent drop -- the array refused the device
 * while a second "first device" pointer still took it, so callers saw two
 * different answers about what the system held.) */
static void test_a_full_registry_refuses_without_side_effects(void)
{
    static struct blkdev fake[BLK_MAX + 1];
    memset(fake, 0, sizeof(fake));
    for (int i = 0; i < BLK_MAX + 1; i++) {
        fake[i].name = "kfakereg";
        fake[i].sector_size = 512;
        fake[i].num_sectors = 1;
    }

    int parked = blk_registry_take();
    K_EXPECT_EQ(blk_registry_count(), 0);

    for (int i = 0; i < BLK_MAX; i++)
        K_EXPECT_EQ(blk_register(&fake[i]), 0);
    K_EXPECT_EQ(blk_registry_count(), BLK_MAX); /* capacity is exactly BLK_MAX */

    K_EXPECT_EQ(blk_register(&fake[BLK_MAX]), (u64)LNXRM_EFAIL);
    K_EXPECT_EQ(blk_registry_count(), BLK_MAX);
    K_EXPECT_EQ((u64)blk_registry_at(BLK_MAX - 1), (u64)&fake[BLK_MAX - 1]);
    K_EXPECT(blk_registry_at(BLK_MAX) == NULL); /* one past the end: absent */

    blk_registry_put();
    K_EXPECT_EQ(blk_registry_count(), parked >= 1 ? parked : 0);
}

KTEST("blk", KTEST_LATE, test_partition_devices_reach_the_registry);
KTEST("blk", KTEST_LATE, test_a_full_registry_refuses_without_side_effects);
