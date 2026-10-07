/* ahci.cpp: Simplified AHCI/SATA driver — DMA transfers via a single-PRDT
 * command table, exposing the same blkdev interface as ide.c */
#include <pci.h>
#include <io.h>
#include <mm/mm.h>
#include <console.h>
#include <sys/vfs.h>
#include <sys/spinlock.h>

extern "C" void *memset(void *s, int c, unsigned long n);

/* AHCI generic registers */
static constexpr u32 AHCI_GHC = 0x04;
static constexpr u32 AHCI_PI = 0x0C;
static constexpr u32 GHC_ARE = 1u << 31;
static constexpr u32 GHC_HR = 1;

/* Port register offsets */
static constexpr u32 P_CLB = 0x00;
static constexpr u32 P_CLBU = 0x04;
static constexpr u32 P_FB = 0x08;
static constexpr u32 P_FBU = 0x0C;
static constexpr u32 P_IS = 0x10;
static constexpr u32 P_CMD = 0x18;
static constexpr u32 P_TFD = 0x20;
static constexpr u32 P_SSTS = 0x28;
static constexpr u32 P_CI = 0x38;

/* PORT_CMD bits */
static constexpr u32 PORT_CMD_ST = 1u << 0;  /* START */
static constexpr u32 PORT_CMD_FRE = 1u << 4; /* FIS receive enable */
static constexpr u32 PORT_CMD_FR = 1u << 14; /* FIS receive running */
static constexpr u32 PORT_CMD_CR = 1u << 15; /* command list running */

/* AHCI command header opts bits — from Linux ahci.h */
static constexpr u32 AHCI_CMD_CLR_BUSY = 1u << 10;
static constexpr u32 AHCI_CMD_WRITE = 1u << 6;
static constexpr u32 IS_TFES = 1u << 30;

/* Command header — 32 bytes, all u32 fields to avoid packing issues */
struct [[gnu::packed]] CmdHdr {
    u32 opts;    /* [4:0] FIS len, [6] write, [10] clr busy, [15:16] prdt_len */
    u32 prdbc;   /* physical byte count transferred */
    u32 ctba_lo; /* command table base addr low */
    u32 ctba_hi; /* command table base addr high */
    u32 _rsv[4];
};

struct [[gnu::packed]] PrdtEntry {
    u32 dba;
    u32 dbahi;
    u32 _rsv;
    u32 flags; /* bit31 = interrupt on completion */
};
static constexpr u32 PRDT_IOC = 1u << 31;

struct [[gnu::packed]] CmdTbl {
    u8 cfis[64];
    u8 acmd[16];
    u8 _rsv[48];
    PrdtEntry prdt[];
};

struct AhciPort {
    volatile u8 *mmio;
    CmdHdr *clb;
    void *fis;
    CmdTbl *ct;
};

static inline u32 pxRd(volatile u8 *px, u32 off)
{ return *(volatile u32 *)(px + off); }

static inline void pxWr(volatile u8 *px, u32 off, u32 v)
{ *(volatile u32 *)(px + off) = v; }

/* Wait until PxTFD clears BSY(7) and DRQ(3) */
static int waitTfd(volatile u8 *px, int timeout)
{
    for (int i = 0; i < timeout; i++) {
        if (!(pxRd(px, P_TFD) & 0x88)) return 0;
        asm volatile("pause");
    }
    return -1;
}

/* Start port DMA engine: set CMD.ST and CMD.FRE, wait for CMD.CR and CMD.FR */
static int portStart(volatile u8 *px)
{
    /* Clear error status */
    pxWr(px, 0x30, ~0u); /* PxSERR */
    pxWr(px, P_IS, ~0u); /* PxIS */

    /* Set CMD.FRE + CMD.ST */
    u32 cmd = pxRd(px, P_CMD);
    cmd |= PORT_CMD_FRE | PORT_CMD_ST;
    pxWr(px, P_CMD, cmd);

    /* Wait for CMD.CR and CMD.FR to set */
    for (int i = 0; i < 1000000; i++) {
        cmd = pxRd(px, P_CMD);
        if ((cmd & (PORT_CMD_CR | PORT_CMD_FR)) == (PORT_CMD_CR | PORT_CMD_FR)) return 0;
        asm volatile("pause");
    }
    return -1;
}

static int issueCmd(volatile u8 *px, int slot)
{
    /* Wait until the port's task-file data register is idle (DRQ/ERR clear)
     * before issuing, otherwise a busy/stuck device is commanded blindly. */
    if (waitTfd(px, 1000000)) return -1;

    /* Clear port error/status */
    pxWr(px, P_IS, ~0u);
    pxWr(px, 0x30, ~0u);

    /* Issue command */
    pxWr(px, P_CI, 1u << slot);

    /* Wait for command completion (CI bit clear) */
    for (int i = 0; i < 10000000; i++) {
        if (pxRd(px, P_CI) & (1u << slot)) {
            asm volatile("pause");
            continue;
        }
        /* C50: CI clear means the port finished, not that the drive succeeded.
         * IS_TFES only fires on a task-file *fatal* error; an ATA command that
         * was rejected sets PxTFD.ERR without it, and the caller then trusts
         * whatever was already in its buffer (IDENTIFY is prefilled 0xCC, so a
         * failure there used to be registered as a disk of 0xCCCCCCCC sectors). */
        if (pxRd(px, P_IS) & IS_TFES) return -1;
        if (pxRd(px, P_TFD) & 0x01) return -1; /* PxTFD.ERR */
        return 0;
    }
    return -1;
}

static void buildH2dFis(u8 *cfis, u8 cmd, u64 lba, u16 count)
{
    memset(cfis, 0, 64);
    cfis[0] = 0x27; /* host-to-device FIS */
    cfis[1] = 0x80; /* command bit */
    cfis[2] = cmd;
    cfis[7] = 0x40; /* device: LBA mode */
    cfis[4] = (u8)lba;
    cfis[5] = (u8)(lba >> 8);
    cfis[6] = (u8)(lba >> 16);
    cfis[8] = (u8)(lba >> 24);
    cfis[9] = (u8)(lba >> 32);
    cfis[10] = (u8)(lba >> 40);
    cfis[12] = (u8)count;
    cfis[13] = (u8)(count >> 8);
}

static int ahci_cmd_read(volatile u8 *px, AhciPort *port, u8 cmd, u64 lba, u32 count, void *buf,
                         u32 bytes)
{
    CmdHdr *h = &port->clb[0];
    CmdTbl *t = port->ct;

    memset(h, 0, sizeof(*h));
    memset(t, 0, sizeof(*t));

    /* opts: FIS_len=5, PRDT_len=1, CLR_BUSY(bit10) */
    h->opts = 5u | (1u << 16) | AHCI_CMD_CLR_BUSY;
    h->prdbc = 0;

    u64 bpa = VIRT_TO_PHYS((uptr)buf);
    t->prdt[0].dba = (u32)bpa;
    t->prdt[0].dbahi = (u32)(bpa >> 32);
    t->prdt[0].flags = (bytes - 1) | PRDT_IOC;

    buildH2dFis(t->cfis, cmd, lba, count);

    u64 ctPa = VIRT_TO_PHYS((uptr)t);
    h->ctba_lo = (u32)ctPa;
    h->ctba_hi = (u32)(ctPa >> 32);

    return issueCmd(px, 0);
}

static int ahci_cmd_write(volatile u8 *px, AhciPort *port, u8 cmd, u64 lba, u32 count,
                          const void *buf, u32 bytes)
{
    CmdHdr *h = &port->clb[0];
    CmdTbl *t = port->ct;

    memset(h, 0, sizeof(*h));
    memset(t, 0, sizeof(*t));

    /* opts: FIS_len=5, PRDT_len=1, WRITE(bit6), CLR_BUSY(bit10) */
    h->opts = 5u | AHCI_CMD_WRITE | (1u << 16) | AHCI_CMD_CLR_BUSY;
    h->prdbc = 0;

    u64 bpa = VIRT_TO_PHYS((uptr)buf);
    t->prdt[0].dba = (u32)bpa;
    t->prdt[0].dbahi = (u32)(bpa >> 32);
    t->prdt[0].flags = (bytes - 1) | PRDT_IOC;

    buildH2dFis(t->cfis, cmd, lba, count);

    u64 ctPa = VIRT_TO_PHYS((uptr)t);
    h->ctba_lo = (u32)ctPa;
    h->ctba_hi = (u32)(ctPa >> 32);

    return issueCmd(px, 0);
}

/* ---------- block device interface ---------- */

/* Serialises CLB/CT setup + command issue so two CPUs can't interleave. */
static spinlock_t ahci_lock = SPINLOCK_INIT;

static int ahciBlkRead(struct blkdev *bd, u64 lba, u32 cnt, void *buf)
{
    auto *p = (u8 *)buf;
    auto *port = (AhciPort *)bd->drv;
    u64 flags;
    spin_lock_irqsave(&ahci_lock, &flags);
    for (u32 i = 0; i < cnt; i++) {
        if (lba + i >= bd->num_sectors ||
            ahci_cmd_read(port->mmio, port, 0x25, lba + i, 1, p + i * 512, 512)) {
            spin_unlock_irqrestore(&ahci_lock, flags);
            return -1;
        }
    }
    spin_unlock_irqrestore(&ahci_lock, flags);
    return 0;
}

static int ahciBlkWrite(struct blkdev *bd, u64 lba, u32 cnt, const void *buf)
{
    auto *port = (AhciPort *)bd->drv;
    auto *p = (const u8 *)buf;
    u64 flags;
    spin_lock_irqsave(&ahci_lock, &flags);
    for (u32 i = 0; i < cnt; i++) {
        if (lba + i >= bd->num_sectors ||
            ahci_cmd_write(port->mmio, port, 0x35, lba + i, 1, p + i * 512, 512)) {
            spin_unlock_irqrestore(&ahci_lock, flags);
            return -1;
        }
    }
    u8 dummy[512];
    ahci_cmd_read(port->mmio, port, 0x25, lba, 1, dummy, 512);
    spin_unlock_irqrestore(&ahci_lock, flags);
    return 0;
}

/* ---------- PCI probe ---------- */

static constexpr u64 DEV_VMA_BASE = 0xffffffff90002000UL;

static int ahciProbe(void *, u8 bus, u8 dev, u8 fn, u16 vendor, u16 devid, u8 class_)
{
    if (class_ != 1) return 0;
    u32 sub = pci_read(bus, dev, fn, 0x08);
    if (((sub >> 16) & 0xFF) != 6) return 0;

    pci_enable_bm(bus, dev, fn);
    u32 bar5lo = pci_read(bus, dev, fn, 0x24);
    u32 bar5hi = pci_read(bus, dev, fn, 0x28);
    u64 abar = ((u64)bar5hi << 32) | (bar5lo & ~0xFUL);

    vmm_map_kernel_page(DEV_VMA_BASE, abar & ~4095UL, 0x03);
    volatile u8 *mmio = (volatile u8 *)(DEV_VMA_BASE + (abar & 4095));

    kprintf("[ahci] controller %04x:%04x at %02x:%02x.%d abar=%#llx\n", vendor, devid, bus, dev, fn,
            abar);

    /* Reset HBA if needed */
    u32 ghc = *(volatile u32 *)(mmio + AHCI_GHC);
    if (ghc & GHC_HR) {
        *(volatile u32 *)(mmio + AHCI_GHC) = GHC_HR;
        for (int i = 0; i < 1000000; i++) {
            if (!(*(volatile u32 *)(mmio + AHCI_GHC) & GHC_HR)) break;
            asm volatile("pause");
        }
    }

    /* Enable AHCI mode, disable global IRQ */
    *(volatile u32 *)(mmio + AHCI_GHC) = GHC_ARE;
    asm volatile("mfence");

    u32 pi = *(volatile u32 *)(mmio + AHCI_PI);
    if (!pi) {
        kprintf("[ahci] no ports\n");
        return 0;
    }

    for (int p = 0; p < 32; p++) {
        if (!((pi >> p) & 1)) continue;
        volatile u8 *px = mmio + 0x100 + p * 0x80;

        u32 ssts = pxRd(px, P_SSTS);
        if ((ssts & 0xF) != 3) continue;

        kprintf("[ahci] port %d: device present (SSTS=%08x)\n", p, ssts);

        /* Allocate the port's DMA structures before the port object: an
         * allocation failure must skip *this* port, not take the machine
         * with it (T-004 -- and AHCI is optional, IDE still boots). */
        CmdHdr *clb = (CmdHdr *)kmalloc(1024);
        void *fis = kmalloc(256);
        CmdTbl *ct = (CmdTbl *)kmalloc(4096);
        if (!clb || !fis || !ct) {
            kprintf("[ahci] port %d: out of memory, port skipped\n", p);
            kfree(clb);
            kfree(fis);
            kfree(ct);
            continue;
        }
        auto *port = new AhciPort;
        port->mmio = px;
        port->clb = clb;
        port->fis = fis;
        port->ct = ct;
        memset(port->clb, 0, 1024);
        memset(port->fis, 0, 256);
        memset(port->ct, 0, 4096);

        /* Stop port first */
        u32 cmd = pxRd(px, P_CMD);
        cmd &= ~(PORT_CMD_ST | PORT_CMD_FRE);
        pxWr(px, P_CMD, cmd);
        for (int i = 0; i < 500000; i++) {
            cmd = pxRd(px, P_CMD);
            if (!(cmd & (PORT_CMD_CR | PORT_CMD_FR))) break;
            asm volatile("pause");
        }

        /* Set CLB and FIS physical addresses */
        u64 clbPa = VIRT_TO_PHYS((uptr)port->clb);
        u64 fisPa = VIRT_TO_PHYS((uptr)port->fis);
        pxWr(px, P_CLB, (u32)clbPa);
        pxWr(px, P_CLBU, (u32)(clbPa >> 32));
        pxWr(px, P_FB, (u32)fisPa);
        pxWr(px, P_FBU, (u32)(fisPa >> 32));

        /* Start port */
        if (portStart(px)) {
            kprintf("[ahci] port %d: failed to start\n", p);
            continue;
        }

        /* IDENTIFY DEVICE */
        u16 ident[256];
        memset(ident, 0xCC, sizeof(ident));

        if (ahci_cmd_read(px, port, 0xEC, 0, 1, ident, 512)) {
            kprintf("[ahci] port %d: IDENTIFY failed tfd=%02x is=%08x\n", p, pxRd(px, P_TFD),
                    pxRd(px, P_IS));
            continue;
        }

        /* Capacity comes from words 60-61 (28-bit), with words 100-103 as the
         * 48-bit answer -- see ata_total_sectors().  The old code fell back to
         * words 100-103 only when words 60-61 were *zero*, but ATA's marker for
         * "use the 48-bit field" is FFFFFFFFh, so the one value that meant
         * 2097151 on the console was exactly the one that never fell back. */
        u64 sectors = ata_total_sectors(ident);

        char model[41];
        for (int i = 0; i < 20; i++) {
            model[i * 2] = (char)(ident[27 + i] >> 8);
            model[i * 2 + 1] = (char)(ident[27 + i] & 0xFF);
        }
        model[40] = 0;

        if (!sectors) {
            /* Do not register a device whose size we could not read: a made-up
             * capacity is worse than no disk, because every bounds check
             * downstream trusts it. */
            kprintf("[ahci] port %d: %.40s, capacity unknown -- port skipped\n", p, model);
            continue;
        }

        auto *bd = new blkdev;
        bd->name = "sda";
        bd->sector_size = 512;
        bd->num_sectors = sectors;
        bd->read = ahciBlkRead;
        bd->write = ahciBlkWrite;
        bd->drv = port;
        blk_register(bd);

        kprintf("[ahci] sda: %.40s, %llu sectors (%llu MiB)\n", model, sectors, sectors / 2048);
        return 1;
    }
    return 0;
}

extern "C" int ahci_probe_fn(void *ctx, u8 bus, u8 dev, u8 fn, u16 vendor, u16 devid, u8 class_)
{ return ahciProbe(ctx, bus, dev, fn, vendor, devid, class_); }

extern "C" void ahci_init(void)
{ pci_scan(ahci_probe_fn, nullptr); }
