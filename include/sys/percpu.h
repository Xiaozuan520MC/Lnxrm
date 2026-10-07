/* Per-CPU data: accessed via GS segment base. */
#pragma once
#include <types.h>
#include <sys/sched.h>
#include <sys/spinlock.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_CPUS 8

struct cpu_info {
    u32 id;       /* logical CPU id */
    u32 apic_id;  /* APIC ID */
    bool started; /* true once AP is online */
    bool bsp;     /* true for the bootstrap processor */

    struct task *_current; /* current running task on this CPU */
    struct task *idle;     /* this CPU's idle task (never on a runqueue) */
    struct task runq_head; /* per-CPU run queue sentinel */

    /* Note: idle context/stack live in the idle TASK (task_table), not
     * here; per-CPU run queues are guarded by sched.c's runq_lock. */

    volatile bool need_resched; /* per-CPU reschedule flag */
};

/* GS-relative per-CPU struct. Size must be a power of 2 for easy addressing. */
#define CPU_DATA_SIZE 4096

/* Read/write the per-CPU pointer via GS. */
struct cpu_info *this_cpu_data(void);
void cpu_set_gs_base(u64 base);
void cpu_init_percpu(u32 cpu_id, u32 apic_id);

/* All CPU info (indexed by logical id). */
extern struct cpu_info cpu_table[MAX_CPUS];

#ifdef __cplusplus
}
#endif
