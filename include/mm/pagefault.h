/* T-090/T-094: page-fault dispatch and lazy user-stack growth. */
#pragma once
#include <types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct intr_frame;

/* Try to RESOLVE a user-mode page fault before the caller reports or kills.
 * Returns true if the fault was fixed (the caller should iretq back and
 * retry the faulting instruction); false means "not mine" (the caller falls
 * through to the existing SIGSEGV/panic path, which stays the safe default).
 *
 * Only ever called for a user-mode #PF (isr.c gates on cs&3).  A kernel #PF
 * still panics: there is no fixup table, and a ring-0 fault is a bug. */
bool page_fault_try_fix(struct intr_frame *f, u64 cr2);

#ifdef __cplusplus
}
#endif
