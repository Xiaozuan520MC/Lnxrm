/* T-090/T-094/T-MMAP: page-fault dispatch -- demand paging, lazy stack
 * growth and copy-on-write in one seam.
 *
 * isr_common() used to treat every #PF as fatal: report, then SIGSEGV (user)
 * or panic (kernel).  That is right for a fault nobody can fix, but it made
 * a deepening user stack lethal (sys_execve maps only 16 KiB of stack), and
 * it had nowhere to put the two later arrivals: an mmap region whose first
 * touch must materialize a frame, and a fork-shared page whose write must
 * be copied out.
 *
 * The dispatch is the seam fixers plug into.  Each inspects (cr2, err) and
 * claims the fault if it can resolve it; the first claim wins and the safe
 * default (kill) is kept for everything else.  Today three mount here, in
 * order: demand_page (not-present inside an mmap VMA), stack_grow
 * (not-present inside the stack growth window), cow_fault (write-protect
 * fault on a PG_COW page).  A write-protect fault on a page that is neither
 * writable nor COW is a genuine read-only violation and deliberately falls
 * through to SIGSEGV.  A file-backed fixer or swap-in would join here the
 * same way.
 */
#include <sys/sched.h>
#include <mm/mm.h>
#include <mm/vma.h>
#include <mm/pagefault.h>
#include <console.h>

/* How far the stack may grow below its initial mapping.  The initial stack
 * sits at [USER_STACK_TOP-0x4000, USER_STACK_TOP).  Below the growth floor
 * lies STACK_GUARD bytes that NOBODY maps (types.h: MMAP_TOP stops one
 * guard below this floor), then the mmap window [MMAP_LO, MMAP_TOP), with
 * brk capped at MMAP_LO -- so heap, mmap, guard and stack approach each
 * other and never overlap.  512 KiB of stack is far more than these
 * programs need. */
#define USTACK_MAX_GROW 0x80000ULL /* 512 KiB */

/* How far below the running user RSP a fault may sit and still be treated
 * as a stack access worth growing for.  x86-64 SysV grants leaf functions
 * a 128-byte red zone below RSP without moving it, and a prologue always
 * subtracts a frame from RSP before touching it, so every legitimate stack
 * fault lands at or above RSP minus this window; a wild store aimed far
 * below the stack does not, and must die as SIGSEGV rather than be handed
 * a fresh page.  Deliberately equal to STACK_GUARD (types.h): with the
 * stack at its deepest, every fault this window accepts falls inside the
 * guard and can never reach an mmap VMA. */
#define STACK_GROW_WINDOW 0x10000ULL /* 64 KiB */

LNXRM_STATIC_ASSERT(USER_STACK_TOP - MMAP_TOP == USTACK_MAX_GROW + STACK_GROW_WINDOW,
                    "STACK_GUARD must equal STACK_GROW_WINDOW: the mmap window "
                    "stops one guard below the stack floor");

/* The window must sit INSIDE user space: below USER_BASE a mmap could never
 * be mapped (vmm_map_user refuses) and a mprotect over it would be a bogus
 * EINVAL.  A wrong shift here -- one digit in the 64 GiB -- drops MMAP_LO
 * 8 TiB under USER_BASE, which is exactly the bug that made a mprotect over
 * an empty window answer EINVAL instead of ENOMEM. */
LNXRM_STATIC_ASSERT(MMAP_LO >= USER_BASE,
                    "mmap window must lie inside [USER_BASE, MMAP_TOP)");
LNXRM_STATIC_ASSERT(MMAP_TOP <= USER_STACK_TOP - USTACK_MAX_GROW,
                    "mmap window must stop at or below the stack growth floor");

/* Resolve one user-mode #PF by growing the stack so it covers `page`.
 * Returns true if the fault is now fixed.  Conservative by design: it
 * declines anything outside [USER_STACK_TOP-USTACK_MAX_GROW, ustack_lo),
 * anything further than STACK_GROW_WINDOW below the running user RSP, or
 * anything that would breach the per-task memory quota, so a runaway
 * recursion or a wild pointer dies as SIGSEGV instead of eating the
 * machine. */
static bool stack_grow(struct intr_frame *f, u64 cr2)
{
    /* Only a NOT-PRESENT fault is a candidate.  A protection fault (P=1) is
     * a real access violation (e.g. write to a read-only page) that growth
     * must not paper over. */
    if (f->err & 1) return false;

    /* A FETCH is never a stack access either: every user page but the
     * sigreturn restorer is mapped NX, so growing for one would hand a
     * stray jump a fresh frame and then fault again on its very first
     * instruction -- one wasted page per wild jump, and no progress. */
    if (f->err & 0x10) return false;

    struct task *t = current;
    if (!t || t->ustack_lo == 0) return false; /* no tracked stack (idle/kernel) */

    if (cr2 < USER_BASE || cr2 >= USER_MAX_VMA) return false;

    u64 page = cr2 & ~4095ULL;
    if (page >= USER_STACK_TOP) return false;          /* not below the stack top */
    if (page < USER_STACK_TOP - USTACK_MAX_GROW) return false; /* too far: overflow/wild */
    if (page >= t->ustack_lo) return false;            /* already mapped (shouldn't fault) */

    /* Must look like a STACK access: no further than STACK_GROW_WINDOW
     * below the running user RSP.  f->ussp is the hardware-pushed user RSP
     * (isr_common only offers us CPL3 faults), and a frame with no user RSP
     * -- ktest builds its own -- has no reference point, so the check is
     * skipped for it.  A deepening recursion drags RSP down with it and
     * passes; a store aimed megabytes below the stack is refused here,
     * before a single frame is taken. */
    u64 rsp = f->ussp;
    if ((f->cs & 3) == 3 && rsp >= USER_BASE && rsp < USER_MAX_VMA &&
        cr2 + STACK_GROW_WINDOW < rsp)
        return false;

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

/* ---- fixer: demand paging for mmap'd ranges (T-MMAP) ----
 *
 * mmap() only records a VMA (mm/vma.c); the frame arrives here, on first
 * touch.  Zeroed on allocation (an anonymous page starts as zeros, never a
 * previous occupant's bytes), mapped with the VMA's protection, and metered
 * through the same cred_mem_quota() ceiling brk and stack growth honour --
 * without that, mmap would turn the 64 MiB cap into a suggestion.  A range
 * nobody touches costs nothing but one VMA node, which is the whole point
 * of the window being 64 GiB wide. */
static bool demand_page(struct intr_frame *f, u64 cr2)
{
    if (f->err & 1) return false; /* present fault: not a missing page */
    struct task *t = current;
    if (!t) return false;
    if (cr2 < USER_BASE || cr2 >= USER_MAX_VMA) return false;

    u64 page = cr2 & ~4095ULL;

    /* The stack guard (types.h) belongs to nobody: the mmap window stops
     * below it, so no VMA can cover it -- this is the belt to that brace,
     * an overflow crossing the growth floor can never be claimed as a
     * legit mmap touch even if a list somehow reached up here. */
    if (page >= MMAP_TOP && page < USER_STACK_TOP - USTACK_MAX_GROW) return false;

    struct vma *v = vma_find(t, page);
    if (!v) return false; /* no mmap claims it -> stack_grow or kill */

    /* The VMA is asked FIRST, before a single frame is taken -- the same
     * order Linux settles faults in (access_error() runs ahead of
     * handle_mm_fault()).  A store into a PROT_READ region or a fetch from
     * a region without PROT_EXEC is going to be killed; allocating for it
     * would only hand the doomed process a frame to its name.  (Reads are
     * never refused: x86 cannot disable reading without PKU, so PROT_NONE
     * degrades to read-only everywhere, as it does at mmap time.) */
    if ((f->err & 2) && !(v->prot & LNXRM_PROT_WRITE)) return false;
    if ((f->err & 0x10) && !(v->prot & LNXRM_PROT_EXEC)) return false;

    /* Already there?  The hardware will not raise a not-present fault for
     * a present page, but the dispatcher is also called directly (ktest)
     * and a stale-TLB retry could bring the same cr2 back twice: without
     * this guard the second pass would allocate a frame, "widen" the
     * existing mapping (which ignores pa) and leak both the frame and a
     * mem_pages increment. */
    if (vmm_user_pte(t->pml4, page)) return false;

    u64 quota = cred_mem_quota(&t->cred);
    if (quota && t->mem_pages + 1 > quota) return false;

    u64 pa = pmm_alloc();
    if (!pa) return false;
    memset((void *)PHYS_TO_VIRT(pa), 0, PAGE_SIZE);
    /* PROT_READ is implicit (every present page is readable -- x86 has no
     * read-disable without PKU), PROT_NONE therefore degrades to read-only;
     * W and NX are exactly what the VMA asked for. */
    if (vmm_map_user(t->pml4, page, pa, (v->prot & LNXRM_PROT_WRITE) != 0, true,
                     (v->prot & LNXRM_PROT_EXEC) != 0) < 0) {
        pmm_free(pa);
        return false;
    }
    t->mem_pages++;
    if (t->mem_pages > t->mem_peak) t->mem_peak = t->mem_pages;
    return true;
}

/* ---- fixer: copy-on-write (T-COW) ----
 *
 * Break the sharing on `page` for address space `t`: clone the shared
 * frame, take it writable and clear PG_COW, then hand back THIS side's
 * reference to the old frame (pmm_free drops the count; the other sharer's
 * keeps the old frame alive until it breaks or unmaps too).
 *
 * Ordering is what makes this race-free between two processes breaking the
 * same frame on two CPUs: copy FIRST, swap the PTE SECOND, drop our old
 * reference LAST.  Our reference alone pins the old frame's bytes until
 * our own copy is done, and the count reaches 0 only after every sharer
 * has copied -- so no copy can ever read a frame the buddy already gave
 * to someone else.
 *
 * Also the routine syscalls need BEFORE they store through a user pointer
 * (task.c user_ptr_writable): after fork, a writable page is PTE-read-only
 * on both sides, and copy_to_user's writability gate would otherwise reject
 * a perfectly legitimate write into the child's (or parent's) own memory.
 * Every kernel store path then gets COW resolution from one place.
 *
 * Returns true when the page is no longer blocking a write: it was never
 * COW (a plain read-only page -- the caller's own gate decides what that
 * means), or the break succeeded.  False = not present, or out of frames
 * (the page stays COW, the caller reports its own error). */
bool page_cow_break(struct task *t, u64 page)
{
    if (!t) return false;
    u64 pte = vmm_user_pte(t->pml4, page);
    if (!(pte & PG_P)) return false;
    if (!(pte & PG_COW)) return true; /* not COW: nothing to resolve here */

    u64 npa = pmm_alloc();
    if (!npa) return false;
    /* Read the shared frame through the high-half alias, never through
     * `page`: that would resolve against the LIVE cr3 (which may be another
     * address space) and a user page would trip SMAP with no STAC window. */
    memcpy((void *)PHYS_TO_VIRT(npa), (void *)PHYS_TO_VIRT(pte & PTE_PA_MASK), PAGE_SIZE);
    u64 fresh = (pte & ~PTE_PA_MASK) | (npa & PTE_PA_MASK); /* same flags, new frame */
    fresh = (fresh | PG_W) & ~PG_COW;                       /* ours to write now */
    if (vmm_user_pte_write(t->pml4, page, fresh) < 0) {
        pmm_free(npa); /* PTE vanished under us: keep the old mapping as-is */
        return false;
    }
    pmm_free(pte & PTE_PA_MASK); /* our reference to the shared frame */
    return true;
}

/* ---- fixer: write-protect fault that is actually COW ---- */
static bool cow_fault(struct intr_frame *f, u64 cr2)
{
    if (!(f->err & 1)) return false; /* not-present: demand/stack handled it */
    /* Only a STORE breaks copy-on-write.  err bit 1 is the write; without
     * this the NX case below would be fatal: a fetch from an NX page
     * (err = P|I = 0x11) reaches here, finds PG_W or PG_COW, "resolves"
     * the fault without changing a thing, and iretq retries the same
     * instruction into the same #PF forever -- the process hangs instead
     * of taking SIGSEGV.  Linux keeps that class out the same way, by
     * testing the access against the VMA before any fixer runs. */
    if (!(f->err & 2)) return false;
    struct task *t = current;
    if (!t) return false;
    if (cr2 < USER_BASE || cr2 >= USER_MAX_VMA) return false;

    u64 page = cr2 & ~4095ULL;
    u64 pte = vmm_user_pte(t->pml4, page);
    if (!(pte & PG_P)) return false;
    /* Already writable: the fault raced a COW break that has landed (the
     * TLB still holds the read-only view).  Claim it and flush, or the
     * retried store faults again on the same stale entry. */
    if (pte & PG_W) {
        vmm_user_pte_write(t->pml4, page, pte); /* rewrite == invlpg */
        return true;
    }
    /* Present, read-only, NO COW mark: text/.rodata.  A genuine violation
     * that must fall through to SIGSEGV -- page_cow_break() alone would
     * answer "not COW, nothing to do" and the dispatcher would treat the
     * fault as fixed, looping the faulting store forever. */
    if (!(pte & PG_COW)) return false;
    return page_cow_break(t, page);
}

bool page_fault_try_fix(struct intr_frame *f, u64 cr2)
{
    /* Dispatch: offer the fault to each fixer in order.  Every fixer
     * declares which faults it will even look at (err bit 0 above all), so
     * the first claim wins and anything unclaimed keeps the safe default:
     * report + SIGSEGV (user) or panic (kernel).
     *   1. demand_page  -- not-present inside an mmap VMA
     *   2. stack_grow   -- not-present inside the stack growth window
     *   3. cow_fault    -- write-protect fault on a PG_COW page
     * A write-protect fault on a page with PG_W clear and no PG_COW is a
     * genuine read-only violation (text/.rodata) and deliberately falls
     * through to SIGSEGV. */
    if (demand_page(f, cr2)) return true;
    if (stack_grow(f, cr2)) return true;
    return cow_fault(f, cr2);
}
