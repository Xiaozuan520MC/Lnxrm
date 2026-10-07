/* PCI configuration-space enumeration (port IO, bus 0..255). */
#include <pci.h>
#include <io.h>
#include <console.h>

#define CFG_ADDR 0xCF8
#define CFG_DATA 0xCFC

u32 pci_read(u8 bus, u8 dev, u8 fn, u8 off)
{
    u32 addr = 0x80000000UL | ((u32)bus << 16) | ((u32)dev << 11) | ((u32)fn << 8) | (off & 0xFC);
    outl(CFG_ADDR, addr);
    return inl(CFG_DATA);
}

void pci_write(u8 bus, u8 dev, u8 fn, u8 off, u32 val)
{
    u32 addr = 0x80000000UL | ((u32)bus << 16) | ((u32)dev << 11) | ((u32)fn << 8) | (off & 0xFC);
    outl(CFG_ADDR, addr);
    outl(CFG_DATA, val);
}

static pci_match_fn matcher;
static void *match_ctx;

static void probe_bus(u8 bus);

/* PCI-to-PCI bridges are followed recursively (probe_fn below) with neither
 * a visited set nor a depth limit.  A root port whose secondary bus number
 * the firmware never assigned reads back 0, points straight at bus 0, and
 * the walk restarts forever -- every level silently, this whole path prints
 * nothing.  The boot stack is 16 KiB (arch/kernel.ld) with no guard page and
 * no IST, so it runs down through cpu_gdt_tab and idt in .bss and into the
 * kernel image itself; the next exception then lands on a trashed IDT and
 * the machine resets.  QEMU/VMware's flat i440FX topology never exercises
 * it, which is why it only shows up on real hardware. */
static u8 bus_seen[256 / 8];
#define PROBE_MAX_DEPTH 16
static int probe_depth;

static void probe_fn(u8 bus, u8 dev, u8 fn)
{
    u32 id = pci_read(bus, dev, fn, 0x00);
    if (id == 0xFFFFFFFF) return;
    u32 classrev = pci_read(bus, dev, fn, 0x08);
    u8 base_class = classrev >> 24;

    if (matcher && matcher(match_ctx, bus, dev, fn, id & 0xFFFF, id >> 16, base_class))
        return; /* claimed */

    if (base_class == 6 && ((classrev >> 16) & 0xFF) == 4)
        probe_bus(pci_read(bus, dev, fn, 0x18) >> 8); /* PCI-to-PCI bridge */
}

static void probe_dev(u8 bus, u8 dev)
{
    u32 id = pci_read(bus, dev, 0, 0x00);
    if (id == 0xFFFFFFFF) return;
    probe_fn(bus, dev, 0);
    if (!(pci_read(bus, dev, 0, 0x0C) & 0x800000)) return; /* no multi-function */
    for (u8 fn = 1; fn < 8; fn++) probe_fn(bus, dev, fn);
}

static void probe_bus(u8 bus)
{
    if (bus_seen[bus >> 3] & (1u << (bus & 7))) return; /* already walked */
    bus_seen[bus >> 3] |= 1u << (bus & 7);
    if (probe_depth >= PROBE_MAX_DEPTH) return;
    probe_depth++;
    for (u8 d = 0; d < 32; d++) probe_dev(bus, d);
    probe_depth--;
}

void pci_scan(pci_match_fn fn, void *ctx)
{
    matcher = fn;
    match_ctx = ctx;
    memset(bus_seen, 0, sizeof(bus_seen)); /* per scan, not per boot */
    probe_depth = 0;
    probe_bus(0);
}
