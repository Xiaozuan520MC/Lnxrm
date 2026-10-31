/* Per-CPU data management and GS segment setup. */
#include <sys/percpu.h>
#include <sys/cpu.h>
#include <mm/mm.h>
#include <console.h>

struct cpu_info cpu_table[MAX_CPUS] __attribute__((aligned(CPU_DATA_SIZE)));

/* Read GS base from MSR. */
static u64 cpu_get_gs_base(void)
{ return rdmsr(0xC0000101); }

void cpu_set_gs_base(u64 base)
{ wrmsr(0xC0000101, base); }

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

