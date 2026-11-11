/* VMA list: which [start,end) ranges a task claimed with mmap(), and what
 * backs each one.
 *
 * The list is the ONLY record that a range was requested.  Nothing is
 * mapped at mmap() time -- the first access to each page goes through the
 * #PF dispatch (page_fault.c), which finds the covering VMA, checks the
 * quota and maps a frame with the VMA's protection.  So a range that is
 * never touched never costs a frame, and the VMA's `prot` is what the
 * demand pager and the COW fixer both read to decide W and NX.
 *
 * A VMA is anonymous or file-backed.  `file == NULL` means anonymous: the
 * page arrives zeroed.  Otherwise `file` names the backing regular file
 * and `file_off` is the byte offset of `start`, so the page at `va` holds
 * the file's bytes at `file_off + (va - start)`.  `flags` then decides
 * what a WRITE to that page means: LNXRM_MAP_SHARED writes are the file's
 * (one frame per (file, offset) system-wide, mm/filepage.c, written back
 * on unmap/exit/exec) and LNXRM_MAP_PRIVATE writes are the process's (a
 * frame of its own, discarded with the mapping).
 *
 * Ownership: one list per task, hanging off struct task.vmas, sorted by
 * start address.  fork deep-copies it (vma_dup, in task.c), execve drops
 * the old image's list (vma_free_all), exit's slot recycle drops whatever
 * is left (task_free_slot).  kmalloc'd nodes, single-owner, no locking:
 * the list lives inside one task and only that task's own context edits
 * it (a syscall, a #PF it took, or its fork).
 *
 * Every node that carries a `file` holds EXACTLY ONE reference on it --
 * the insert takes the caller's, every split and every fork copy takes a
 * fresh one, and erasing or freeing the node gives it back.  That is what
 * keeps the file (and therefore its cached shared frames) alive for as
 * long as any piece of the range exists, however many times the range has
 * been cut in half.
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
struct file;

struct vma {
    u64 start, end;   /* page-aligned, [start, end) */
    u32 prot;         /* LNXRM_PROT_* bits (abi/lnxrm_abi.h) */
    u32 flags;        /* LNXRM_MAP_* bits; 0 is never stored (sys_mmap
                       * normalises it to MAP_PRIVATE before inserting) */
    u64 file_off;     /* byte offset in `file` of `start` */
    struct file *file; /* NULL = anonymous; one reference when non-NULL */
    struct vma *next; /* ascending by start */
};

/* VMA covering `va`, or NULL. */
struct vma *vma_find(const struct task *t, u64 va);
/* Any VMA intersecting [start, end)?  (mmap's overlap gate.) */
bool vma_overlaps(const struct task *t, u64 start, u64 end);
/* Record a fresh range.  No overlap check by design -- the caller (sys_mmap)
 * has just checked it under the same list, so re-scanning here would only be
 * a second chance to disagree (C24).  Returns 0 or LNXRM_ENOMEM.
 *
 * `file` may be NULL (anonymous) and is otherwise a reference the CALLER
 * already owns: this function takes it over, so a failure path must drop
 * it itself. */
int vma_insert(struct task *t, u64 start, u64 end, u32 prot, u32 flags,
               u64 file_off, struct file *file);
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
 * vma_erase's split.  Split nodes inherit the file identity with the byte
 * offset moved to their own start, and take their own reference. */
int vma_protect(struct task *t, u64 start, u64 end, u32 prot);
/* Deep-copy src's list onto dst (fork).  0, or LNXRM_ENOMEM with dst's
 * list left exactly as it was.  Each copied node takes its own reference
 * on the file. */
int vma_dup(const struct task *src, struct task *dst);
/* Release the whole list (execve, slot recycle).  NULL-safe.  Drops one
 * file reference per node -- the LAST one closes the file, which is what
 * drops its cached shared frames (mm/filepage.c). */
void vma_free_all(struct task *t);

/* Write back the MAP_SHARED pages of `t` that the hardware marked dirty
 * (PG_D) inside [lo, hi), through the VMA that owns each one.  `pml4` is
 * explicit rather than taken from current because both callers run at a
 * moment when it is NOT the live root: execve writes back the OLD image's
 * ranges after CR3 already names the new one, and must do so BEFORE that
 * old root is destroyed.  Returns the number of pages written.
 *
 * Called from exactly three places -- munmap (before the range goes), exec
 * (before the old address space goes) and exit (before the fds go) -- which
 * together cover every way a mapping can disappear.  A page that was never
 * written costs no I/O: PG_D is the CPU's own dirty bit, set by the store
 * that made the page worth writing. */
u64 vma_writeback(struct task *t, u64 pml4, u64 lo, u64 hi);

/* Undo fork's copy-on-write conversion for `t`'s MAP_SHARED ranges, in
 * BOTH page tables.  dup_user_aspace() write-protects every writable page
 * it shares -- right for anonymous and private memory, wrong for shared
 * file pages: two processes writing the same file must keep writing ONE
 * frame, not two private copies of it.  Call after dup_user_aspace() and
 * vma_dup() have both succeeded.  Takes no new frame reference: the fork
 * already took one for the child, and the frame is the same one. */
void vma_fork_shared(struct task *t, u64 parent_pml4, u64 child_pml4);

#ifdef __cplusplus
}
#endif
