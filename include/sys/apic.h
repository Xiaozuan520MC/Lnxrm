/* Local APIC + IPI interface. */
#pragma once
#include <types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Local APIC register offsets (MMIO, 16-byte aligned) */
#define LAPIC_ID          0x020
#define LAPIC_TPR         0x080
#define LAPIC_EOI         0x0B0
#define LAPIC_SPURIOUS    0x0F0
#define LAPIC_ICR_LOW     0x300
#define LAPIC_ICR_HIGH    0x310
#define LAPIC_LVT_TIMER   0x320
#define LAPIC_LVT_THERMAL 0x330
#define LAPIC_LVT_PMC     0x340
#define LAPIC_LVT_LINT0   0x350
#define LAPIC_LVT_LINT1   0x360
#define LAPIC_LVT_ERROR   0x370
#define LAPIC_TIMER_ICR   0x380
#define LAPIC_TIMER_CCR   0x390
#define LAPIC_TIMER_DCR   0x3E0

/* MSR addresses */
#define MSR_IA32_APIC_BASE 0x1B

/* LVT timer modes */
#define LVT_TIMER_PERIODIC 0x20000

/* APIC ID */
#define MAX_CPUS    8

void apic_init(void); /* init LAPIC on BSP */
void apic_enable(void);
void apic_eoi(void);
void apic_write(u32 reg, u32 val);
u32 apic_read(u32 reg);
u32 apic_get_id(void);

/* IPI */
void apic_send_ipi(u32 dest_apic_id, u32 vector, u32 delivery, u32 level);
void apic_send_init_ipi(u32 dest_apic_id);
void apic_send_sipi(u32 dest_apic_id, u32 vector);

/* IOAPIC (minimal) */
#define IOAPIC_BASE       0xFEC00000UL
#define IOAPIC_VER        0x01
#define IOAPIC_REDIR_BASE 0x10

void ioapic_init(void);
void ioapic_set_irq(u32 irq, u32 vector, u32 dest_apic_id);
void ioapic_mask_irq(u32 irq);
void ioapic_unmask_irq(u32 irq);

/* Per-CPU LAPIC MMIO base (set during ap init) */
extern volatile u32 *lapic_base;

#ifdef __cplusplus
}
#endif
