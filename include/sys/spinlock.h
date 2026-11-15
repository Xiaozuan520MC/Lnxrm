/* Spinlock: busy-wait mutual exclusion for SMP.
 * The irqsave variant is the default: most locks in this kernel can be
 * taken from interrupt context (handlers print, the tick calls the
 * scheduler), so a lock held with interrupts on would self-deadlock when
 * the same CPU re-enters it from an IRQ.  The one exception, and the reason
 * this API is no longer irqsave-only, is spin_lock_irqon() below (C7): a
 * lock no interrupt path can reach -- fat_fs_lock -- may be acquired
 * WITHOUT the lock deciding interrupt state at all.  The historical non-irq
 * spin_lock()/spin_unlock()/spin_init() trio had no callers left after
 * every call site migrated to irqsave and was removed (2026-10-01);
 * static initialization goes through SPINLOCK_INIT. */
#pragma once
#include <types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    volatile u32 locked; /* 0=free, 1=held */
    u32 pad;
} spinlock_t;

#define SPINLOCK_INIT {0, 0}

/* Disable interrupts, acquire lock, save old IF into *flags. */
void spin_lock_irqsave(spinlock_t *l, u64 *flags);
/* Restore old IF and release lock. */
void spin_unlock_irqrestore(spinlock_t *l, u64 flags);

/* C7: acquire WITHOUT touching interrupt state -- interrupts stay exactly
 * as they were (clear: every caller today arrives through an interrupt
 * gate, so IF is already 0 when this runs).
 *
 * Only for a lock that is never taken from interrupt or exception context,
 * which today means fat_fs_lock: the sole takers are the fs_ops wrappers in
 * fat32.c, all reached from ring-0 syscall paths, and no IRQ handler, the
 * tick, or panic() goes near it.  What the lock gives up is ownership of
 * IF, not a freeze it imposed: at today's IF=0 callers this only drops the
 * cli/popfq pair, and it becomes load-bearing the moment a caller runs with
 * interrupts on (GAP §3, the "syscall runs with interrupts off" item),
 * where irqsave would stop this CPU's tick for the length of every FS
 * operation.
 *
 * The contract two things depend on: never take such a lock from an IRQ
 * handler (AHCI's lifted poll window means an IRQ can land mid-hold), and
 * never schedule() while holding it (this kernel does not preempt ring-0
 * tasks, which is exactly what keeps a holder on its CPU). */
void spin_lock_irqon(spinlock_t *l);
void spin_unlock_irqon(spinlock_t *l);
/* C27 panic path: acquire only when free, never wait.  Same irqsave
 * contract; false denies without touching the lock. */
bool spin_trylock_irqsave(spinlock_t *l, u64 *flags);

#ifdef __cplusplus
}
#endif
