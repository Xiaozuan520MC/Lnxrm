/* CPU bring-up: GDT/TSS, IDT, APIC, PIT timer, MSR helpers. */
#pragma once
#include <types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GDT register structure (matches x86 LGDT format) */
struct gdtr {
    u16 limit;
    u64 base;
} __attribute__((packed));

void cpu_init(void);
void gdt_init(void);
void gdt_init_cpu(int cpu); /* per-CPU GDT + TSS load (cpu id 0..MAX_CPUS-1) */
void idt_init(void);
void pit_init(u32 hz);
/* One-time LAPIC-period + TSC-rate measurement (BSP, before smp_init). */
u32 lapic_timer_calibrate(void);
void lapic_timer_start(u32 vector, u32 hz); /* any CPU, uses the shared period */
void mdelay(u32 ms);                        /* TSC-based busy wait */
u64 tsc_per_ms(void);                       /* calibrated TSC rate (0 = not yet) */
u64 rdmsr(u32 msr);
void wrmsr(u32 msr, u64 v);
u64 rdtsc(void);

#define MSR_IA32_EFER 0xC0000080
#define EFER_NXE      (1ULL << 11) /* PTE XD (bit 63) honoured */

/* ---- CR0 / CR4 control bits (Intel SDM Vol.3 Ch.2) ---- */
#define CR0_MP         (1UL << 1)
#define CR0_EM         (1UL << 2)
#define CR0_TS         (1UL << 3)
#define CR0_NE         (1UL << 5)
#define CR0_WP         (1UL << 16) /* ring-0 stores honor the page R/W bit:
                                    * a kernel write to a read-only page
                                    * raises #PF instead of landing */
#define CR4_OSFXSR     (1UL << 9)
#define CR4_OSXMMEXCPT (1UL << 10)
#define CR4_SMEP       (1UL << 20) /* supervisor-mode execution prevention:
                                    * CPL=0 cannot fetch from a user page */
#define CR4_SMAP       (1UL << 21) /* supervisor-mode access prevention:
                                    * CPL=0 cannot LOAD/STORE a user page
                                    * (U/S=1) unless EFLAGS.AC=1 via STAC */

/* CPUID.7.0:EBX.SMEP -- false on models that have no such bit (the bare
 * qemu64 default), where writing CR4.SMEP would raise #GP. */
bool cpu_has_smep(void);
/* CPUID.7.0:EBX.SMAP (bit 20, same register as SMEP) -- same trap: writing
 * CR4.SMAP on a model without it is a reserved-bit #GP. */
bool cpu_has_smap(void);
/* Idempotent: set CR0.WP and (when the CPU advertises it) CR4.SMEP and
 * CR4.SMAP on the calling CPU.  The boot asm turns CR0.WP on together with
 * CR0.PG; this is the C-level backstop (BSP cpu_init, every AP ap_main) and
 * the only place CR4.SMEP/CR4.SMAP are set, because those need a CPUID
 * gate.  Also publishes g_smap so the uaccess helpers below know whether
 * STAC/CLAC exist at all (#UD when CR4.SMAP=0). */
void cpu_protect_init(void);

/* ---- SMAP uaccess windows (only valid once cpu_protect_init ran) ---- */
/* True iff this CPU has SMAP enabled right now (CR4.SMAP set).  Written by
 * cpu_protect_init(), read by every helper and by the ring-0 entry points
 * (isr_common/syscall_entry clear AC on entry so a user-set AC bit cannot
 * smuggle ring-0 access past SMAP). */
extern bool g_smap;

/* Open/close an AC=1 window for touching USER pages from ring 0.
 * Save/restore the whole flags word: nesting works (inner leave restores
 * the outer AC=1), the caller's IF survives, and a plain no-op when the
 * CPU never advertised SMAP.  Every direct dereference of a user VA inside
 * the kernel must sit between these two calls -- copy_from/to_user do it
 * for you; hand-rolled stores (signal frames, the initial argv stack) do
 * it themselves.  Pages reached through the high-half phys alias are NOT
 * user pages (U/S=0) and need no window. */
static inline u64 smap_enter(void)
{
    u64 f;
    if (!g_smap) return 0;
    __asm__ volatile("pushfq; pop %0; stac" : "=r"(f) ::"memory");
    return f;
}
static inline void smap_leave(u64 saved)
{
    if (!g_smap) return;
    __asm__ volatile("push %0; popfq" ::"r"(saved) : "memory", "cc");
}

/* GDT/TSS reload (entry64.S) */
void gdt_reload(const struct gdtr *gdtr);
void tss_load(u16 sel);
void tss_set_rsp0(u64 rsp);

/* Forward-declare interrupt frame (defined in sched.h). */
struct intr_frame;

/* irq registration (kernel/isr.c) */
typedef void (*irq_handler_t)(struct intr_frame *);
void isr_common(struct intr_frame *f);
void irq_install(int irq, irq_handler_t h);
irq_handler_t irq_get_handler(int irq);

/* IPI handler registration (for vectors 0xF0..0xF2) */
typedef void (*ipi_handler_t)(void);
void ipi_handler_install(u32 vector, ipi_handler_t h);
void ipi_dispatch(u32 vector);

/* input queue (kernel/isr.c feeds it; serial too) */
void kbd_irq_handler(struct intr_frame *f);
void serial_rx_handler(struct intr_frame *f);
void input_push(char c);
int input_pop(void); /* -1 if empty */
struct task;
void console_waiter_arm(struct task *t);
void console_waiter_disarm(struct task *t);

extern volatile u64 jiffies;
#define HZ 100

#ifdef __cplusplus
}
#endif
