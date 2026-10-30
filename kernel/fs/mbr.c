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
    /* The name lives in the object that uses it, not in a file-scope buffer.
     * blkdev.name is a plain pointer, and one `static char[16]` meant every
     * partition of every device resolved to the *same* string: enumerate two
     * partitions (or a second disk) and the mount table and the logs start
     * reporting one name for all of them.  It also ties the string's
     * lifetime to the blkdev that points at it. */
    char name[16];
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

/* C36: hard ceiling on the extended-partition chain walked at parse time.
 * A legal EBR chain always moves forward through the disk, so the bound is
 * about hostile tables, not real layouts. */
#define EBR_CHAIN_MAX 32

/* Record one validated entry in `out`.  `idx` is only for the log line.
 * Returns false (and prints why) when the entry lies off the disk or is
 * empty-sized -- C36: range violations die here, not at I/O time. */
static bool mbr_take(struct blkdev *dev, struct mbr_info *out, u8 status, u8 type, u64 lba,
                     u32 count, const char *idx)
{
    if (count == 0 || lba + count > dev->num_sectors) {
        kprintf("[mbr] %s %s lba=%llu+%u outside the disk (%llu sectors) "
                "-- dropped\n",
                idx, mbr_type_name(type), lba, count, dev->num_sectors);
        return false;
    }
    if (out->part_count >= MBR_MAX_PARTITIONS) {
        kprintf("[mbr] %s dropped: partition table full (%d)\n", idx, MBR_MAX_PARTITIONS);
        return false;
    }
    out->parts[out->part_count].type = type;
    out->parts[out->part_count].lba_first = (u32)lba;
    out->parts[out->part_count].sectors_count = count;
    out->parts[out->part_count].is_extended =
        (type == PART_TYPE_EXTENDED || type == PART_TYPE_EXT_LBA);
    kprintf("[mbr] %s: status=0x%02X type=0x%02X (%s) lba=%llu sectors=%u (%llu MB)\n", idx,
            status, type, mbr_type_name(type), lba, count, (u64)count / 2048);
    out->part_count++;
    return true;
}

/* Read one EBR-sector table entry out of a raw MBR/EBR sector. */
static struct mbr_entry *mbr_slot(const u8 *sector, int slot)
{
    return (struct mbr_entry *)(sector + MBR_PARTITION_TABLE_OFFSET +
                                slot * MBR_PARTITION_ENTRY_SIZE);
}

static bool mbr_is_ext(u8 type)
{ return type == PART_TYPE_EXTENDED || type == PART_TYPE_EXT_LBA; }

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

    /* Parse the primary table. */
    u32 ext_lba = 0;
    for (int i = 0; i < 4; i++) {
        struct mbr_entry *e = mbr_slot(sector, i);
        if (e->type == PART_TYPE_NONE) continue;
        char idx[16];
        /* no snprintf in the kernel: "partition <slot>" by hand */
        memcpy(idx, "partition ", 10);
        idx[10] = (char)('0' + i);
        idx[11] = 0;
        if (!mbr_take(dev, out, e->status, e->type, e->lba_first, e->sectors_count, idx))
            continue;
        if (e->status & 0x80) out->boot_ind = out->part_count - 1;
        if (mbr_is_ext(e->type) && !ext_lba) ext_lba = e->lba_first;
    }

    /* C36: extended partitions hold a chain of EBRs, one logical
     * partition each in that EBR's entry 0 and a forward link in entry 1
     * relative to the EBR itself.  The old parser stopped at the primaries,
     * so every logical partition was silently invisible. */
    u32 ebr = ext_lba;
    for (u32 depth = 0; ebr && depth < EBR_CHAIN_MAX; depth++) {
        if ((u64)ebr >= dev->num_sectors || blk_io_read(dev, ebr, 1, sector)) {
            kprintf("[mbr] EBR at sector %u unreadable -- chain cut\n", ebr);
            break;
        }
        if (*(u16 *)(sector + 510) != MBR_SIGNATURE) {
            kprintf("[mbr] EBR at sector %u: no signature -- chain cut\n", ebr);
            break;
        }
        /* no snprintf: "ebr<chain depth>" by hand */
        char idx[16];
        memcpy(idx, "ebr", 3);
        u32 d = depth;
        int li = 3;
        do {
            idx[li++] = (char)('0' + d % 10);
            d /= 10;
        } while (d);
        idx[li] = 0;

        struct mbr_entry *l = mbr_slot(sector, 0), *n = mbr_slot(sector, 1);
        if (l->type != PART_TYPE_NONE)
            mbr_take(dev, out, l->status, l->type, (u64)ebr + l->lba_first, l->sectors_count,
                     idx);
        if (n->type != PART_TYPE_NONE && !mbr_is_ext(n->type)) {
            /* some tools put stray data in the link slot: not a chain */
            kprintf("[mbr] EBR at sector %u: entry 1 is not a link -- chain cut\n", ebr);
            break;
        }
        u32 next = (n->type == PART_TYPE_NONE || !mbr_is_ext(n->type)) ? 0 : n->lba_first;
        if (!next) break;
        /* A link is relative, so it can only point forward; a "forward"
         * hop that leaves the disk is a hostile table, cut here. */
        if ((u64)ebr + next >= dev->num_sectors) {
            kprintf("[mbr] EBR link at sector %u leaves the disk -- chain cut\n", ebr);
            break;
        }
        ebr += next;
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
    int nlen = 0;
    const char *src = parent->name;
    while (*src && nlen < 12) pd->name[nlen++] = *src++;
    pd->name[nlen++] = '0' + index + 1;
    pd->name[nlen] = 0;

    out_dev->name = pd->name;
    out_dev->sector_size = parent->sector_size;
    out_dev->num_sectors = mbr->parts[index].sectors_count;
    out_dev->read = part_read;
    out_dev->write = part_write;
    out_dev->drv = pd;

    kprintf("[mbr] partition %d: start_lba=%u size_sectors=%u -> %s\n", index, pd->start_lba,
            pd->size_sectors, pd->name);

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
