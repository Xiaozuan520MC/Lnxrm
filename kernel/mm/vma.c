/* VMA list -- see include/mm/vma.h for the contract.
 * Sorted singly-linked list, kmalloc'd nodes, one list per task, and every
 * node that names a file owns exactly one reference on it. */
#include <mm/vma.h>
#include <mm/mm.h>
#include <sys/sched.h>
#include <sys/vfs.h>

/* Give a node that will start at `new_start` the file identity of `src`,
 * with the byte offset moved to follow its own start -- a VMA split in
 * half must not leave both halves claiming the same file bytes -- and take
 * the reference every file-carrying node owes.  `new_start` is always
 * >= src->start (splits only ever move a start forward), so the offset
 * arithmetic cannot wrap. */
static void vma_file_inherit(struct vma *dst, const struct vma *src, u64 new_start)
{
    dst->flags = src->flags;
    dst->file_off = src->file_off + (new_start - src->start);
    dst->file = src->file;
    if (dst->file) __sync_fetch_and_add(&dst->file->refcnt, 1);
}

/* The node is going away: hand back the reference it held.  NULL-safe, so
 * every teardown path can call it unconditionally. */
static void vma_file_drop(const struct vma *v)
{
    if (v->file) vfs_close_file(v->file);
}

struct vma *vma_find(const struct task *t, u64 va)
{
    if (!t) return NULL;
    for (struct vma *v = t->vmas; v && v->start <= va; v = v->next)
        if (va < v->end) return v;
    return NULL;
}

bool vma_overlaps(const struct task *t, u64 start, u64 end)
{
    if (!t || start >= end) return false;
    /* list is sorted: stop once the next VMA starts at or past the end */
    for (struct vma *v = t->vmas; v && v->start < end; v = v->next)
        if (v->end > start) return true;
    return false;
}

int vma_insert(struct task *t, u64 start, u64 end, u32 prot, u32 flags,
               u64 file_off, struct file *file)
{
    if (!t || start >= end) return LNXRM_EINVAL;
    struct vma *v = kmalloc(sizeof(*v));
    if (!v) return LNXRM_ENOMEM;
    v->start = start;
    v->end = end;
    v->prot = prot;
    v->flags = flags;
    v->file_off = file_off;
    v->file = file; /* takes over the caller's reference (see vma.h) */

    /* insert in start order (ascending), keeping `head` on the task */
    struct vma **link = &t->vmas;
    while (*link && (*link)->start < start) link = &(*link)->next;
    v->next = *link;
    *link = v;
    return 0;
}

/* Replace [vs,ve) with at most one node -- the TAIL of an existing VMA
 * left after a range erased its head, or the HEAD left after a tail erase.
 * `t` and `v` are fixed; on allocation failure the caller has not touched
 * anything yet (the caller checks the return before unmapping pages), so
 * a failed re-cut leaves the original VMA intact -- including its file
 * reference, which is only ever taken once the node exists. */
static int vma_recut(struct task *t, struct vma *v, u64 vs, u64 ve)
{
    struct vma *n = kmalloc(sizeof(*n));
    if (!n) return LNXRM_ENOMEM;
    n->start = vs;
    n->end = ve;
    n->prot = v->prot;
    vma_file_inherit(n, v, vs);
    n->next = v->next;
    v->next = n;
    return 0;
}

int vma_erase(struct task *t, u64 start, u64 end)
{
    if (!t || start >= end) return LNXRM_EINVAL;
    bool hit = false;
    struct vma **link = &t->vmas;

    while (*link) {
        struct vma *v = *link;
        if (v->start >= end) break;        /* sorted: nothing past here intersects */
        if (v->end <= start) { link = &v->next; continue; }
        hit = true;

        if (v->start >= start && v->end <= end) {
            /* whole VMA swallowed */
            *link = v->next;
            vma_file_drop(v);
            kfree(v);
            continue; /* `link` already points at the next node */
        }
        if (v->start >= start) {
            /* range cuts off the HEAD: [start, end) hits v->start .. < v->end.
             * The survivor starts at `end`, so its file offset moves with it
             * BEFORE the start is rewritten (it is measured from the old one). */
            v->file_off += end - v->start;
            v->start = end;
            return 0;
        }
        if (v->end <= end) {
            /* range cuts off the TAIL: v ends inside the range.  The
             * survivor keeps v's own start, so its offset does not move. */
            v->end = start;
            link = &v->next;
            continue;
        }
        /* range punches a hole through the middle: shrink v to the head
         * [v->start, start) and add the surviving tail [end, v->end) */
        u64 tail_lo = end, tail_hi = v->end;
        v->end = start;
        if (vma_recut(t, v, tail_lo, tail_hi) < 0) return LNXRM_ENOMEM;
        return 0;
    }
    return hit ? 0 : LNXRM_EINVAL;
}

/* Re-protect [start, end): count the re-cuts first, allocate every node
 * they need, and only then write a single prot -- so ENOMEM leaves the list
 * byte-for-byte as it was (the same all-or-nothing rule as vma_erase's
 * split).  Per intersecting VMA the shapes are: fully covered (0 new
 * nodes), head taken (1), tail taken (1), punched through the middle (2).
 * A new node inherits the file identity with its own start, which is what
 * keeps a split range's byte offsets right and its file reference counted
 * once per node. */
int vma_protect(struct task *t, u64 start, u64 end, u32 prot)
{
    if (!t || start >= end) return LNXRM_EINVAL;

    int need = 0;
    for (const struct vma *v = t->vmas; v && v->start < end; v = v->next) {
        if (v->end <= start) continue;                 /* stops before the range */
        if (v->start >= start && v->end <= end) continue; /* whole: re-prot in place */
        if (v->start < start && v->end > end) { need += 2; continue; } /* hole */
        need += 1;                                      /* head or tail */
    }

    struct vma *pool = NULL;
    while (need-- > 0) {
        struct vma *n = kmalloc(sizeof(*n));
        if (!n) {
            while (pool) { struct vma *x = pool->next; kfree(pool); pool = x; }
            return LNXRM_ENOMEM;
        }
        n->next = pool;
        pool = n;
    }

    struct vma **link = &t->vmas;
    while (*link) {
        struct vma *v = *link;
        if (v->start >= end) break;                    /* sorted: past the range */
        if (v->end <= start) { link = &v->next; continue; }
        u32 old = v->prot;

        if (v->start >= start && v->end <= end) {
            v->prot = prot;
            link = &v->next;
        } else if (v->start < start && v->end > end) {
            /* punched through: [v->start,start) old, [start,end) new,
             * [end,v->end) old -- two nodes out of the pre-charged pool,
             * which the count pass above guaranteed (same traversal). */
            struct vma *mid = pool; pool = pool->next;
            struct vma *tail = pool; pool = pool->next;
            mid->start = start; mid->end = end; mid->prot = prot;
            tail->start = end; tail->end = v->end; tail->prot = old;
            vma_file_inherit(mid, v, mid->start);
            vma_file_inherit(tail, v, tail->start);
            tail->next = v->next;
            mid->next = tail;
            v->end = start;
            v->next = mid;
            link = &tail->next;
        } else if (v->start >= start) {
            /* range takes the HEAD: v becomes [v->start, end) with the new
             * prot (its own start, so its file offset stays put), the old
             * tail survives as [end, v->end) */
            struct vma *tail = pool; pool = pool->next;
            tail->start = end; tail->end = v->end; tail->prot = old;
            vma_file_inherit(tail, v, tail->start);
            tail->next = v->next;
            v->end = end;
            v->prot = prot;
            v->next = tail;
            link = &tail->next;
        } else {
            /* range takes the TAIL: v keeps [v->start, start) with the old
             * prot, the covered part becomes [start, v->end) */
            struct vma *part = pool; pool = pool->next;
            part->start = start; part->end = v->end; part->prot = prot;
            vma_file_inherit(part, v, part->start);
            part->next = v->next;
            v->end = start;
            v->next = part;
            link = &part->next;
        }
    }
    return 0;
}

int vma_dup(const struct task *src, struct task *dst)
{
    dst->vmas = NULL;
    if (!src) return 0;

    struct vma **link = &dst->vmas;
    for (const struct vma *s = src->vmas; s; s = s->next) {
        struct vma *n = kmalloc(sizeof(*n));
        if (!n) {
            vma_free_all(dst);
            return LNXRM_ENOMEM;
        }
        *n = *s;
        n->next = NULL;
        if (n->file) __sync_fetch_and_add(&n->file->refcnt, 1);
        *link = n;
        link = &n->next;
    }
    return 0;
}

void vma_free_all(struct task *t)
{
    if (!t) return;
    struct vma *v = t->vmas;
    t->vmas = NULL;
    while (v) {
        struct vma *n = v->next;
        vma_file_drop(v);
        kfree(v);
        v = n;
    }
}

/* ---- MAP_SHARED write-back (mm/filepage.h has the frame side of this) -- */
u64 vma_writeback(struct task *t, u64 pml4, u64 lo, u64 hi)
{
    if (!t || !pml4 || lo >= hi) return 0;
    u64 npages = 0;

    for (struct vma *v = t->vmas; v; v = v->next) {
        if (v->start >= hi) break;        /* sorted: nothing past the range */
        if (v->end <= lo) continue;       /* stops before the range */
        if (!v->file || !(v->flags & LNXRM_MAP_SHARED)) continue;

        u64 from = v->start > lo ? v->start : lo;
        u64 to = v->end < hi ? v->end : hi;
        /* The file may not reach the end of the range: a mapping is
         * rounded up to whole pages, so its last page is usually PARTIAL,
         * and writing the whole frame would grow the file by up to 4095
         * bytes of zeros nobody ever asked for.  The size is read fresh
         * from the filesystem -- vfs_file_size()'s open-time snapshot would
         * still say 0 for a file this process created and wrote, and every
         * page of it would then be skipped as "past the end". */
        u64 fsz = vfs_file_size_now(v->file);

        for (u64 va = from; va < to; va += PAGE_SIZE) {
            u64 pte = vmm_user_pte(pml4, va);
            if (!pte || !(pte & PG_P)) continue; /* never faulted in */
            if (!(pte & PG_D)) continue;         /* read, never written: no I/O */

            u64 off = v->file_off + (va - v->start);
            if (off >= fsz) continue;            /* the file ends before this page */
            size_t n = PAGE_SIZE;
            if (off + n > fsz) n = (size_t)(fsz - off);

            u64 pa = pte & PTE_PA_MASK;
            long r = vfs_pwrite(v->file, (const void *)PHYS_TO_VIRT(pa), n, off);
            if (r != (long)n) continue; /* short or failed: PG_D stays set for the next attempt */

            /* Clear the dirty bit only now: the bytes are on their way, and
             * a page left dirty by a failed write must be retried rather
             * than be believed clean at the next exit. */
            vmm_user_pte_write(pml4, va, pte & ~PG_D);
            npages++;
        }
    }
    return npages;
}

/* ---- fork: keep MAP_SHARED frames shared (see vma.h) ---- */
void vma_fork_shared(struct task *t, u64 parent_pml4, u64 child_pml4)
{
    if (!t || !parent_pml4 || !child_pml4) return;

    for (struct vma *v = t->vmas; v; v = v->next) {
        if (!v->file || !(v->flags & LNXRM_MAP_SHARED)) continue;
        for (u64 va = v->start; va < v->end; va += PAGE_SIZE) {
            /* Only pages that exist can have been converted: an untouched
             * page faults in later through the demand pager, which asks the
             * filepage cache for THE frame and never sees a COW mark. */
            u64 cp = vmm_user_pte(child_pml4, va);
            if (!cp || !(cp & PG_P) || !(cp & PG_COW)) continue;
            u64 pp = vmm_user_pte(parent_pml4, va);
            /* Restore each side from its OWN PTE so NX, dirty and any other
             * flag ride along untouched; only W comes back and COW goes. */
            if (pp & PG_P)
                vmm_user_pte_write(parent_pml4, va, (pp | PG_W) & ~PG_COW);
            vmm_user_pte_write(child_pml4, va, (cp | PG_W) & ~PG_COW);
            /* No pmm_ref here: dup_user_aspace() already took the child's
             * reference to this very frame before it converted it. */
        }
    }
}
