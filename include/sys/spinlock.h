/* Spinlock: busy-wait mutual exclusion for SMP.
 * Only the irqsave variant is exported: every lock in this kernel can be
 * taken from interrupt context (handlers print, the tick calls the
 * scheduler), so a lock held with interrupts on would self-deadlock when
 * the same CPU re-enters it from an IRQ.  The historical non-irq
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

#ifdef __cplusplus
}
#endif
