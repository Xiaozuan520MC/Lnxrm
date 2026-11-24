// SPDX-License-Identifier: Apache-2.0

/* T-090/T-094/T-MMAP: page-fault dispatch -- demand paging, lazy stack
 * growth and copy-on-write in one seam.  isr_common() used to treat every #PF
 * as fatal, which made a deepening user stack lethal (sys_execve maps only
 * 16 KiB of stack); the dispatch order lives in page_fault_try_fix(). */
#include <sys/sched.h>
#include <mm/mm.h>
#include <mm/vma.h>
#include <mm/filepage.h>
#include <mm/pagefault.h>
#include <sys/vfs.h>
#include <console.h>

/* How far the stack may grow below its initial mapping.  Below the growth
 * floor lie STACK_GUARD bytes nobody maps (types.h: MMAP_TOP stops one guard
 * below it), then the mmap window -- heap, mmap, guard and stack approach
 * each other and never overlap. */
#define USTACK_MAX_GROW 0x80000ULL /* 512 KiB */

/* How far below the running user RSP a fault may sit and still count as a
 * stack access worth growing for: the SysV red zone and a prologue's frame put
 * every legitimate stack fault at or above RSP minus this window, so a wild
 * store aimed far below dies as SIGSEGV.  Equal to STACK_GUARD (types.h), so
 * the deepest accepted fault always lands inside the guard and can never reach
 * an mmap VMA. */
#define STACK_GROW_WINDOW 0x10000ULL /* 64 KiB */

LNXRM_STATIC_ASSERT(USER_STACK_TOP - MMAP_TOP == USTACK_MAX_GROW + STACK_GROW_WINDOW,
                    "STACK_GUARD must equal STACK_GROW_WINDOW: the mmap window "
                    "stops one guard below the stack floor");

/* The window must sit INSIDE user space: below USER_BASE a mmap could never
 * be mapped (vmm_map_user refuses) and a mprotect over it would be a bogus
 * EINVAL -- one wrong digit in the 64 GiB shift did exactly that. */
LNXRM_STATIC_ASSERT(MMAP_LO >= USER_BASE,
                    "mmap window must lie inside [USER_BASE, MMAP_TOP)");
LNXRM_STATIC_ASSERT(MMAP_TOP <= USER_STACK_TOP - USTACK_MAX_GROW,
                    "mmap window must stop at or below the stack growth floor");

/* Resolve one user-mode #PF by growing the stack so it covers `page`;
 * returns true if the fault is now fixed.  Declines anything outside
 * [USER_STACK_TOP-USTACK_MAX_GROW, ustack_lo), anything further than
 * STACK_GROW_WINDOW below the user RSP, or anything past cred_mem_quota(),
 * so a runaway recursion dies as SIGSEGV instead of eating the machine. */
static bool stack_grow(struct intr_frame *f, u64 cr2)
{
    /* A protection fault (P=1) is a real access violation growth must not
     * paper over -- only a NOT-PRESENT fault is a candidate. */
    if (f->err & 1) return false;

    /* A FETCH is never a stack access: every user page but the sigreturn
     * restorer is NX, so growing for one would hand a stray jump a fresh
     * frame and fault again on its first instruction. */
    if (f->err & 0x10) return false;

    struct task *t = current;
    if (!t || t->ustack_lo == 0) return false; /* no tracked stack (idle/kernel) */

    if (cr2 < USER_BASE || cr2 >= USER_MAX_VMA) return false;

    u64 page = cr2 & ~4095ULL;
    if (page >= USER_STACK_TOP) return false;          /* not below the stack top */
    if (page < USER_STACK_TOP - USTACK_MAX_GROW) return false; /* too far: overflow/wild */
    if (page >= t->ustack_lo) return false;            /* already mapped (shouldn't fault) */

    /* Must look like a STACK access: no further than STACK_GROW_WINDOW below
     * the running user RSP (f->ussp; a frame with no user RSP -- ktest builds
     * its own -- has none, so the check is skipped for it).  A deepening
     * recursion drags RSP down and passes; a store aimed megabytes below is
     * refused before a single frame is taken. */
    u64 rsp = f->ussp;
    if ((f->cs & 3) == 3 && rsp >= USER_BASE && rsp < USER_MAX_VMA &&
        cr2 + STACK_GROW_WINDOW < rsp)
        return false;

    /* Page-aligned at both ends, so this is exact. */
    u64 npages = (t->ustack_lo - page) / PAGE_SIZE;

    /* Refuse BEFORE taking any frame: cred_mem_quota() caps a plain user's
     * whole address space, the same ceiling sys_brk honours, so stack growth
     * cannot sidestep the limit. */
    u64 quota = cred_mem_quota(&t->cred);
    if (quota && t->mem_pages + npages > quota) return false;

    /* Map page by page down to `page`, each zeroed (a fresh stack frame must
     * not leak the previous occupant's bytes) and installed NX+user+writable:
     * the stack is data, so a stray jump into it faults. */
    u64 va = t->ustack_lo;
    while (va > page) {
        va -= PAGE_SIZE;
        u64 pa = pmm_alloc();
        if (!pa) {
            /* Out of frames part-way: whatever was mapped stays mapped and
             * accounted, the faulting page stays missing, so report failure and
             * let the caller turn it into SIGSEGV.  Before that -- the machine
             * is dry, so somebody else's memory gives it back (mm/oom.c). */
            oom_kill_largest();
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

/* ---- fixer: demand paging for mmap'd ranges (T-MMAP, T-FILE) ----
 *
 * mmap() only records a VMA (mm/vma.c); the frame arrives here, on first
 * touch.  What fills it is the VMA's business: zeros for an anonymous range,
 * the file's bytes for a file-backed one, and the SHARED frame of that file
 * page under MAP_SHARED -- so two processes mapping the same file at the same
 * offset get the same physical page.  Either way it is mapped with the VMA's
 * protection and metered through cred_mem_quota(), the same ceiling brk and
 * stack growth honour, so mmap cannot turn the cap into a suggestion.  An
 * untouched range costs one VMA node and no frame. */
static bool demand_page(struct intr_frame *f, u64 cr2)
{
    if (f->err & 1) return false; /* present fault: not a missing page */
    struct task *t = current;
    if (!t) return false;
    if (cr2 < USER_BASE || cr2 >= USER_MAX_VMA) return false;

    u64 page = cr2 & ~4095ULL;

    /* The stack guard (types.h) belongs to nobody: the mmap window stops below
     * it, so no VMA can cover it and an overflow can never claim to be an mmap
     * touch. */
    if (page >= MMAP_TOP && page < USER_STACK_TOP - USTACK_MAX_GROW) return false;

    struct vma *v = vma_find(t, page);
    if (!v) return false; /* no mmap claims it -> stack_grow or kill */

    /* The VMA is asked FIRST, before a single frame is taken -- the order
     * Linux settles faults in (access_error() ahead of handle_mm_fault()).
     * A store into a PROT_READ region or a fetch without PROT_EXEC will be
     * killed, so allocating first would only hand the doomed process a frame.
     * Reads are never refused (no PKU), so PROT_NONE degrades to read-only. */
    if ((f->err & 2) && !(v->prot & LNXRM_PROT_WRITE)) return false;
    if ((f->err & 0x10) && !(v->prot & LNXRM_PROT_EXEC)) return false;

    /* Already there?  The dispatcher is also called directly (ktest) and a
     * stale-TLB retry could bring the same cr2 back twice -- without this
     * guard the second pass would allocate and leak a frame plus a
     * mem_pages increment. */
    if (vmm_user_pte(t->pml4, page)) return false;

    u64 quota = cred_mem_quota(&t->cred);
    if (quota && t->mem_pages + 1 > quota) return false;

    u64 pa;
    if (v->file) {
        /* File-backed: the bytes come from the file at the offset this VMA
         * names, not from the zero page.  MAP_SHARED asks the cache for THE
         * frame of that (file, offset) -- one frame per file page system-wide,
         * which is what makes one process's store visible to the next mapping
         * and, at write-back, to the file.  MAP_PRIVATE gets a frame of its
         * own, since private bytes belong to the process and fork's COW needs
         * something to copy. */
        u64 off = v->file_off + (page - v->start);
        if (v->flags & LNXRM_MAP_SHARED) {
            pa = filepage_get(v->file, off);
            if (!pa) {
                /* 0 means no frame came back: a dry buddy, a failed read or a
                 * cache node that could not be allocated.  Only the first is a
                 * shortage to kill over (mm/oom.c). */
                if (!pmm_free_bytes()) oom_kill_largest();
                return false;
            }
        } else {
            pa = pmm_alloc();
            if (!pa) {
                /* Dry buddy: have the memory hog pay for the machine
                 * (mm/oom.c) before this fault becomes SIGSEGV. */
                oom_kill_largest();
                return false;
            }
            memset((void *)PHYS_TO_VIRT(pa), 0, PAGE_SIZE);
            /* A SHORT read is normal: the file's last page is usually partial
             * and the frame was zeroed first, so the tail reads as zeros
             * exactly as the file's own bytes past EOF do.  A failed read
             * (negative) is not a page -- hand the frame back rather than map
             * zeros that pretend to be the file's contents. */
            if (vfs_pread(v->file, (void *)PHYS_TO_VIRT(pa), PAGE_SIZE, off) < 0) {
                pmm_free(pa);
                return false;
            }
        }
    } else {
        pa = pmm_alloc();
        if (!pa) {
            /* The common case, and a dry buddy here is a real shortage: kill
             * the biggest holder first (mm/oom.c). */
            oom_kill_largest();
            return false;
        }
        memset((void *)PHYS_TO_VIRT(pa), 0, PAGE_SIZE);
    }
    /* PROT_READ is implicit (no PKU), so PROT_NONE degrades to read-only; W and
     * NX are exactly what the VMA asked for.  A shared file page gets W straight
     * from here too -- it is not a COW candidate (vma_fork_shared keeps it
     * writable across fork), so the next write must not trap. */
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
 * Break the sharing on `page` for address space `t`: clone the shared frame,
 * take it writable, clear PG_COW, then hand back THIS side's reference to the
 * old frame (pmm_free drops the count; the other sharer's keeps it alive until
 * it breaks or unmaps too).
 *
 * Ordering makes this race-free when two processes break the same frame on two
 * CPUs: copy FIRST, swap the PTE SECOND, drop our reference LAST.  Our
 * reference alone pins the old frame's bytes until our own copy is done, and
 * the count reaches 0 only after every sharer has copied -- so no copy can read
 * a frame the buddy already gave away.
 *
 * Also the routine syscalls need BEFORE they store through a user pointer
 * (task.c user_ptr_writable): after fork a writable page is PTE-read-only on
 * both sides, and copy_to_user's gate would otherwise reject a legitimate
 * write into the child's own memory.  Returns true when the page no longer
 * blocks a write (never COW, or the break succeeded); false when it is not
 * present or the machine is out of frames (the page stays COW, the caller
 * reports its own error). */
bool page_cow_break(struct task *t, u64 page)
{
    if (!t) return false;
    u64 pte = vmm_user_pte(t->pml4, page);
    if (!(pte & PG_P)) return false;
    if (!(pte & PG_COW)) return true; /* not COW: nothing to resolve here */

    u64 npa = pmm_alloc();
    if (!npa) {
        /* Same shortage, same answer as the other fixers: the hog pays, this
         * fault still fails and stays copy-on-write (mm/oom.c). */
        oom_kill_largest();
        return false;
    }
    /* Read through the high-half alias, never through `page`: that resolves
     * against the LIVE cr3 (possibly another address space) and a user page
     * would trip SMAP with no STAC window. */
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
    /* Only a STORE breaks copy-on-write (err bit 1).  Without this a fetch
     * from an NX page (err = P|I = 0x11) would "resolve" the fault without
     * changing a thing and iretq would retry the same instruction into the
     * same #PF forever.  Linux keeps that class out the same way: test the
     * access against the VMA before any fixer runs. */
    if (!(f->err & 2)) return false;
    struct task *t = current;
    if (!t) return false;
    if (cr2 < USER_BASE || cr2 >= USER_MAX_VMA) return false;

    u64 page = cr2 & ~4095ULL;
    u64 pte = vmm_user_pte(t->pml4, page);
    if (!(pte & PG_P)) return false;
    /* Already writable: the fault raced a COW break that has landed (the TLB
     * still holds the read-only view).  Claim it and flush, or the retried
     * store faults again on the stale entry. */
    if (pte & PG_W) {
        vmm_user_pte_write(t->pml4, page, pte); /* rewrite == invlpg */
        return true;
    }
    /* Present, read-only, NO COW mark: text/.rodata.  A genuine violation that
     * must fall through -- page_cow_break() alone would answer "not COW" and
     * the dispatcher would treat the fault as fixed, looping the store
     * forever. */
    if (!(pte & PG_COW)) return false;
    return page_cow_break(t, page);
}

bool page_fault_try_fix(struct intr_frame *f, u64 cr2)
{
    /* Dispatch: offer the fault to each fixer in order.  Every fixer declares
     * which faults it will even look at (err bit 0 above all), so the first
     * claim wins and anything unclaimed keeps the safe default: report +
     * SIGSEGV (user) or panic (kernel).
     *   1. demand_page  -- not-present inside an mmap VMA
     *   2. stack_grow   -- not-present inside the stack growth window
     *   3. cow_fault    -- write-protect fault on a PG_COW page */
    if (demand_page(f, cr2)) return true;
    if (stack_grow(f, cr2)) return true;
    return cow_fault(f, cr2);
}
