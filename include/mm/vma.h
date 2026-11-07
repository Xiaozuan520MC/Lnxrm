/* Anonymous VMA list: which [start,end) ranges a task claimed with mmap().
 *
 * The list is the ONLY record that a range was requested.  Nothing is
 * mapped at mmap() time -- the first access to each page goes through the
 * #PF dispatch (page_fault.c), which finds the covering VMA, checks the
 * quota and maps a zeroed frame with the VMA's protection.  So a range
 * that is never touched never costs a frame, and the VMA's `prot` is what
 * the demand pager and the COW fixer both read to decide W and NX.
 *
 * Ownership: one list per task, hanging off struct task.vmas, sorted by
 * start address.  fork deep-copies it (vma_dup, in task.c), execve drops
 * the old image's list (vma_free_all), exit's slot recycle drops whatever
 * is left (task_free_slot).  kmalloc'd nodes, single-owner, no locking:
 * the list lives inside one task and only that task's own context edits
 * it (a syscall, a #PF it took, or its fork).
 *
 * The window itself lives in types.h: [MMAP_LO, MMAP_TOP), top-down, with
 * MMAP_TOP pinned to the stack growth floor so the two can never collide.
 */
#pragma once
#include <types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct task;

struct vma {
    u64 start, end;   /* page-aligned, [start, end) */
    u32 prot;         /* LNXRM_PROT_* bits (abi/lnxrm_abi.h) */
    struct vma *next; /* ascending by start */
};

/* VMA covering `va`, or NULL. */
struct vma *vma_find(const struct task *t, u64 va);
/* Any VMA intersecting [start, end)?  (mmap's overlap gate.) */
bool vma_overlaps(const struct task *t, u64 start, u64 end);
/* Record a fresh range.  No overlap check by design -- the caller (sys_mmap)
 * has just checked it under the same list, so re-scanning here would only be
 * a second chance to disagree (C24).  Returns 0 or LNXRM_ENOMEM. */
int vma_insert(struct task *t, u64 start, u64 end, u32 prot);
/* Drop [start, end) from the list: erases whole VMAs, cuts partial ones at
 * both edges, and SPLITS a VMA that the range punches through.  Returns 0
 * if anything intersected, LNXRM_EINVAL if nothing did (sys_munmap's
 * answer to unmapping a range that was never mapped) and LNXRM_ENOMEM if
 * the split node could not be allocated -- in which case NOTHING changed. */
int vma_erase(struct task *t, u64 start, u64 end);
/* Re-protect [start, end) with `prot`: a fully covered VMA takes it, a
 * range that cuts into one re-cuts it (head and tail keep the old prot,
 * the middle takes the new; a range punching straight through produces
 * three pieces).  A VMA the range misses is left alone -- sys_mprotect has
 * already established that every page in the range is present or claimed,
 * so "misses" means the pages are mapped outright (text, heap, stack) and
 * live in the page tables instead.  Returns 0, or LNXRM_ENOMEM when a
 * split node could not be allocated -- in which case NOTHING changed: the
 * nodes are all allocated before the first prot is written, exactly like
 * vma_erase's split. */
int vma_protect(struct task *t, u64 start, u64 end, u32 prot);
/* Deep-copy src's list onto dst (fork).  0, or LNXRM_ENOMEM with dst's
 * list left exactly as it was. */
int vma_dup(const struct task *src, struct task *dst);
/* Release the whole list (execve, slot recycle).  NULL-safe. */
void vma_free_all(struct task *t);

#ifdef __cplusplus
}
#endif
