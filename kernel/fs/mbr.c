/* MBR Partition Table Parser */
#include <disk.h>
#include <sys/vfs.h>
#include <console.h>
#include <mm/mm.h>

/* Virtual partition device for reading partition contents */
struct part_dev {
    struct blkdev *parent;
    u32 start_lba;
    u32 size_sectors;
};

static int part_read(struct blkdev *dev, u64 lba, u32 count, void *buf)
{
    struct part_dev *pd = dev->drv;
    if (lba + count > pd->size_sectors) return LNXRM_EFAIL;
    return blk_io_read(pd->parent, pd->start_lba + lba, count, buf);
}

static int part_write(struct blkdev *dev, u64 lba, u32 count, const void *buf)
{
    struct part_dev *pd = dev->drv;
    if (lba + count > pd->size_sectors) return LNXRM_EFAIL;
    return blk_io_write(pd->parent, pd->start_lba + lba, count, buf);
}

int mbr_parse(struct blkdev *dev, struct mbr_info *out)
{
    u8 sector[512];

    memset(out, 0, sizeof(*out));
    out->dev = dev;
    out->boot_ind = -1;
    out->part_count = 0;
    out->total_sectors = dev->num_sectors;

    kprintf("[mbr] parsing MBR on %s (%llu sectors, %llu MB)\n", dev->name, dev->num_sectors,
            dev->num_sectors / 2048);

    /* Read sector 0 (MBR) */
    if (blk_io_read(dev, 0, 1, sector)) {
        kprintf("[mbr] failed to read sector 0\n");
        return LNXRM_EFAIL;
    }

    /* Check signature */
    u16 sig = *(u16 *)(sector + 510);
    if (sig != MBR_SIGNATURE) {
        kprintf("[mbr] invalid signature 0x%04X (expected 0xAA55)\n", sig);
        /* dump first 64 bytes of sector 0 for debugging */
        kprintf("[mbr] sector 0 hex: ");
        for (int i = 0; i < 64; i++) kprintf("%02X ", sector[i]);
        kprintf("\n");
        return LNXRM_EFAIL;
    }

    kprintf("[mbr] signature OK (0xAA55)\n");

    /* Parse partition table */
    for (int i = 0; i < MBR_MAX_PARTITIONS; i++) {
        struct mbr_entry *e = (struct mbr_entry *)(sector + MBR_PARTITION_TABLE_OFFSET +
                                                   i * MBR_PARTITION_ENTRY_SIZE);

        if (e->type == PART_TYPE_NONE) continue;

        out->parts[out->part_count].type = e->type;
        out->parts[out->part_count].lba_first = e->lba_first;
        out->parts[out->part_count].sectors_count = e->sectors_count;
        out->parts[out->part_count].is_extended =
            (e->type == PART_TYPE_EXTENDED || e->type == PART_TYPE_EXT_LBA);

        if (e->status & 0x80) out->boot_ind = out->part_count;

        u64 part_size_mb = (u64)e->sectors_count / 2048;
        kprintf("[mbr] partition %d: status=0x%02X type=0x%02X (%s) lba=%u sectors=%u (%llu MB)\n",
                i, e->status, e->type, mbr_type_name(e->type), e->lba_first, e->sectors_count,
                part_size_mb);

        out->part_count++;
    }

    kprintf("[mbr] found %d partition(s), boot=%d\n", out->part_count, out->boot_ind);

    return 0;
}

int mbr_get_partition(struct mbr_info *mbr, int index, struct blkdev *out_dev,
                      struct blkdev *parent)
{
    if (index < 0 || index >= mbr->part_count) {
        kprintf("[mbr] get_partition: invalid index %d (count=%d)\n", index, mbr->part_count);
        return LNXRM_EFAIL;
    }

    struct part_dev *pd = kmalloc(sizeof(*pd));
    if (!pd) {
        kprintf("[mbr] get_partition: out of memory\n");
        return LNXRM_EFAIL;
    }

    pd->parent = parent;
    pd->start_lba = mbr->parts[index].lba_first;
    pd->size_sectors = mbr->parts[index].sectors_count;

    /* Build name manually since kernel doesn't have snprintf */
    static char part_name_buf[16];
    int nlen = 0;
    const char *src = parent->name;
    while (*src && nlen < 12) part_name_buf[nlen++] = *src++;
    part_name_buf[nlen++] = '0' + index + 1;
    part_name_buf[nlen] = 0;

    out_dev->name = part_name_buf;
    out_dev->sector_size = parent->sector_size;
    out_dev->num_sectors = mbr->parts[index].sectors_count;
    out_dev->read = part_read;
    out_dev->write = part_write;
    out_dev->drv = pd;

    kprintf("[mbr] partition %d: start_lba=%u size_sectors=%u -> %s\n", index, pd->start_lba,
            pd->size_sectors, part_name_buf);

    return 0;
}

const char *mbr_type_name(u8 type)
{
    switch (type) {
    case PART_TYPE_NONE:
        return "Empty";
    case PART_TYPE_FAT12:
        return "FAT12";
    case PART_TYPE_FAT16_SM:
        return "FAT16 (<32MB)";
    case PART_TYPE_EXTENDED:
        return "Extended";
    case PART_TYPE_FAT16:
        return "FAT16";
    case PART_TYPE_FAT32:
        return "FAT32";
    case PART_TYPE_FAT32_LBA:
        return "FAT32 LBA";
    case PART_TYPE_FAT16_LBA:
        return "FAT16 LBA";
    case PART_TYPE_EXT_LBA:
        return "Extended LBA";
    case PART_TYPE_LINUX:
        return "Linux (ext2/3/4)";
    case PART_TYPE_LINUX_SWAP:
        return "Linux Swap";
    default:
        return "Unknown";
    }
}
