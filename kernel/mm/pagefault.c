/* T-090/T-094: page-fault dispatch + lazy user-stack growth.
 *
 * isr_common() used to treat every #PF as fatal: report, then SIGSEGV (user)
 * or panic (kernel).  That is right for a fault nobody can fix, but it made
 * a deepening user stack lethal -- sys_execve maps only 16 KiB of stack
 * (task.c) and nothing could extend it, so a moderately deep recursion
 * died.  This module adds the missing "try to fix it first" step and mounts
 * the first fixer: grow the stack down on demand.
 *
 * The dispatch is the seam future fixers plug into -- demand paging, COW,
 * mmap fill, swap-in.  Each inspects (cr2, err) and claims the fault if it
 * can resolve it; the first claim wins and the safe default (kill) is kept
 * for everything else.
 */
#include <sys/sched.h>
#include <mm/mm.h>
#include <console.h>

/* How far the stack may grow below its initial mapping.  The initial stack
 * sits at [USER_STACK_TOP-0x4000, USER_STACK_TOP).  brk is hard-capped at
 * USER_STACK_TOP-1 MiB (syscall.c sys_brk), so the heap can never rise above
 * that line.  Keeping the stack's floor ABOVE that ceiling means a grown
 * stack and a maxed heap meet exactly at 1 MiB below the top and never
 * overlap.  512 KiB of stack is far more than these programs need and leaves
 * a clean 512 KiB gap between the two ceilings. */
#define USTACK_MAX_GROW 0x80000ULL /* 512 KiB */

/* Resolve one user-mode #PF by growing the stack so it covers `page`.
 * Returns true if the fault is now fixed.  Conservative by design: it
 * declines anything outside [USER_STACK_TOP-USTACK_MAX_GROW, ustack_lo) or
 * that would breach the per-task memory quota, so a runaway recursion or a
 * wild pointer dies as SIGSEGV instead of eating the machine. */
static bool stack_grow(struct intr_frame *f, u64 cr2)
{
    /* Only a NOT-PRESENT fault is a candidate.  A protection fault (P=1) is
     * a real access violation (e.g. write to a read-only page) that growth
     * must not paper over. */
    if (f->err & 1) return false;

    struct task *t = current;
    if (!t || t->ustack_lo == 0) return false; /* no tracked stack (idle/kernel) */

    if (cr2 < USER_BASE || cr2 >= USER_MAX_VMA) return false;

    u64 page = cr2 & ~4095ULL;
    if (page >= USER_STACK_TOP) return false;          /* not below the stack top */
    if (page < USER_STACK_TOP - USTACK_MAX_GROW) return false; /* too far: overflow/wild */
    if (page >= t->ustack_lo) return false;            /* already mapped (shouldn't fault) */

    /* How many pages to add.  Both ends are page-aligned, so this is exact. */
    u64 npages = (t->ustack_lo - page) / PAGE_SIZE;

    /* Refuse BEFORE taking any frame: a plain user's whole address space --
     * image + stack + restorer + heap -- is capped by cred_mem_quota().  This
     * is the same ceiling sys_brk honours, so stack growth cannot be used to
     * sidestep the 64 MiB limit. */
    u64 quota = cred_mem_quota(&t->cred);
    if (quota && t->mem_pages + npages > quota) return false;

    /* Map page by page from the current low-watermark down to `page`.  Each
     * page is zeroed (a fresh stack frame must not leak the previous
     * occupant's bytes) and installed NX+user+writable: the stack is data,
     * so a stray jump into it faults (every user page but the sigreturn
     * restorer is non-executable). */
    u64 va = t->ustack_lo;
    while (va > page) {
        va -= PAGE_SIZE;
        u64 pa = pmm_alloc();
        if (!pa) {
            /* Out of frames part-way.  Whatever was mapped stays mapped and
             * accounted (the address space is torn down correctly on exit);
             * the faulting page is still missing, so report failure and let
             * the caller turn it into SIGSEGV rather than loop forever. */
            return false;
        }
        memset((void *)PHYS_TO_VIRT(pa), 0, PAGE_SIZE);
        if (vmm_map_user(t->pml4, va, pa, true, true, false) < 0) {
            pmm_free(pa);
            return false;
        }
        t->ustack_lo = va;
        t->mem_pages++;
    }
    if (t->mem_pages > t->mem_peak) t->mem_peak = t->mem_pages;
    return true;
}

bool page_fault_try_fix(struct intr_frame *f, u64 cr2)
{
    /* Dispatch: offer the fault to each fixer in order.  Today there is one
     * (lazy stack growth).  A future COW fixer will also want protection
     * faults and so must check (f->err & 1) itself; stack_grow() declines
     * them above. */
    return stack_grow(f, cr2);
}
