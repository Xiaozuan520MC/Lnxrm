/* VMA list: the [start,end) ranges a task claimed with mmap(), and what backs
 * each one.  Nothing is mapped at mmap() time -- the first access #PFs, and
 * page_fault.c finds the covering VMA, checks the quota and maps a frame with
 * the VMA's protection, so an untouched range costs no frame.  An untouched
 * `prot` is what the demand pager and the COW fixer both read for W and NX.
 *
 * Anonymous if `file == NULL` (page arrives zeroed); otherwise `file` and
 * `file_off` name the byte at `start`.  `flags` says what a WRITE means:
 * MAP_SHARED writes are the file's (one frame per (file, offset) system-wide,
 * mm/filepage.c), MAP_PRIVATE writes are the process's own.
 *
 * One kmalloc'd list per task, sorted by start, single owner and unlocked:
 * only that task's own context edits it.  fork deep-copies it (vma_dup),
 * execve drops it (vma_free_all).  A node carrying `file` holds EXACTLY ONE
 * reference, so the file lives as long as any piece of the range exists.
 *
 * The window lives in types.h: [MMAP_LO, MMAP_TOP), top-down, MMAP_TOP pinned
 * to the stack growth floor so the two can never collide. */
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
/* Record a fresh range.  No overlap check by design: sys_mmap has just checked
 * it on this same list, so re-scanning is only a second chance to disagree
 * (C24).  `file` is a reference the CALLER already owns that this takes over
 * -- a failure path must drop it itself.  Returns 0 or LNXRM_ENOMEM. */
int vma_insert(struct task *t, u64 start, u64 end, u32 prot, u32 flags,
               u64 file_off, struct file *file);
/* Drop [start, end): erase whole VMAs, cut partial ones, SPLIT one the range
 * punches through.  0 if anything intersected, LNXRM_EINVAL if nothing did,
 * LNXRM_ENOMEM if a split node could not be allocated -- nothing changed. */
int vma_erase(struct task *t, u64 start, u64 end);
/* Re-protect [start, end) with `prot`: a covered VMA takes it, a range cutting
 * into one re-cuts it (head and tail keep the old prot).  A VMA the range
 * misses is left alone -- sys_mprotect already established every page in the
 * range is present or claimed.  Returns 0, or LNXRM_ENOMEM with nothing
 * changed: split nodes are all allocated before the first prot is written,
 * exactly like vma_erase's split. */
int vma_protect(struct task *t, u64 start, u64 end, u32 prot);
/* Deep-copy src's list onto dst (fork): 0, or LNXRM_ENOMEM with dst's list
 * left exactly as it was.  Each copied node takes its own file reference. */
int vma_dup(const struct task *src, struct task *dst);
/* Release the whole list (execve, slot recycle).  NULL-safe.  Drops one file
 * reference per node -- the LAST one closes the file, which is what drops its
 * cached shared frames (mm/filepage.c). */
void vma_free_all(struct task *t);

/* Write back the MAP_SHARED pages of `t` that the hardware marked dirty (PG_D)
 * inside [lo, hi), through the VMA owning each one.  `pml4` is explicit
 * because both callers run when it is NOT the live root: execve writes back
 * the OLD image after CR3 already names the new one.  Exactly three callers --
 * munmap, exec, exit -- cover every way a mapping can disappear, each before
 * the mapping becomes unreachable.  A page never written costs no I/O. */
u64 vma_writeback(struct task *t, u64 pml4, u64 lo, u64 hi);

/* Undo fork's copy-on-write conversion for `t`'s MAP_SHARED ranges, in BOTH
 * page tables.  dup_user_aspace() write-protects every writable page it shares
 * -- right for anonymous and private memory, wrong for shared file pages: two
 * processes writing one file must keep writing ONE frame.  Call after
 * dup_user_aspace() and vma_dup() both succeeded.  Takes no new reference: the
 * fork already took one for the child, and it is the same frame. */
void vma_fork_shared(struct task *t, u64 parent_pml4, u64 child_pml4);

#ifdef __cplusplus
}
#endif
