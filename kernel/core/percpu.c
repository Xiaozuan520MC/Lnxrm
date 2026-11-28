// SPDX-License-Identifier: Apache-2.0

/* Per-CPU data management and GS segment setup. */
#include <sys/percpu.h>
#include <sys/cpu.h>
#include <console.h>

/* arch/entry64.S reads entry_stack_top and entry_scratch straight out of GS
 * using its own %defines (CPU_OFF_ENTRY_STACK_TOP / CPU_OFF_ENTRY_SCRATCH).
 * Two files spelling the same offset is how an asm/C contract drifts, so the
 * numbers are asserted here, where the compiler can see the struct: change
 * the layout and the build stops instead of entry_SYSCALL_64 loading RSP
 * from whatever now sits at offset 0. (2026-11-27) */
LNXRM_STATIC_ASSERT(offsetof(struct cpu_info, entry_stack_top) == 0,
                    "entry64.S CPU_OFF_ENTRY_STACK_TOP must stay 0");
LNXRM_STATIC_ASSERT(offsetof(struct cpu_info, entry_scratch) == 8,
                    "entry64.S CPU_OFF_ENTRY_SCRATCH must stay 8");

struct cpu_info cpu_table[MAX_CPUS] __attribute__((aligned(CPU_DATA_SIZE)));

/* Read GS base from MSR. */
static u64 cpu_get_gs_base(void)
{ return rdmsr(MSR_IA32_GS_BASE); }

/* Publish this CPU's kernel GS base, and initialise the shadow SWAPGS
 * exchanges it with.  Both writes belong together (S3.4): leaving
 * IA32_KERNEL_GS_BASE at whatever the previous occupant of the MSR left --
 * or at the hardware reset value 0 that nothing in this tree ever wrote
 * before S3.4 -- means the first SWAPGS out of ring 0 hands ring 3 a stale
 * base and, worse, the next SWAPGS back in hands the kernel that same stale
 * value as its per-CPU pointer.  Zero is the right initial user GS: this
 * ABI gives ring 3 nothing to put there, and arch_prctl(ARCH_SET_GS) is the
 * only writer from now on.
 *
 * Called once per CPU at bring-up (main.c for the BSP, gdt_init_cpu after
 * a GDT reload, ap_main for every AP) -- never while a user GS is live,
 * which is what makes resetting the shadow here safe. */
void cpu_set_gs_base(u64 base)
{
    wrmsr(MSR_IA32_GS_BASE, base);
    wrmsr(MSR_IA32_KERNEL_GS_BASE, 0);
}

struct cpu_info *this_cpu_data(void)
{ return (struct cpu_info *)cpu_get_gs_base(); }

void cpu_init_percpu(u32 cpu_id, u32 apic_id)
{
    struct cpu_info *c = &cpu_table[cpu_id];
    memset(c, 0, sizeof(*c));
    c->id = cpu_id;
    c->apic_id = apic_id;
    c->bsp = (cpu_id == 0);
    c->started = true;
    c->_current = NULL;
    c->need_resched = false;

    /* Set GS base so this_cpu_data() works for the current CPU. */
    /* NOTE: this must also be done after gdt_init() because mov gs,ax
     * in gdt_reload can reset the hidden base from the GDT entry. */

    /* per-CPU run queue: circular singly linked list via rq_next */
    c->runq_head.pid = 0;
    c->runq_head.state = T_UNUSED;
    c->runq_head.rq_next = &c->runq_head;
}

