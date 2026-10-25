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

/* Sectors carried by ONE PIO command.  READ SECTORS/WRITE SECTORS take an
 * 8-bit count: 1..255 with 0 meaning 256.  Issuing a command per sector costs
 * a full command round trip -- several hundred microseconds in PIO -- for every
 * 512 bytes.  QEMU answers in nanoseconds, so every test passed while hiding
 * what it does on a real drive: fat_mount() loads the *whole* FAT up front,
 * and one-sector-at-a-time turns that into minutes of silence with irqs off,
 * which is indistinguishable from a hang.  Same fix as ahci.c. */
#define IDE_MAX_CHUNK 256
#define IDE_CMD_IDENTIFY 0xEC

/* Polling budgets, counted in port reads (there is no timer this early).
 *
 * IDE_PROBE_ITERS: the boot-time presence probe.  A port with no drive
 * answers 0x00/0xFF forever, so the probe must give up quickly or every
 * diskless boot stalls for seconds; a live drive answers immediately.
 *
 * IDE_POLL_ITERS: every wait on a drive we already know exists.  BSY may be
 * held as long as the medium takes to commit a sector, and QEMU's IDE runs
 * in the vCPU thread: a synchronous pwrite() into a host filesystem under
 * writeback pressure blocks the guest for tens of milliseconds, which the
 * old 100000-read budget (~10 ms at TCG port-I/O speed) turned into a
 * spurious EIO in the middle of a sustained fill -- the command was never
 * issued, so the sector was silently dropped and the caller saw a dead
 * journal.  The budget is still finite: a wedged controller gives up in a
 * couple of seconds of guest time instead of hanging the boot. (2026-10-21) */
#define IDE_PROBE_ITERS 100000
#define IDE_POLL_ITERS  20000000

static int ide_wait_ready(int mask, int timeout)
{
    for (int i = 0; i < timeout; i++) {
        u8 s = inb(IDE_STATUS);
        if (!(s & IDE_SR_BSY) && (s & mask)) return 0;
    }
    return LNXRM_EFAIL;
}

static int ide_wait_drq(int timeout)
{
    for (int i = 0; i < timeout; i++) {
        u8 s = inb(IDE_STATUS);
        if (s & IDE_SR_ERR) return LNXRM_EFAIL;
        if (s & IDE_SR_DRQ) return 0;
    }
    return LNXRM_EFAIL;
}

static int ide_read_sectors(u64 lba, u32 n, void *buf)
{
    /* LBA28 tops out at 128 GiB (2^28 sectors).  Without this line the
     * (lba >> 24) & 0x0F below drops the top bits and answers from a
     * *different* sector instead of failing: wrong data, no error, nothing
     * in the log to trace it by (C53). */
    if (lba > 0x0FFFFFFFUL) return LNXRM_EFAIL;
    if (!n || n > IDE_MAX_CHUNK) return LNXRM_EFAIL;
    if (ide_wait_ready(IDE_SR_DRDY, IDE_POLL_ITERS)) return LNXRM_EFAIL;

    outb(IDE_SECCOUNT, (u8)n); /* n == 256 encodes as 0, which ATA reads as 256 */
    outb(IDE_LBA_LO, (u8)(lba));
    outb(IDE_LBA_MID, (u8)(lba >> 8));
    outb(IDE_LBA_HI, (u8)(lba >> 16));
    outb(IDE_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    outb(IDE_CMD, IDE_CMD_READ);

    u16 *p = (u16 *)buf;
    for (u32 s = 0; s < n; s++) {
        /* One DRQ per sector, not one per command: the drive raises DRQ as
         * each sector becomes ready, so waiting only before the first would
         * race every sector after it. */
        if (ide_wait_drq(IDE_POLL_ITERS)) return LNXRM_EFAIL;
        for (int i = 0; i < 256; i++) p[i] = inw(IDE_DATA);
        p += 256;
    }
    return 0;
}

static int ide_write_sectors(u64 lba, u32 n, const void *buf)
{
    /* Same ceiling as the read path. */
    if (lba > 0x0FFFFFFFUL) return LNXRM_EFAIL;
    if (!n || n > IDE_MAX_CHUNK) return LNXRM_EFAIL;
    if (ide_wait_ready(IDE_SR_DRDY, IDE_POLL_ITERS)) return LNXRM_EFAIL;

    outb(IDE_SECCOUNT, (u8)n);
    outb(IDE_LBA_LO, (u8)(lba));
    outb(IDE_LBA_MID, (u8)(lba >> 8));
    outb(IDE_LBA_HI, (u8)(lba >> 16));
    outb(IDE_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    outb(IDE_CMD, IDE_CMD_WRITE);

    const u16 *p = (const u16 *)buf;
    for (u32 s = 0; s < n; s++) {
        /* The device takes the data one sector at a time and re-asserts DRQ
         * between them; writing the next 256 words without that wait would
         * overrun a device that is still flushing the previous one. */
        if (ide_wait_drq(IDE_POLL_ITERS)) return LNXRM_EFAIL;
        for (int i = 0; i < 256; i++) outw(IDE_DATA, p[i]);
        p += 256;
    }

    /* flush */
    ide_wait_ready(IDE_SR_DRDY, IDE_POLL_ITERS);
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
    /* Same bound the old per-sector loop enforced: a request reaching past the
     * end of the device fails as a whole rather than reading what it can. */
    if (count && lba + count > b->num_sectors) {
        spin_unlock_irqrestore(&ide_lock, flags);
        return LNXRM_EFAIL;
    }
    while (count) {
        u32 n = count > IDE_MAX_CHUNK ? IDE_MAX_CHUNK : count;
        if (ide_read_sectors(lba, n, p)) {
            spin_unlock_irqrestore(&ide_lock, flags);
            return LNXRM_EFAIL;
        }
        lba += n;
        p += n * 512;
        count -= n;
    }
    spin_unlock_irqrestore(&ide_lock, flags);
    return 0;
}

static int ide_blk_write(struct blkdev *b, u64 lba, u32 count, const void *buf)
{
    const u8 *p = buf;
    u64 flags;
    spin_lock_irqsave(&ide_lock, &flags);
    if (count && lba + count > b->num_sectors) {
        spin_unlock_irqrestore(&ide_lock, flags);
        return LNXRM_EFAIL;
    }
    while (count) {
        u32 n = count > IDE_MAX_CHUNK ? IDE_MAX_CHUNK : count;
        if (ide_write_sectors(lba, n, p)) {
            spin_unlock_irqrestore(&ide_lock, flags);
            return LNXRM_EFAIL;
        }
        lba += n;
        p += n * 512;
        count -= n;
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

    if (ide_wait_ready(IDE_SR_DRDY, IDE_PROBE_ITERS)) {
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
    if (ide_wait_drq(IDE_PROBE_ITERS)) {
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
