/* Minimal ACPI discovery: RSDP -> RSDT/XSDT -> MADT.
 * Only what SMP + the IOAPIC need: which local APIC IDs exist, and where
 * the IOAPIC sits.
 *
 * The RSDP lives below 1 MiB and is read through the identity map.  The
 * tables it points at do NOT: real firmware parks them at the top of low
 * RAM — routinely above the identity window entry64.S lays down, where a
 * plain dereference either faults or (past 2 GiB, via the linear alias)
 * reads an unrelated frame.  acpi_init() therefore runs after vmm_init()
 * and copies every table through the one-page bounce window below. */
#pragma once
#include <types.h>

/* One-page bounce window: acpi.c maps the physical page holding the bytes
 * it needs here, copies them out, and moves on (vmm_map_kernel_page does
 * the invlpg).  PD_HI[108] — kernel/image slots 0..31, VGA 96, LFB 104..107,
 * device 128, heap 256..271 are taken; vmm.c's alias loop skips this slot
 * for the same reason it skips those.  Keep in sync with kernel/mm/vmm.c. */
#define ACPI_SCRATCH_VA 0xffffffff8e000000UL

#ifdef __cplusplus
extern "C" {
#endif

/* Locate the RSDP in the legacy BIOS areas and parse the MADT.
 * Not fatal when nothing is found — acpi_collect_ap_ids() then reports 0
 * and the caller falls back to probing APIC IDs itself.  Call once,
 * before cpu_init() (ioapic_init() wants the MADT's IOAPIC address). */
void acpi_init(void);

/* Enabled local APIC / x2APIC IDs from the MADT (INCLUDING the BSP's own,
 * deduplicated).  Returns 0 when there is no usable MADT.  Capped at
 * max — entries beyond MAX_CPUS exist on big machines but this kernel
 * cannot start them anyway. */
u32 acpi_collect_ap_ids(u32 *ids, u32 max);

/* First IOAPIC's MMIO address from the MADT, or 0 if unknown — the caller
 * then uses the conventional IOAPIC_BASE. */
u32 acpi_ioapic_base(void);

#ifdef __cplusplus
}
#endif
