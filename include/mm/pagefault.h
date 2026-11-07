/* T-090/T-094/T-MMAP: page-fault dispatch -- demand paging, lazy stack
 * growth and copy-on-write. */
#pragma once
#include <types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct intr_frame;
struct task;

/* Try to RESOLVE a user-mode page fault before the caller reports or kills.
 * Returns true if the fault was fixed (the caller should iretq back and
 * retry the faulting instruction); false means "not mine" (the caller falls
 * through to the existing SIGSEGV/panic path, which stays the safe default).
 *
 * Only ever called for a user-mode #PF (isr.c gates on cs&3).  A kernel #PF
 * still panics: there is no fixup table, and a ring-0 fault is a bug. */
bool page_fault_try_fix(struct intr_frame *f, u64 cr2);

/* Break the COW sharing on `page` in `t`'s address space (copy the frame,
 * take it writable, clear PG_COW).  True when the page no longer blocks a
 * write -- including when it was never COW; false when it is not present
 * or the copy ran out of frames.  Called by the #PF fixer above and by
 * task.c's user_ptr_writable() before any ring-0 store through a user
 * address (fork leaves every writable page read-only on both sides). */
bool page_cow_break(struct task *t, u64 page);

#ifdef __cplusplus
}
#endif
