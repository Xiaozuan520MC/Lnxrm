/* Local APIC + IOAPIC driver for SMP.
 *
 * Two register-access paths, chosen per core in apic_enable():
 *   - xAPIC  (MMIO): the historical one — the LAPIC page is mapped into
 *     the DEV_VMA window and registers are u32 accesses at reg offsets.
 *   - x2APIC (MSRs): used when IA32_APIC_BASE.EXTD is set — by firmware
 *     (native x2APIC machines: most hardware from the last decade) or by
 *     apic_enable() itself when the CPU advertises the feature.  In this
 *     mode the MMIO window is DEAD (accesses are discarded/return all
 *     ones): a driver that only knows MMIO sends its INIT/SIPI into the
 *     void on such a machine and the APs never start — exactly how this
 *     kernel used to come up single-core on real hardware.  Registers are
 *     reached at MSR 0x800 + (reg >> 4); the ICR is one 64-bit write
 *     (MSR 0x831, the xAPIC ICR_HIGH, is reserved and would #GP). */
#include <sys/apic.h>
#include <sys/acpi.h>
#include <sys/cpu.h>
#include <io.h>
#include <mm/mm.h>
#include <console.h>

/* LAPIC mapped at DEV_VMA + 0, IOAPIC at DEV_VMA + 0x1000.
 * Each is a single 4 KiB MMIO page (xAPIC mode only). */
#define LAPIC_VA  DEV_VMA            /* 0xFFFFFFFF90000000 */
#define IOAPIC_VA (DEV_VMA + 0x1000) /* 0xFFFFFFFF90001000 */

/* x2APIC register MSRs: 0x800 + (reg >> 4).  e.g. EOI 0xB0 -> MSR 0x80B,
 * ICR low 0x300 -> MSR 0x830, timer CCR 0x390 -> MSR 0x839. */
#define MSR_X2APIC 0x800UL

volatile u32 *lapic_base;

/* The LAPIC access mode of the RUNNING core.  Every core re-derives it in
 * apic_enable() from its own MSR — it is never inherited across cores,
 * because #GP'ing an MSR 0x800+ access on a core still in xAPIC mode is
 * exactly the bug this flag exists to prevent. */
static bool s_x2apic;

/* ---- helpers ---- */

void apic_write(u32 reg, u32 val)
{
    if (s_x2apic) { wrmsr(MSR_X2APIC + (reg >> 4), val); return; }
    lapic_base[reg / 4] = val;
}

u32 apic_read(u32 reg)
{
    if (s_x2apic) return (u32)rdmsr(MSR_X2APIC + (reg >> 4));
    return lapic_base[reg / 4];
}

void apic_eoi(void)
{ apic_write(LAPIC_EOI, 0); }

/* Settle this core's access mode (MSR only — no register writes, so it is
 * safe to call before the MMIO window exists).  Idempotent. */
static void apic_mode_settle(void)
{
    u64 base = rdmsr(MSR_IA32_APIC_BASE);

    /* x2APIC switch, per core:
     * firmware-native x2APIC arrives with EXTD (bit 10) already set; on a
     * machine firmware left in xAPIC mode whose CPU advertises x2APIC
     * (CPUID.1:ECX[21]), set EXTD now — the SDM allows it while EN=1 and
     * the APIC's register state carries over (Linux does the same in
     * early_enable_x2apic()).  ap_main() runs this on every AP, so all
     * cores end up in the mode they individually read back. */
    if (!(base & (1UL << 10)) && cpu_has_x2apic())
        base |= (1UL << 10);
    base |= (1UL << 11); /* APIC Global Enable */
    wrmsr(MSR_IA32_APIC_BASE, base);

    s_x2apic = (base & (1UL << 10)) != 0;
}

void apic_enable(void)
{
    apic_mode_settle(); /* must precede the register write below */
    apic_write(LAPIC_SPURIOUS, 0x100 | 0xFF); /* enable + spurious vector 0xFF */
}

u32 apic_get_id(void)
{
    /* x2APIC: the whole 32-bit MSR is the ID.  xAPIC: the ID lives in
     * bits 31:24 of the MMIO register. */
    if (s_x2apic) return (u32)rdmsr(MSR_X2APIC + (LAPIC_ID >> 4));
    return apic_read(LAPIC_ID) >> 24;
}

void apic_init(void)
{
    /* read MMIO base from MSR (kept for the log line even in x2APIC
     * mode, where the window is not mapped at all) */
    u64 base = rdmsr(MSR_IA32_APIC_BASE);
    u64 phys = base & 0xFFFFFF000UL;

    /* Decide/switch the access mode BEFORE the first apic_write: in
     * x2APIC mode the MMIO window must not be touched at all. */
    apic_mode_settle();

    if (!s_x2apic) {
        /* map the LAPIC page into DEV_VMA */
        vmm_map_kernel_page(LAPIC_VA, phys, PG_PCD);
        lapic_base = (volatile u32 *)LAPIC_VA;
    }

    /* disable PIC interrupts (mask all) */
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);

    /* enable LAPIC (writes SPURIOUS through the path chosen above) */
    apic_enable();

    /* set task priority to 0 (accept all interrupts) */
    apic_write(LAPIC_TPR, 0);

    /* LVT: mask all except LINT0/LINT1 for now */
    apic_write(LAPIC_LVT_TIMER, 0x10000); /* masked */
    apic_write(LAPIC_LVT_THERMAL, 0x10000);
    apic_write(LAPIC_LVT_PMC, 0x10000);
    apic_write(LAPIC_LVT_LINT0, 0x10000);
    apic_write(LAPIC_LVT_LINT1, 0x10000);
    apic_write(LAPIC_LVT_ERROR, 0x10000);

    kprintf("[apic] %s base=%p ID=%u\n", s_x2apic ? "x2APIC MSR" : "MMIO",
            (void *)phys, apic_get_id());
}

/* ---- IPI ---- */

/* One ICR write in the mode of the running core.
 * `low` carries the xAPIC ICR-LOW layout (bits 31:0): vector, delivery
 * mode, level, trigger.  x2APIC keeps that layout in bits 31:0 of MSR
 * 0x830 with the destination in bits 63:32 (full 32-bit APIC ID, no
 * ICR_HIGH handshake at all). */
static void icr_send(u32 dest_apic_id, u32 low)
{
    if (s_x2apic) {
        while (rdmsr(MSR_X2APIC + (LAPIC_ICR_LOW >> 4)) & (1u << 12));
        wrmsr(MSR_X2APIC + (LAPIC_ICR_LOW >> 4),
              ((u64)dest_apic_id << 32) | low);
        return;
    }
    while (apic_read(LAPIC_ICR_LOW) & (1 << 12));
    apic_write(LAPIC_ICR_HIGH, dest_apic_id << 24);
    apic_write(LAPIC_ICR_LOW, low);
}

void apic_send_ipi(u32 dest_apic_id, u32 vector, u32 delivery, u32 level)
{
    icr_send(dest_apic_id, vector | (delivery << 8) | (level << 14));
}

void apic_send_init_ipi(u32 dest_apic_id)
{
    /* INIT, level assert, level-triggered (bit 15 = trigger mode).
     * x2APIC supports INIT and STARTUP delivery; only the (deprecated)
     * INIT *de-assert* is excluded, which we never send. */
    icr_send(dest_apic_id, (5 << 8) | (1 << 14) | (1 << 15));
}

void apic_send_sipi(u32 dest_apic_id, u32 vector)
{ apic_send_ipi(dest_apic_id, vector, 6, 1); /* SIPI */ }

/* ---- IOAPIC ---- */
static volatile u32 *ioapic_base;

static u32 ioapic_read(u32 reg)
{
    ioapic_base[0] = reg;
    return ioapic_base[4];
}

static void ioapic_write(u32 reg, u32 val)
{
    ioapic_base[0] = reg;
    ioapic_base[4] = val;
}

void ioapic_init(void)
{
    /* The MADT is authoritative for where the IOAPIC lives — 0xFEC00000
     * is only the conventional address.  Without ACPI, fall back to it. */
    u32 phys = acpi_ioapic_base();
    if (!phys) phys = (u32)IOAPIC_BASE;

    /* map the IOAPIC page into DEV_VMA */
    vmm_map_kernel_page(IOAPIC_VA, phys, PG_PCD);
    ioapic_base = (volatile u32 *)IOAPIC_VA;

    u32 ver = ioapic_read(IOAPIC_VER);
    u32 max = (ver >> 16) & 0xFF;
    if (max == 0) max = 23;
    kprintf("[ioapic] base=%p version=%u max_irq=%u\n",
            (void *)(u64)phys, ver & 0xFF, max);

    /* mask all IRQ lines */
    for (u32 i = 0; i <= max; i++) ioapic_mask_irq(i);
}

void ioapic_set_irq(u32 irq, u32 vector, u32 dest_apic_id)
{
    u32 base = IOAPIC_REDIR_BASE + irq * 2;
    ioapic_write(base, vector);                      /* low: vector */
    ioapic_write(base + 1, (u32)dest_apic_id << 24); /* high: dest APIC ID */
}

void ioapic_mask_irq(u32 irq)
{
    u32 base = IOAPIC_REDIR_BASE + irq * 2;
    ioapic_write(base, ioapic_read(base) | (1 << 16));
}

void ioapic_unmask_irq(u32 irq)
{
    u32 base = IOAPIC_REDIR_BASE + irq * 2;
    ioapic_write(base, ioapic_read(base) & ~(1 << 16));
}
