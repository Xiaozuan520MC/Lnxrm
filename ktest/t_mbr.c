/* C36: the MBR layer must validate every partition entry where it parses
 * it -- a lie about range or size may never survive until I/O -- and must
 * walk the extended partition's EBR chain, where logical partitions used
 * to be invisible.
 *
 * The subject is a parser, so the test forges its disk: a kblk RAM device
 * carries a hand-built table the real volume could never show, including
 * an out-of-range entry and a multi-hop chain.  Only parsing and the
 * derived partition devices are exercised; mounting a FAT inside one is
 * fat_mount()'s job and stays covered on the real volume. */
#include <sys/ktest.h>
#include <disk.h>
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

static void test_mbr_ranges_and_the_ebr_chain(void)
{
    /* Disk: 8192 sectors (4 MiB).  Layout:
     *   p0 FAT32       [4096, 2048)             primary
     *   p1 extended    [1024, 1536)             primary, holds the chain
     *   p3 FAT32 LBA   [7168, 9216)             9216 > 8192: a range lie
     *   EBR#1 @1024:   logical [1056, 1696)     link +1024 -> EBR#2 @2048
     *   EBR#2 @2048:   logical [2064, 2368)     link  +512 -> EBR#3 @2560
     *   EBR#3 @2560:   logical [2568, 2584)     link LBA 0 -> chain end
     * Expected: 5 live partitions, the range lie dropped at parse time,
     * the chain followed exactly two hops and closed on the empty link. */
    struct kblk *k = kblk_make("kmbrcraft", 8192, 2600);
    K_ASSERT(k != NULL);

    u8 *mbr = kblk_sector(k, 0);
    K_ASSERT(mbr != NULL);
    *(u16 *)&mbr[510] = 0xAA55;
    put_entry(mbr + 446 + 0 * 16, 0x00, PART_TYPE_FAT32, 4096, 2048);
    put_entry(mbr + 446 + 1 * 16, 0x00, PART_TYPE_EXTENDED, 1024, 1536);
    /* slot 2 stays empty */
    put_entry(mbr + 446 + 3 * 16, 0x00, PART_TYPE_FAT32_LBA, 7168, 2048);

    u8 *e1 = kblk_sector(k, 1024);
    K_ASSERT(e1 != NULL);
    *(u16 *)&e1[510] = 0xAA55;
    put_entry(e1 + 446 + 0 * 16, 0x00, PART_TYPE_FAT32, 32, 640);
    put_entry(e1 + 446 + 1 * 16, 0x00, PART_TYPE_EXTENDED, 1024, 0);

    u8 *e2 = kblk_sector(k, 2048);
    K_ASSERT(e2 != NULL);
    *(u16 *)&e2[510] = 0xAA55;
    put_entry(e2 + 446 + 0 * 16, 0x00, PART_TYPE_FAT16_LBA, 16, 304);
    put_entry(e2 + 446 + 1 * 16, 0x00, PART_TYPE_EXT_LBA, 512, 0);

    u8 *e3 = kblk_sector(k, 2560);
    K_ASSERT(e3 != NULL);
    *(u16 *)&e3[510] = 0xAA55;
    put_entry(e3 + 446 + 0 * 16, 0x00, PART_TYPE_FAT32_LBA, 8, 16);
    /* entry 1 empty: the chain ends here */

    /* the second logical's first sector carries a marker, so reading
     * through a partition device proves the redirect really happened */
    u8 *mark = kblk_sector(k, 2064);
    K_ASSERT(mark != NULL);
    *(u32 *)&mark[0] = 0x20647A21;

    struct mbr_info mi;
    K_EXPECT_EQ(mbr_parse(&k->dev, &mi), 0);
    K_EXPECT_EQ((long)mi.part_count, 5); /* the range lie never lands */

    /* primaries in table order */
    K_EXPECT_EQ((long)mi.parts[0].type, (long)PART_TYPE_FAT32);
    K_EXPECT_EQ((long)mi.parts[0].lba_first, 4096);
    K_EXPECT_EQ((long)mi.parts[0].sectors_count, 2048);
    K_EXPECT(!mi.parts[0].is_extended);
    K_EXPECT_EQ((long)mi.parts[1].type, (long)PART_TYPE_EXTENDED);
    K_EXPECT_EQ((long)mi.parts[1].lba_first, 1024);
    K_EXPECT(mi.parts[1].is_extended);

    /* logicals, one per EBR, resolved to absolute addresses */
    K_EXPECT_EQ((long)mi.parts[2].type, (long)PART_TYPE_FAT32);
    K_EXPECT_EQ((long)mi.parts[2].lba_first, 1056);
    K_EXPECT_EQ((long)mi.parts[2].sectors_count, 640);
    K_EXPECT(!mi.parts[2].is_extended);
    K_EXPECT_EQ((long)mi.parts[3].lba_first, 2064);
    K_EXPECT_EQ((long)mi.parts[3].sectors_count, 304);
    K_EXPECT_EQ((long)mi.parts[4].lba_first, 2568);
    K_EXPECT_EQ((long)mi.parts[4].sectors_count, 16);
    K_EXPECT(!mi.parts[4].is_extended);

    /* a partition device handed out for a logical must read from the
     * logical's own start, not the whole disk's */
    struct blkdev ld;
    memset(&ld, 0, sizeof(ld));
    K_EXPECT_EQ(mbr_get_partition(&mi, 3, &ld, &k->dev), 0);
    K_EXPECT_EQ(ld.num_sectors, 304u);
    u8 probe[512];
    K_EXPECT_EQ(blk_io_read(&ld, 0, 1, probe), 0);
    K_EXPECT_EQ(*(u32 *)&probe[0], 0x20647A21u);

    /* out-of-range indices refuse */
    K_EXPECT(mbr_get_partition(&mi, 5, &ld, &k->dev) != 0);
    K_EXPECT(mbr_get_partition(&mi, -1, &ld, &k->dev) != 0);

    kblk_free(k);
}

static void test_mbr_signature_is_mandatory(void)
{
    struct kblk *k = kblk_make("kmbrcraft2", 1024, 8);
    K_ASSERT(k != NULL);
    u8 *s0 = kblk_sector(k, 0);
    K_ASSERT(s0 != NULL);
    s0[0] = 0xE9; /* a boot jump, but no 55AA anywhere */
    struct mbr_info mi;
    K_EXPECT_EQ(mbr_parse(&k->dev, &mi), LNXRM_EFAIL);
    K_EXPECT_EQ((long)mi.part_count, 0); /* nothing leaked from a bad sector */
    kblk_free(k);
}

KTEST("mbr", KTEST_LATE, test_mbr_ranges_and_the_ebr_chain);
KTEST("mbr", KTEST_LATE, test_mbr_signature_is_mandatory);
