// SPDX-License-Identifier: Apache-2.0

/* Per-CPU data: accessed via GS segment base. */
#pragma once
#include <types.h>
#include <sys/sched.h>

#ifdef __cplusplus
extern "C" {
#endif

/* four.4: per-CPU cache of order-0 (4 KiB) frames.  A small lock-free stash
 * that keeps the single most common allocation off the global buddy lock.
 * Only the owning CPU touches its own entry, always with interrupts off
 * (see pmm.c), so no cross-CPU synchronisation is needed. */
#define PCACHE_N 32

struct cpu_info {
    /* SYSCALL entry (arch/entry64.S) reaches these two with a GS-relative
     * load and nothing else, so they come FIRST: their offsets are pinned
     * by _Static_assert in percpu.c, and putting them ahead of every other
     * member keeps them at 0 and 8 no matter how wide struct task (which
     * this struct embeds) ever grows.  A field inserted above them would
     * silently point entry_SYSCALL_64 at the wrong word. */
    u64 entry_stack_top; /* top of the ring3->ring0 entry stack; always the
                          * value tss_set_rsp0() just gave TSS.rsp0, so a
                          * `syscall` builds its frame exactly where the
                          * `int 0x80` gate would have.  0 until the first
                          * switch_to(), which is also when ring 3 first
                          * becomes reachable. */
    u64 entry_scratch;   /* SYSCALL-entry scratch: the user RSP, parked here
                          * across the stack switch (SYSCALL saves nothing
                          * and every GPR is already spoken for by the
                          * frame).  Same idea as Linux's TSS.sp2. */

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

    u64 pcache[PCACHE_N]; /* order-0 frame stash (four.4) */
    int pcache_n;         /* frames currently in the stash */
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
