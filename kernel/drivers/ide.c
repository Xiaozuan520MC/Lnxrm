/* ide.c: Simple PIO-mode IDE/ATA driver for QEMU's built-in PIIX4 IDE.
 * Reads/writes sectors from the primary IDE controller (0x1F0). */
#include <sys/vfs.h>
#include <io.h>
#include <console.h>
#include <mm/mm.h>
#include <sys/spinlock.h>

#define IDE_DATA     0x1F0
#define IDE_SECCOUNT 0x1F2
#define IDE_LBA_LO   0x1F3
#define IDE_LBA_MID  0x1F4
#define IDE_LBA_HI   0x1F5
#define IDE_DRIVE    0x1F6
#define IDE_STATUS   0x1F7
#define IDE_CMD      0x1F7

#define IDE_SR_BSY  0x80
#define IDE_SR_DRDY 0x40
#define IDE_SR_DRQ  0x08
#define IDE_SR_ERR  0x01

#define IDE_CMD_READ     0x20
#define IDE_CMD_WRITE    0x30
#define IDE_CMD_IDENTIFY 0xEC

static int ide_wait_ready(int mask, int timeout)
{
    for (int i = 0; i < timeout; i++) {
        u8 s = inb(IDE_STATUS);
        if (!(s & IDE_SR_BSY) && (s & mask)) return 0;
    }
    return LNXRM_EFAIL;
}

static int ide_wait_drq(void)
{
    for (int i = 0; i < 100000; i++) {
        u8 s = inb(IDE_STATUS);
        if (s & IDE_SR_ERR) return LNXRM_EFAIL;
        if (s & IDE_SR_DRQ) return 0;
    }
    return LNXRM_EFAIL;
}

static int ide_read_sector(u64 lba, void *buf)
{
    /* LBA28 tops out at 128 GiB (2^28 sectors).  Without this line the
     * (lba >> 24) & 0x0F below drops the top bits and answers from a
     * *different* sector instead of failing: wrong data, no error, nothing
     * in the log to trace it by (C53). */
    if (lba > 0x0FFFFFFFUL) return LNXRM_EFAIL;
    if (ide_wait_ready(IDE_SR_DRDY, 100000)) return LNXRM_EFAIL;

    outb(IDE_SECCOUNT, 1);
    outb(IDE_LBA_LO, (u8)(lba));
    outb(IDE_LBA_MID, (u8)(lba >> 8));
    outb(IDE_LBA_HI, (u8)(lba >> 16));
    outb(IDE_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    outb(IDE_CMD, IDE_CMD_READ);

    if (ide_wait_drq()) return LNXRM_EFAIL;

    u16 *p = (u16 *)buf;
    for (int i = 0; i < 256; i++) p[i] = inw(IDE_DATA);
    return 0;
}

static int ide_write_sector(u64 lba, const void *buf)
{
    /* Same ceiling as the read path: see ide_read_sector(). */
    if (lba > 0x0FFFFFFFUL) return LNXRM_EFAIL;
    if (ide_wait_ready(IDE_SR_DRDY, 100000)) return LNXRM_EFAIL;

    outb(IDE_SECCOUNT, 1);
    outb(IDE_LBA_LO, (u8)(lba));
    outb(IDE_LBA_MID, (u8)(lba >> 8));
    outb(IDE_LBA_HI, (u8)(lba >> 16));
    outb(IDE_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    outb(IDE_CMD, IDE_CMD_WRITE);

    if (ide_wait_drq()) return LNXRM_EFAIL;

    const u16 *p = (const u16 *)buf;
    for (int i = 0; i < 256; i++) outw(IDE_DATA, p[i]);

    /* flush */
    ide_wait_ready(IDE_SR_DRDY, 100000);
    return 0;
}

static struct blkdev ide_bd = {
    .name = "hda",
    .sector_size = 512,
    .read = NULL,
    .write = NULL,
};

/* Serialises the LBA-register setup + command + data-phase sequence so two
 * CPUs can't interleave and make task A's command use task B's LBA. */
static spinlock_t ide_lock = SPINLOCK_INIT;

static int ide_blk_read(struct blkdev *b, u64 lba, u32 count, void *buf)
{
    u8 *p = buf;
    u64 flags;
    spin_lock_irqsave(&ide_lock, &flags);
    for (u32 i = 0; i < count; i++) {
        if (lba + i >= b->num_sectors || ide_read_sector(lba + i, p + i * 512)) {
            spin_unlock_irqrestore(&ide_lock, flags);
            return LNXRM_EFAIL;
        }
    }
    spin_unlock_irqrestore(&ide_lock, flags);
    return 0;
}

static int ide_blk_write(struct blkdev *b, u64 lba, u32 count, const void *buf)
{
    const u8 *p = buf;
    u64 flags;
    spin_lock_irqsave(&ide_lock, &flags);
    for (u32 i = 0; i < count; i++) {
        if (lba + i >= b->num_sectors || ide_write_sector(lba + i, p + i * 512)) {
            spin_unlock_irqrestore(&ide_lock, flags);
            return LNXRM_EFAIL;
        }
    }
    spin_unlock_irqrestore(&ide_lock, flags);
    return 0;
}

void ide_init(void)
{
    /* probe: send IDENTIFY to primary channel */
    outb(IDE_DRIVE, 0xA0);
    /* ATA-2 §9.3: allow 400 ns after drive select before the command register
     * is written.  Four status reads are a conservative stand-in for that
     * delay.  QEMU answers instantly, so skipping this only ever hurt real
     * hardware (C54). */
    for (int i = 0; i < 4; i++) (void)inb(IDE_STATUS);

    u8 s = inb(IDE_STATUS);
    /* A port nothing answers on reads back 0xFF (open bus), not 0x00: the old
     * `s == 0` test only caught a controller that stayed silent. */
    if (s == 0x00 || s == 0xFF) {
        kprintf("[ide] no device on primary channel (status=%02x)\n", s);
        return;
    }

    if (ide_wait_ready(IDE_SR_DRDY, 100000)) {
        kprintf("[ide] primary channel not ready\n");
        return;
    }

    outb(IDE_CMD, IDE_CMD_IDENTIFY);

    /* Wait for DRQ *and* fail on ERR.  ide_wait_drq() is the only place in
     * this file that looks at IDE_SR_ERR, and the IDENTIFY path never called
     * it: an aborted command (ATAPI device, port with nothing on it) left the
     * 256-word read running against an idle data port, which answers 0xFFFF.
     * ident[60..61] then read as FFFFFFFFh, blk_register() published it, and
     * the console showed 2097151 -- that is where the number came from (C54). */
    if (ide_wait_drq()) {
        kprintf("[ide] IDENTIFY rejected (status=%02x)\n", inb(IDE_STATUS));
        return;
    }

    u16 ident[256];
    for (int i = 0; i < 256; i++) ident[i] = inw(IDE_DATA);

    /* capacity: words 60-61, with the 48-bit words 100-103 taking precedence */
    u64 sectors = ata_total_sectors(ident);
    char model[41];
    for (int i = 0; i < 20; i++) {
        model[i * 2] = (char)(ident[27 + i] >> 8);
        model[i * 2 + 1] = (char)(ident[27 + i] & 0xFF);
    }
    model[40] = 0;

    if (!sectors) {
        /* Registering a device whose size we could not read would hand every
         * downstream bounds check a lie, so decline instead. */
        kprintf("[ide] hda: %.40s, capacity unknown -- not registered\n", model);
        return;
    }

    ide_bd.read = ide_blk_read;
    ide_bd.write = ide_blk_write;
    ide_bd.num_sectors = sectors;
    blk_register(&ide_bd);

    kprintf("[ide] hda: %.40s, %llu sectors (%llu MiB)\n", model, sectors, sectors / 2048);
}
