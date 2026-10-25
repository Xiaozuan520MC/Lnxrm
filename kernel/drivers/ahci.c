/* ahci.c: Simplified AHCI/SATA driver — DMA transfers via a single-PRDT
 * command table, exposing the same blkdev interface as ide.c
 *
 * Pure C (rewritten from ahci.cpp 2026-10-17): the driver never used any
 * C++ feature beyond constexpr / [[gnu::packed]] / new / a default argument,
 * all four of which have direct C equivalents (see the notes at each site).
 * Same register programming, same probe sequence, same timeouts. */
#include <pci.h>
#include <io.h>
#include <mm/mm.h>
#include <console.h>
#include <sys/vfs.h>
#include <sys/spinlock.h>
#include <sys/cpu.h>

/* AHCI generic registers */
#define AHCI_GHC 0x04u
#define AHCI_PI  0x0Cu
#define GHC_ARE  (1u << 31)
#define GHC_HR   1u

/* Port register offsets */
#define P_CLB  0x00u
#define P_CLBU 0x04u
#define P_FB   0x08u
#define P_FBU  0x0Cu
#define P_IS   0x10u
#define P_CMD  0x18u
#define P_TFD  0x20u
#define P_SSTS 0x28u
#define P_SERR 0x30u
#define P_SACT 0x34u
#define P_CI   0x38u

/* PORT_CMD bits */
#define PORT_CMD_ST  (1u << 0)  /* START */
#define PORT_CMD_FRE (1u << 4)  /* FIS receive enable */
#define PORT_CMD_FR  (1u << 14) /* FIS receive running */
#define PORT_CMD_CR  (1u << 15) /* command list running */

/* AHCI command header opts bits — from Linux ahci.h */
#define AHCI_CMD_CLR_BUSY (1u << 10)
#define AHCI_CMD_WRITE    (1u << 6)
#define IS_TFES           (1u << 30)

/* ---- timeouts ----
 *
 * These used to be counts of `pause` instructions, which is a unit with no
 * relationship to time: 1e6 pauses is 3 ms on a Core2 and 50 ms on a Skylake,
 * so the pre-issue task-file gate that was "generous" in the QEMU log was in
 * reality a few milliseconds on real silicon -- long enough for everything
 * QEMU ever did, far too short for a drive that has just come out of link
 * training and is still spinning up.  On the one machine this driver has been
 * run on, every wait was silently a 10-100x tighter budget than the comment
 * claimed.  Deadlines are in milliseconds of calibrated TSC time instead, so
 * the numbers mean the same thing everywhere. */
#define AHCI_LINK_TIMEOUT_MS 5000 /* PHY link training (DET==1) */
#define AHCI_STOP_TIMEOUT_MS 100  /* CR / FR drain after ST/FRE clear */
#define AHCI_TFD_TIMEOUT_MS  3000 /* device idle before a command */
#define AHCI_CMD_TIMEOUT_MS  5000 /* PxCI clear (command completion) */

/* TSC rate is calibrated by lapic_timer_calibrate(), which main.c calls only
 * *after* the disk is mounted -- and mdelay()'s own fallback would calibrate
 * it from the PIT on first use.  Do the same here, once: every deadline below
 * needs a nonzero rate before the first port is probed. */
static u64 tscRate(void)
{
    static u64 rate;
    if (!rate) {
        rate = tsc_per_ms();
        if (!rate) lapic_timer_calibrate();
        rate = tsc_per_ms();
        if (!rate) rate = 2000000ULL; /* last resort, cpu.c's own fallback */
    }
    return rate;
}

static u64 deadlineMs(int ms)
{
    return rdtsc() + tscRate() * (u64)ms;
}

static bool overdue(u64 deadline)
{
    return rdtsc() >= deadline;
}


/* Command header — 32 bytes, all u32 fields to avoid packing issues.
 * typedef + __attribute__((packed)) replaces C++'s struct [[gnu::packed]]
 * and keeps every use site spelling-free. */
typedef struct __attribute__((packed)) {
    u32 opts;    /* [4:0] FIS len, [6] write, [10] clr busy, [15:16] prdt_len */
    u32 prdbc;   /* physical byte count transferred */
    u32 ctba_lo; /* command table base addr low */
    u32 ctba_hi; /* command table base addr high */
    u32 _rsv[4];
} CmdHdr;

typedef struct __attribute__((packed)) {
    u32 dba;
    u32 dbahi;
    u32 _rsv;
    u32 flags; /* bit31 = interrupt on completion */
} PrdtEntry;
#define PRDT_IOC (1u << 31)

typedef struct __attribute__((packed)) {
    u8 cfis[64];
    u8 acmd[16];
    u8 _rsv[48];
    PrdtEntry prdt[];
} CmdTbl;

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

/* Wait until PxTFD clears BSY(7) and DRQ(3).  `timeout` is milliseconds. */
static int waitTfd(volatile u8 *px, int timeout)
{
    u64 dl = deadlineMs(timeout);
    for (;;) {
        if (!(pxRd(px, P_TFD) & 0x88)) return 0;
        if (overdue(dl)) break;
        __asm__ volatile("pause");
    }
    return -1;
}

/* Bring a port to a full stop in the order the specification gives: clear ST
 * and wait for CR to clear, then clear FRE and wait for FR.  The two engines
 * are stopped one at a time because they drain independently, and FRE must not
 * be cleared while CR is still set.
 *
 * Returns 0 only when both CR and FR are actually clear afterwards.  That
 * matters more than the ordering does: PxCLB/PxFB may be written exclusively
 * while the port is stopped, so a caller that cannot tell a real stop from a
 * timed-out one will hand its addresses to an engine that never took them. */
static int ahciPortStop(volatile u8 *px)
{
    u32 cmd = pxRd(px, P_CMD);

    if (cmd & PORT_CMD_ST) {
        pxWr(px, P_CMD, cmd & ~PORT_CMD_ST);
        u64 dl = deadlineMs(AHCI_STOP_TIMEOUT_MS);
        while (pxRd(px, P_CMD) & PORT_CMD_CR) {
            if (overdue(dl)) break;
            __asm__ volatile("pause");
        }
    }

    cmd = pxRd(px, P_CMD);
    if (cmd & PORT_CMD_FRE) {
        pxWr(px, P_CMD, cmd & ~PORT_CMD_FRE);
        u64 dl = deadlineMs(AHCI_STOP_TIMEOUT_MS);
        while (pxRd(px, P_CMD) & PORT_CMD_FR) {
            if (overdue(dl)) break;
            __asm__ volatile("pause");
        }
    }

    return (pxRd(px, P_CMD) & (PORT_CMD_CR | PORT_CMD_FR)) ? -1 : 0;
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
    u64 dl = deadlineMs(AHCI_STOP_TIMEOUT_MS);
    for (;;) {
        cmd = pxRd(px, P_CMD);
        if ((cmd & (PORT_CMD_CR | PORT_CMD_FR)) == (PORT_CMD_CR | PORT_CMD_FR)) return 0;
        if (overdue(dl)) break;
        __asm__ volatile("pause");
    }
    return -1;
}

/* A command that never completes, or one that completes without moving a byte,
 * is the failure this driver cannot report through a return value: the caller
 * just sees "read failed" -- or worse, "read succeeded" -- and "the disk will
 * not mount" is where that ends up.  Dump the port once so it is a diagnosis:
 * CI still set means the doorbell never reached the HBA, TFD.BSY set means the
 * drive never accepted the command, IS set means the HBA took it and rejected
 * it, SSTS says what the link looks like.  Printed at most once, because every
 * command would otherwise repeat the same registers. */
static void ahciDumpPort(volatile u8 *px, int slot, const char *why)
{
    static bool dumped;
    if (dumped) return;
    dumped = true;
    kprintf("[ahci] %s (slot %d): CMD=%08x IS=%08x TFD=%02x CI=%08x "
            "SACT=%08x SSTS=%08x SERR=%08x\n",
            why, slot, pxRd(px, P_CMD), pxRd(px, P_IS), pxRd(px, P_TFD), pxRd(px, P_CI),
            pxRd(px, P_SACT), pxRd(px, P_SSTS), pxRd(px, P_SERR));
}

static int issueCmd(volatile u8 *px, int slot)
{
    /* Wait until the port's task-file data register is idle (DRQ/ERR clear)
     * before issuing, otherwise a busy/stuck device is commanded blindly.
     * This is the gate a freshly linked drive sits behind while it spins up,
     * so it gets seconds, not the milliseconds the old `pause` budget gave. */
    if (waitTfd(px, AHCI_TFD_TIMEOUT_MS)) {
        ahciDumpPort(px, slot, "task file busy before issue");
        return -1;
    }

    /* Clear port error/status */
    pxWr(px, P_IS, ~0u);
    pxWr(px, P_SERR, ~0u);

    /* Issue command */
    pxWr(px, P_CI, 1u << slot);

    /* Wait for command completion (CI bit clear) */
    u64 dl = deadlineMs(AHCI_CMD_TIMEOUT_MS);
    for (;;) {
        if (pxRd(px, P_CI) & (1u << slot)) {
            if (overdue(dl)) break;
            __asm__ volatile("pause");
            continue;
        }
        /* C50: CI clear means the port finished, not that the drive succeeded.
         * IS_TFES only fires on a task-file *fatal* error; an ATA command that
         * was rejected sets PxTFD.ERR without it.  Neither of those catches the
         * third outcome -- port finished, zero bytes moved -- which is what
         * ahci_cmd_read()'s PRDBC check below is for. */
        if (pxRd(px, P_IS) & IS_TFES) return -1;
        if (pxRd(px, P_TFD) & 0x01) return -1; /* PxTFD.ERR */
        return 0;
    }
    ahciDumpPort(px, slot, "command never completed (PxCI stuck set)");
    return -1;
}

/* devReg is the ATA device/head register: 0x40 selects LBA addressing, and is
 * what every data command wants.  For IDENTIFY (0xEC) those bits are reserved
 * by the command, and some devices are picky about them -- so the port probe
 * passes whichever value it is testing rather than this being hard-coded. */
static void buildH2dFis(u8 *cfis, u8 cmd, u64 lba, u16 count, u8 devReg)
{
    memset(cfis, 0, 64);
    cfis[0] = 0x27; /* host-to-device FIS */
    cfis[1] = 0x80; /* command bit */
    cfis[2] = cmd;
    cfis[7] = devReg;
    cfis[4] = (u8)lba;
    cfis[5] = (u8)(lba >> 8);
    cfis[6] = (u8)(lba >> 16);
    cfis[8] = (u8)(lba >> 24);
    cfis[9] = (u8)(lba >> 32);
    cfis[10] = (u8)(lba >> 40);
    cfis[12] = (u8)count;
    cfis[13] = (u8)(count >> 8);
}

/* ---------- CPU <-> device cache coherence ----------
 *
 * The command list, FIS receive area, command table and every data buffer
 * are touched by whichever side is not the CPU, through a physical address
 * the other side owns.  lnxrm programs neither MTRRs nor IA32_PAT, so all of
 * it lives as ordinary write-back memory: a store by the CPU sits in its
 * cache while the HBA -- which sees memory, never the cache -- reads the old
 * bytes, and a store by the HBA is invisible to a CPU whose line is still
 * valid.  QEMU models no caches whatsoever, so every one of these failures is
 * invisible in the default `make run` path.
 *
 * clflush is a write-back *and* an invalidate, so one sweep handles both
 * directions: dirty lines reach memory before the device reads them, and the
 * device's writes become visible to the CPU afterwards.  Rounding to 64-byte
 * boundaries is mandatory -- starting at an unaligned pointer would skip the
 * bytes between it and the next line, so a mid-line buffer's first bytes
 * would never be flushed. */
static inline void dmaCoherent(const void *p, u32 bytes)
{
    if (!bytes) return;
    uptr a = (uptr)p & ~63UL;
    uptr e = ((uptr)p + bytes + 63UL) & ~63UL;
    for (uptr c = a; c < e; c += 64)
        __asm__ volatile("clflush (%0)" ::"r"(c) : "memory");
    /* mfence rather than sfence.  Whether sfence orders clflush is one of the
     * places the memory model has been tightened and re-tightened across
     * families, and getting it wrong leaves a dirty line to be written back
     * over the data the device just DMA'd -- a silent read of stale memory,
     * with no error bit anywhere to point at it.  This runs a handful of times
     * per command, so the stronger fence costs nothing worth measuring. */
    __asm__ volatile("mfence" ::: "memory");
}

/* AHCI 1.0 wants the command list 1 KiB-aligned, the FIS base 256-byte
 * aligned and each command table 128-byte aligned (§3.3).  kmalloc()
 * prefixes every block with an 8-byte header and returns base+8, so nothing
 * it hands out can satisfy even the loosest of those, and its slab size
 * classes are not multiples of 128 either.  QEMU never validates the
 * addresses it is given -- which is precisely why this went unnoticed --
 * while a real HBA masks PxCLB to its required alignment and then fetches
 * the command list from the wrong address.  A buddy page is aligned to its
 * own size, so a single order-0 frame satisfies all three rules at once. */
static void *dmaAlloc(void)
{
    u64 pa = pmm_alloc_order(0);
    return pa ? (void *)PHYS_TO_VIRT(pa) : NULL;
}

static void dmaFree(void *p)
{
    if (p) pmm_free_order(VIRT_TO_PHYS((uptr)p), 0);
}

/* The PRDT-length/dirty-line dump below is per-port diagnostics: re-armed by
 * the probe for every port so each one shows the addresses it handed the HBA,
 * not just whichever port happened to run first. */
static bool dumpedDma;

/* hdrExtra / devReg are the two knobs the port probe A/Bs when a transfer
 * comes back with PRDBC == 0 (see ahciProbePort).  Everything else -- the
 * single PRDT entry, the clflush discipline, the PRDBC check -- is common. */
static int ahci_cmd_read_opts(volatile u8 *px, struct AhciPort *port, u8 cmd, u64 lba, u32 count,
                              void *buf, u32 bytes, u32 hdrExtra, u8 devReg)
{
    CmdHdr *h = &port->clb[0];
    CmdTbl *t = port->ct;

    /* sizeof(*t) stops at the flexible array: it does NOT cover prdt[0], so
     * memset'ing only sizeof(*t) leaves the PRDT's reserved dword carrying
     * whatever a previous command (or a recycled frame) left there.  AHCI
     * wants reserved bits zero and some HBAs reject the entry outright. */
    memset(h, 0, sizeof(*h));
    memset(t, 0, sizeof(*t) + sizeof(PrdtEntry));

    /* opts: FIS_len=5, PRDT_len=1, plus whatever the caller is testing.
     * CLR_BUSY used to be OR'd in unconditionally here; it is now off by
     * default because it is the one header bit whose spec-defined action is
     * "on receipt of the PIO/DMA-in FIS, clear BSY and retire the command"--
     * exactly the step that answered "0 of 512 bytes" on real hardware. */
    h->opts = 5u | (1u << 16) | hdrExtra;
    h->prdbc = 0;

    u64 bpa = VIRT_TO_PHYS((uptr)buf);
    t->prdt[0].dba = (u32)bpa;
    t->prdt[0].dbahi = (u32)(bpa >> 32);
    t->prdt[0].flags = (bytes - 1) | PRDT_IOC;

    buildH2dFis(t->cfis, cmd, lba, count, devReg);

    u64 ctPa = VIRT_TO_PHYS((uptr)t);
    h->ctba_lo = (u32)ctPa;
    h->ctba_hi = (u32)(ctPa >> 32);

    /* Before: push our command structures out and drop any dirty line in the
     * destination buffer, or a line the CPU wrote earlier gets written back
     * *after* the device's data and silently overwrites it (that is exactly
     * what the 0xCC IDENTIFY prefill would have done). */
    dmaCoherent(buf, bytes);
    dmaCoherent(h, sizeof(*h));
    dmaCoherent(t, sizeof(*t) + sizeof(PrdtEntry));

    /* One-shot view of exactly what the HBA will fetch.  These reads happen
     * *after* the flush-and-invalidate, so the lines are cold and the CPU has
     * to pull them from memory -- the same memory the HBA's DMA reads.  A bug
     * here is invisible from the CPU side: the structures look perfect in
     * cache while the bus master is handed something else, and the only
     * symptom is "command completed, zero bytes moved". */
    if (!dumpedDma) {
        dumpedDma = true;
        kprintf("[ahci] dma: buf phys=%#llx ct phys=%#llx clb phys=%#llx\n", bpa, ctPa,
                VIRT_TO_PHYS((uptr)h));
        kprintf("[ahci] dma: hdr opts=%08x prdtl=%u ctba=%08x:%08x | "
                "prdt dba=%08x:%08x dbc=%u ioc=%u\n",
                h->opts, (h->opts >> 16) & 0xFFFF, h->ctba_hi, h->ctba_lo, t->prdt[0].dbahi,
                t->prdt[0].dba, (t->prdt[0].flags & 0x1FFFFF) + 1,
                (t->prdt[0].flags >> 31) & 1);
        /* Below 4 GiB is what pmm.c promises every allocation and what an HBA
         * without 64-bit addressing can reach.  A PRDT pointing past it does
         * not fault -- it transfers nothing, which is indistinguishable from a
         * hung disk until someone prints the address. */
        if (bpa >= 0x100000000ULL || ctPa >= 0x100000000ULL)
            kprintf("[ahci] dma: WARNING buffer above 4 GiB, this HBA may not reach it\n");
    }

    if (issueCmd(px, 0)) return -1;

    /* Re-read the header through the flush-and-invalidate path so the byte
     * count we look at is the one the HBA wrote, not our own zero from a line
     * still sitting in cache.  PRDBC is how the HBA reports what it actually
     * moved, and it is the only place the third failure mode shows up: the port
     * finished, no error was set, and not one byte arrived.  CI-clear-plus-
     * no-error is what the old code accepted unconditionally, which is how real
     * hardware registered an IDENTIFY that had delivered nothing at all.  A
     * count of 0 with a write pending also means the doorbell may simply never
     * have latched -- there is no way to tell those two apart after the fact,
     * and both mean the same thing downstream: the buffer still holds whatever
     * was already in it. */
    dmaCoherent(h, sizeof(*h));
    dmaCoherent(buf, bytes);
    if (h->prdbc != bytes) {
        kprintf("[ahci] read lba %llu: %u of %u bytes transferred by the HBA\n",
                (unsigned long long)lba, h->prdbc, bytes);
        ahciDumpPort(px, 0, "command completed without moving the data");
        return -1;
    }
    return 0;
}

static int ahci_cmd_read(volatile u8 *px, struct AhciPort *port, u8 cmd, u64 lba, u32 count, void *buf,
                         u32 bytes)
{
    /* The production path: no CLR_BUSY, LBA device register.  Identical to
     * what the port probe validates before this is ever reached. */
    return ahci_cmd_read_opts(px, port, cmd, lba, count, buf, bytes, 0, 0x40);
}

static int ahci_cmd_write(volatile u8 *px, struct AhciPort *port, u8 cmd, u64 lba, u32 count,
                          const void *buf, u32 bytes)
{
    CmdHdr *h = &port->clb[0];
    CmdTbl *t = port->ct;

    memset(h, 0, sizeof(*h));
    memset(t, 0, sizeof(*t) + sizeof(PrdtEntry));

    /* opts: FIS_len=5, PRDT_len=1, WRITE(bit6).  No CLR_BUSY -- see the read
     * path for why that bit is no longer set unconditionally. */
    h->opts = 5u | AHCI_CMD_WRITE | (1u << 16);
    h->prdbc = 0;

    u64 bpa = VIRT_TO_PHYS((uptr)buf);
    t->prdt[0].dba = (u32)bpa;
    t->prdt[0].dbahi = (u32)(bpa >> 32);
    t->prdt[0].flags = (bytes - 1) | PRDT_IOC;

    buildH2dFis(t->cfis, cmd, lba, count, 0x40);

    u64 ctPa = VIRT_TO_PHYS((uptr)t);
    h->ctba_lo = (u32)ctPa;
    h->ctba_hi = (u32)(ctPa >> 32);

    /* Everything the device is about to read must be in memory before the
     * doorbell rings: buf (the payload) and the two structures that tell it
     * where to look. */
    dmaCoherent(buf, bytes);
    dmaCoherent(h, sizeof(*h));
    dmaCoherent(t, sizeof(*t) + sizeof(PrdtEntry));

    if (issueCmd(px, 0)) return -1;

    /* Same PRDBC check as the read path: a write that "completed" without the
     * HBA counting any bytes never left this machine, so reporting success
     * would be a durability promise that was never kept. */
    dmaCoherent(h, sizeof(*h));
    if (h->prdbc != bytes) {
        kprintf("[ahci] write lba %llu: %u of %u bytes accepted by the HBA\n",
                (unsigned long long)lba, h->prdbc, bytes);
        ahciDumpPort(px, 0, "write completed without moving the data");
        return -1;
    }
    return 0;
}

/* ATA FLUSH CACHE (E7): push what the drive holds in its write cache onto
 * the medium.  This is what makes write() durable, and it replaces the old
 * "read the sector back into a dummy buffer" stand-in, which only proved the
 * data had reached the *drive's* cache -- and cost a full round trip per
 * write on top of proving nothing about the medium.  A non-data command, so
 * the header carries PRDT_len=0 and there is no PRDT entry to describe.
 *
 * It is also the cheapest probe of the HBA's *non-data* command path: no
 * PRDT, no PIO, no DMA -- if this completes with TFD clean, the doorbell and
 * the completion logic work, and whatever fails next is one layer deeper.
 * `hdrExtra` exists so the port probe can A/B CLR_BUSY on that path too.
 * (C has no default arguments — the .cpp original wrote = 0 here — so both
 * call sites pass the value explicitly.) */
static int ahciFlushCache(volatile u8 *px, struct AhciPort *port, u32 hdrExtra)
{
    CmdHdr *h = &port->clb[0];
    CmdTbl *t = port->ct;

    memset(h, 0, sizeof(*h));
    memset(t, 0, sizeof(*t));
    h->opts = 5u | hdrExtra;
    h->prdbc = 0;
    buildH2dFis(t->cfis, 0xE7, 0, 0, 0x40);

    u64 ctPa = VIRT_TO_PHYS((uptr)t);
    h->ctba_lo = (u32)ctPa;
    h->ctba_hi = (u32)(ctPa >> 32);

    dmaCoherent(h, sizeof(*h));
    dmaCoherent(t, sizeof(*t));

    /* FLUSH CACHE has no data phase.  The sectors were already accepted, so
     * a device that cannot honour 0xE7 must not turn a completed write into
     * a failed one -- but it is worth saying out loud, because "write()
     * returned 0" would otherwise be a durability promise we are not keeping. */
    int rc = issueCmd(px, 0);
    if (rc) kprintf("[ahci] FLUSH CACHE (E7) failed tfd=%02x is=%08x serr=%08x\n",
                    pxRd(px, P_TFD), pxRd(px, P_IS), pxRd(px, P_SERR));
    return rc;
}

/* ---------- port probe: three ATA protocols, tested separately ----------
 *
 * IDENTIFY DEVICE is a PIO data-in command: the device answers with a PIO
 * Setup FIS and the *HBA* is supposed to pull the 512 bytes out of the task
 * file and DMA them into our PRDT.  On the one real machine this driver has
 * run on, everything below that step had already proved itself -- PCI, ABAR,
 * port registers, link (SSTS=0x133), the doorbell (CI cleared), the device's
 * own answer (PxIS showed the PIO Setup FIS, TFD=0x50 = DRDY, no ERR) -- and
 * PRDBC still read 0.  A single "IDENTIFY failed" line cannot say which of
 * those layers broke, so probe them in order of increasing depth, each with
 * its own verdict line:
 *
 *   1/3 non-data  FLUSH CACHE 0xE7     doorbell + completion, no PRDT at all
 *   2/3 DMA       READ DMA EXT 0x25    the PRDT path every later read uses
 *   3/3 PIO       IDENTIFY 0xEC        the path IDENTIFY itself needs
 *
 * Steps 2 and 3 also A/B the two header choices this driver used to hard-code
 * -- CLR_BUSY and the LBA device register -- so one boot answers "does this
 * HBA need them?" instead of one boot per guess. */
struct CmdVariant {
    u32 extra; /* OR'd into the command header opts */
    u8 devReg; /* ATA device/head register in the CFIS */
    const char *name;
};

static int addVariant(struct CmdVariant *v, int n, u32 extra, u8 devReg, const char *name)
{
    for (int i = 0; i < n; i++)
        if (v[i].extra == extra && v[i].devReg == devReg) return n;
    v[n].extra = extra;
    v[n].devReg = devReg;
    v[n].name = name;
    return n + 1;
}

/* Returns 0 only when both the DMA and the PIO path moved a full 512 bytes,
 * in which case `ident` holds a real IDENTIFY block. */
static int ahciProbePort(volatile u8 *px, struct AhciPort *port, int pnum, u16 *ident)
{
    /* Re-arm the address dump: what matters now is the buffers THIS port is
     * about to hand the HBA, not the ones whichever port ran first used. */
    dumpedDma = false;

    /* --- 1/3 non-data --- */
    int nd = ahciFlushCache(px, port, 0);
    kprintf("[ahci] port %d: probe 1/3 non-data 0xE7 -> %s (tfd=%02x is=%08x serr=%08x)\n",
            pnum, nd ? "FAIL" : "ok", pxRd(px, P_TFD), pxRd(px, P_IS), pxRd(px, P_SERR));

    /* --- 2/3 DMA data-in: this is the path every filesystem read takes, so
     * whichever variant works here is the one the driver keeps. --- */
    int dm = -1;
    u32 bestExtra = 0;
    for (int v = 0; v < 2; v++) {
        u32 extra = v ? AHCI_CMD_CLR_BUSY : 0;
        memset(ident, 0xCC, 512);
        int rc = ahci_cmd_read_opts(px, port, 0x25, 0, 1, ident, 512, extra, 0x40);
        kprintf("[ahci] port %d: probe 2/3 DMA 0x25 [%s] -> %s "
                "(tfd=%02x is=%08x serr=%08x)\n",
                pnum, v ? "clr_busy" : "plain", rc ? "FAIL" : "ok", pxRd(px, P_TFD),
                pxRd(px, P_IS), pxRd(px, P_SERR));
        if (!rc) {
            dm = 0;
            bestExtra = extra;
            break;
        }
    }

    /* --- 3/3 PIO data-in: start from whatever the DMA step liked, then cover
     * the combinations this driver has shipped. --- */
    struct CmdVariant var[4];
    int nv = 0;
    nv = addVariant(var, nv, bestExtra, 0x00, "dev=00");
    nv = addVariant(var, nv, bestExtra, 0x40, "dev=40");
    nv = addVariant(var, nv, 0, 0x00, "plain");
    nv = addVariant(var, nv, AHCI_CMD_CLR_BUSY, 0x40, "clr_busy+dev=40");

    int pio = -1;
    for (int i = 0; i < nv && pio; i++) {
        memset(ident, 0xCC, 512);
        pio = ahci_cmd_read_opts(px, port, 0xEC, 0, 1, ident, 512, var[i].extra, var[i].devReg);
        kprintf("[ahci] port %d: probe 3/3 PIO 0xEC [%s] -> %s "
                "(tfd=%02x is=%08x serr=%08x)\n",
                pnum, var[i].name, pio ? "FAIL" : "ok", pxRd(px, P_TFD), pxRd(px, P_IS),
                pxRd(px, P_SERR));
    }

    /* One line that says which engine is broken.  A port with a working PIO
     * path but no DMA path can be IDENTIFY'd and never read; a port with DMA
     * but no PIO can be read but never identified -- neither is mountable, and
     * the two used to be indistinguishable from "no disk". */
    kprintf("[ahci] port %d: protocol verdict: non-data=%s DMA=%s PIO=%s\n", pnum,
            nd ? "FAIL" : "ok", dm ? "FAIL" : "ok", pio ? "FAIL" : "ok");

    return (dm || pio) ? -1 : 0;
}

/* ---------- block device interface ---------- */

/* Serialises CLB/CT setup + command issue so two CPUs can't interleave. */
static spinlock_t ahci_lock = SPINLOCK_INIT;

/* Sectors carried by ONE AHCI command.  A single PRDT entry can describe up to
 * 4 MiB and the ATA count field holds 65535, so 256 (128 KiB -- the usual
 * request size) sits far inside both limits; the constraint here is
 * throughput, not width.
 *
 * This driver used to issue one command per sector.  QEMU answers each of those
 * in microseconds, so every test passed while hiding what it costs on real
 * hardware: each command is a full non-NCQ round trip, 200 us-1 ms on a
 * physical drive.  fat_mount() reads the *entire* FAT into RAM up front
 * (fat32.c), which is a few hundred kilobytes on the 64 MiB test image but
 * hundreds of megabytes on a normal volume -- so the log stops at "trying
 * FAT32" and stays there for minutes with irqs held off.  That is what * "stuck
 * at mounting the disk" looks like: not a failure, just one-sector-at-a-time
 * I/O multiplied by a filesystem-sized number of sectors. */
#define AHCI_MAX_CHUNK 256

static int ahciBlkRead(struct blkdev *bd, u64 lba, u32 cnt, void *buf)
{
    u8 *p = (u8 *)buf;
    struct AhciPort *port = (struct AhciPort *)bd->drv;
    u64 flags;
    spin_lock_irqsave(&ahci_lock, &flags);
    /* Same bound the old per-sector loop enforced: a request reaching past the
     * end of the device fails as a whole rather than reading what it can. */
    if (cnt && lba + cnt > bd->num_sectors) {
        spin_unlock_irqrestore(&ahci_lock, flags);
        return -1;
    }
    while (cnt) {
        u32 n = cnt > AHCI_MAX_CHUNK ? AHCI_MAX_CHUNK : cnt;
        if (ahci_cmd_read(port->mmio, port, 0x25, lba, n, p, n * 512)) {
            spin_unlock_irqrestore(&ahci_lock, flags);
            return -1;
        }
        lba += n;
        p += (uptr)n * 512;
        cnt -= n;
    }
    spin_unlock_irqrestore(&ahci_lock, flags);
    return 0;
}

static int ahciBlkWrite(struct blkdev *bd, u64 lba, u32 cnt, const void *buf)
{
    struct AhciPort *port = (struct AhciPort *)bd->drv;
    const u8 *p = (const u8 *)buf;
    u64 flags;
    spin_lock_irqsave(&ahci_lock, &flags);
    if (cnt && lba + cnt > bd->num_sectors) {
        spin_unlock_irqrestore(&ahci_lock, flags);
        return -1;
    }
    while (cnt) {
        u32 n = cnt > AHCI_MAX_CHUNK ? AHCI_MAX_CHUNK : cnt;
        if (ahci_cmd_write(port->mmio, port, 0x35, lba, n, p, n * 512)) {
            spin_unlock_irqrestore(&ahci_lock, flags);
            return -1;
        }
        lba += n;
        p += (uptr)n * 512;
        cnt -= n;
    }
    /* The sectors reached the drive; now reach the medium. */
    (void)ahciFlushCache(port->mmio, port, 0);
    spin_unlock_irqrestore(&ahci_lock, flags);
    return 0;
}

/* ---------- PCI probe ---------- */

#define DEV_VMA_BASE 0xffffffff90002000UL

static int ahciProbe(void *, u8 bus, u8 dev, u8 fn, u16 vendor, u16 devid, u8 class_)
{
    if (class_ != 1) return 0;
    u32 sub = pci_read(bus, dev, fn, 0x08);
    if (((sub >> 16) & 0xFF) != 6) return 0;

    pci_enable_bm(bus, dev, fn);
    u32 bar5lo = pci_read(bus, dev, fn, 0x24);
    u32 bar5hi = pci_read(bus, dev, fn, 0x28);
    u64 abar = ((u64)bar5hi << 32) | (bar5lo & ~0xFUL);

    /* ABAR's register set runs 0x0000..0x10FF: the 0x100-byte generic header
     * followed by one 0x80-byte block per implemented port, up to 32 ports.
     * That is 0x1100 bytes -- past the first page, so mapping a single 4 KiB
     * page left every port register beyond offset 0xF00 reading whatever the
     * next page table entry happened to hold.  Also: PG_PCD alone selects
     * PAT[2] = UC-, which inherits the firmware's MTRRs (see mm.h); a
     * doorbell write that lands in the cache instead of on the bus is the
     * classic way an HBA hangs, so ask for UC outright. */
    u64 abar0 = abar & ~4095UL;
    vmm_map_kernel_page(DEV_VMA_BASE, abar0, PG_PCD | PG_PWT);
    vmm_map_kernel_page(DEV_VMA_BASE + PAGE_SIZE, abar0 + PAGE_SIZE, PG_PCD | PG_PWT);
    volatile u8 *mmio = (volatile u8 *)(DEV_VMA_BASE + (abar & 4095));

    kprintf("[ahci] controller %04x:%04x at %02x:%02x.%d abar=%#llx\n", vendor, devid, bus, dev, fn,
            abar);

    /* Reset HBA if needed */
    u32 ghc = *(volatile u32 *)(mmio + AHCI_GHC);
    if (ghc & GHC_HR) {
        *(volatile u32 *)(mmio + AHCI_GHC) = GHC_HR;
        for (int i = 0; i < 1000000; i++) {
            if (!(*(volatile u32 *)(mmio + AHCI_GHC) & GHC_HR)) break;
            __asm__ volatile("pause");
        }
    }

    /* Enable AHCI mode, disable global IRQ */
    *(volatile u32 *)(mmio + AHCI_GHC) = GHC_ARE;
    __asm__ volatile("mfence");

    u32 pi = *(volatile u32 *)(mmio + AHCI_PI);
    if (!pi) {
        kprintf("[ahci] no ports\n");
        return 0;
    }

    for (int p = 0; p < 32; p++) {
        if (!((pi >> p) & 1)) continue;
        volatile u8 *px = mmio + 0x100 + p * 0x80;

        /* Wait for the link instead of sampling it once.  SATA link training
         * is not instantaneous: PxSSTS.DET reads 1 (device detected, PHY not
         * ready) while the device negotiates, and only becomes 3 once
         * communication is established.  QEMU reports 3 immediately, so the
         * old single sample passed every test while being able to silently
         * drop a real disk that was still coming up -- and the symptom is
         * indistinguishable from "no disk": nothing else probes this port
         * and vfs gives up.  Only DET==1 is waited on; 0 (nothing attached)
         * and 4 (PHY failure) are settled answers, so the empty ports that
         * make up most of the 32 cost nothing. */
        u32 ssts = pxRd(px, P_SSTS);
        if ((ssts & 0xF) == 1) {
            u64 dl = deadlineMs(AHCI_LINK_TIMEOUT_MS);
            for (;;) {
                ssts = pxRd(px, P_SSTS);
                if ((ssts & 0xF) != 1) break;
                if (overdue(dl)) break;
                __asm__ volatile("pause");
            }
        }
        if ((ssts & 0xF) != 3) {
            /* Say it for EVERY unattached port, not just DET==1.  DET==0
             * (link down -- needs a COMRESET this driver does not send) and
             * DET==4 (PHY failure) used to be skipped in total silence, so on
             * the real machine that first failed here "no disk" and "the
             * disk's port never came up" produced byte-identical logs. */
            kprintf("[ahci] port %d: no device (SSTS=%08x DET=%u)%s\n", p, ssts, ssts & 0xF,
                    (ssts & 0xF) == 1 ? " -- link never established" : "");
            continue;
        }

        kprintf("[ahci] port %d: device present (SSTS=%08x)\n", p, ssts);

        /* Allocate the port's DMA structures before the port object: an
         * allocation failure must skip *this* port, not take the machine
         * with it (T-004 -- and AHCI is optional, IDE still boots).  These
         * are DMA buffers, so they come from the buddy, not kmalloc(): the
         * HBA's alignment rules (1 KiB / 256 B / 128 B) cannot be met by a
         * block that starts 8 bytes in. */
        CmdHdr *clb = (CmdHdr *)dmaAlloc();
        void *fis = dmaAlloc();
        CmdTbl *ct = (CmdTbl *)dmaAlloc();
        if (!clb || !fis || !ct) {
            kprintf("[ahci] port %d: out of memory, port skipped\n", p);
            dmaFree(clb);
            dmaFree(fis);
            dmaFree(ct);
            continue;
        }
        /* The .cpp original used `new` here, which routed to kmalloc_or_panic
         * -- i.e. it panicked on OOM, in a function whose own comment two
         * paragraphs up says an allocation failure must skip *this* port and
         * not take the machine with it (T-004).  kmalloc() + a NULL check
         * makes the code do what that comment promises; the .cpp had the
         * principle and the panic side by side. */
        struct AhciPort *port = (struct AhciPort *)kmalloc(sizeof(*port));
        if (!port) {
            kprintf("[ahci] port %d: out of memory for port object, port skipped\n", p);
            dmaFree(clb);
            dmaFree(fis);
            dmaFree(ct);
            continue;
        }
        port->mmio = px;
        port->clb = clb;
        port->fis = fis;
        port->ct = ct;
        memset(port->clb, 0, 1024);
        memset(port->fis, 0, 256);
        memset(port->ct, 0, 4096);
        /* these are the frames the HBA will fetch from -- make sure nothing
         * of ours is still sitting in a cache line when the doorbell rings */
        dmaCoherent(port->clb, 1024);
        dmaCoherent(port->fis, 256);
        dmaCoherent(port->ct, 4096);

        /* Stop the port before its command-list addresses change.  AHCI
         * sequences this -- ST first, wait for CR, then FRE, wait for FR --
         * because PxCLB/PxFB may only be written while the port is stopped.
         * Clearing both bits at once and then ignoring the result (what this
         * driver did) means a port whose FRE write has not taken yet gets its
         * addresses changed underneath a running engine.  QEMU stores the CLB
         * write unconditionally and clears CR/FR the instant the bits go, so
         * the sloppy order is invisible there.  On a real HBA the write is
         * dropped, the port keeps fetching command headers from the address
         * the firmware left behind, and every doorbell is answered "command
         * complete" with no data moved -- which is exactly how an 8 GB disk
         * came up registered as 0xCCCCCCCCCCCCCCCC sectors. */
        if (ahciPortStop(px)) {
            kprintf("[ahci] port %d: engine would not stop (CMD=%08x), port skipped\n", p,
                    pxRd(px, P_CMD));
            continue;
        }

        /* PxSCTL may still hold whatever the firmware or an Option ROM left
         * in it -- including a forced DET value, which would pin the PHY in a
         * state the link wait above can neither clear nor explain.  Only the
         * DET sub-field is a "write to change" control, and 0 means "no
         * forced state", so this is safe while the port is stopped. */
        pxWr(px, 0x2C /* PxSCTL */, 0);

        /* Set CLB and FIS physical addresses */
        u64 clbPa = VIRT_TO_PHYS((uptr)port->clb);
        u64 fisPa = VIRT_TO_PHYS((uptr)port->fis);
        pxWr(px, P_CLB, (u32)clbPa);
        pxWr(px, P_CLBU, (u32)(clbPa >> 32));
        pxWr(px, P_FB, (u32)fisPa);
        pxWr(px, P_FBU, (u32)(fisPa >> 32));

        /* Read the addresses back.  A write the HBA refused lands nowhere and
         * reports nothing -- the doorbell still rings and the port still
         * answers, against someone else's command list.  This is the one
         * place where "did the setup actually happen" is knowable before
         * trusting every later command to it. */
        if (pxRd(px, P_CLB) != (u32)clbPa || pxRd(px, P_FB) != (u32)fisPa) {
            kprintf("[ahci] port %d: PxCLB/PxFB readback mismatch "
                    "(got %08x/%08x want %08x/%08x), port skipped\n",
                    p, pxRd(px, P_CLB), pxRd(px, P_FB), (u32)clbPa, (u32)fisPa);
            continue;
        }

        /* Start port */
        if (portStart(px)) {
            kprintf("[ahci] port %d: failed to start\n", p);
            continue;
        }

        /* IDENTIFY DEVICE -- reached only through ahciProbePort(), which has
         * already proven the DMA and PIO paths with a real 512-byte transfer.
         * A DMA allocation rather than a stack array: the destination address
         * is the one value this driver must get right by construction, not by
         * luck of where the stack happened to land, and pmm is the allocator
         * that can promise where the physical page is. */
        u16 *ident = (u16 *)dmaAlloc();
        if (!ident) {
            kprintf("[ahci] port %d: no DMA memory for IDENTIFY\n", p);
            continue;
        }
        memset(ident, 0xCC, 512);

        if (ahciProbePort(px, port, p, ident)) {
            kprintf("[ahci] port %d: no usable ATA protocol, port skipped\n", p);
            dmaFree(ident);
            continue;
        }

        /* The port reported success and the buffer is still the0xCC we seeded
         * it with, byte for byte -- nothing was ever written to it.  This is
         * not hypothetical: it is exactly what real hardware showed, an 8 GB
         * disk registered as 14757395258967641292 sectors (0xCCCCCCCCCCCCCCCC,
         * the prefill pattern) with an empty model, because CI cleared without
         * a single byte of IDENTIFY data arriving.  Refuse it here rather than
         * hand a fabricated capacity to every bounds check downstream that
         * trusts it -- and say so, since "the disk did not mount" is otherwise
         * all the evidence there is. */
        bool untouched = true;
        for (u32 i = 0; i < 512; i++)
            if (((const u8 *)ident)[i] != 0xCC) {
                untouched = false;
                break;
            }
        if (untouched) {
            kprintf("[ahci] port %d: IDENTIFY completed but transferred no data\n", p);
            ahciDumpPort(px, 0, "IDENTIFY returned nothing");
            dmaFree(ident);
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
            dmaFree(ident);
            continue;
        }

        /* Same T-004 call as the port object: OOM here must not panic a
         * machine whose disk is otherwise fine -- reclaim this port
         * completely and register nothing (nothing leaks, nothing panics). */
        struct blkdev *bd = (struct blkdev *)kmalloc(sizeof(*bd));
        if (!bd) {
            kprintf("[ahci] port %d: out of memory for blkdev, port skipped\n", p);
            dmaFree(ident);
            dmaFree(port->clb);
            dmaFree(port->fis);
            dmaFree(port->ct);
            kfree(port);
            return 0;
        }
        bd->name = "sda";
        bd->sector_size = 512;
        bd->num_sectors = sectors;
        bd->read = ahciBlkRead;
        bd->write = ahciBlkWrite;
        bd->drv = port;
        blk_register(bd);

        kprintf("[ahci] sda: %.40s, %llu sectors (%llu MiB)\n", model, sectors, sectors / 2048);
        dmaFree(ident);
        return 1;
    }
    return 0;
}

void ahci_init(void)
{
    /* Every wait below is a TSC deadline, and tsc_per_ms() is still 0 here:
     * main.c does not calibrate it until after the disk is mounted.  Arm it
     * from the PIT now rather than letting the first deadline divide by it. */
    if (!tsc_per_ms()) lapic_timer_calibrate();
    pci_scan(ahciProbe, NULL);
}
