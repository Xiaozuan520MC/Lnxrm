/* Anonymous VMA list -- see include/mm/vma.h for the contract.
 * Sorted singly-linked list, kmalloc'd nodes, one list per task. */
#include <mm/vma.h>
#include <mm/mm.h>
#include <sys/sched.h>

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

int vma_insert(struct task *t, u64 start, u64 end, u32 prot)
{
    if (!t || start >= end) return LNXRM_EINVAL;
    struct vma *v = kmalloc(sizeof(*v));
    if (!v) return LNXRM_ENOMEM;
    v->start = start;
    v->end = end;
    v->prot = prot;

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
 * a failed re-cut leaves the original VMA intact. */
static int vma_recut(struct task *t, struct vma *v, u64 vs, u64 ve)
{
    struct vma *n = kmalloc(sizeof(*n));
    if (!n) return LNXRM_ENOMEM;
    n->start = vs;
    n->end = ve;
    n->prot = v->prot;
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
            kfree(v);
            continue; /* `link` already points at the next node */
        }
        if (v->start >= start) {
            /* range cuts off the HEAD: [start, end) hits v->start .. < v->end */
            v->start = end;
            return 0;
        }
        if (v->end <= end) {
            /* range cuts off the TAIL: v ends inside the range */
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
 * nodes), head taken (1), tail taken (1), punched through the middle (2). */
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
            tail->next = v->next;
            mid->next = tail;
            v->end = start;
            v->next = mid;
            link = &tail->next;
        } else if (v->start >= start) {
            /* range takes the HEAD: v becomes [v->start, end) with the new
             * prot, the old tail survives as [end, v->end) */
            struct vma *tail = pool; pool = pool->next;
            tail->start = end; tail->end = v->end; tail->prot = old;
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
        kfree(v);
        v = n;
    }
}
