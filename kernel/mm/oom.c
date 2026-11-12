/* T-OOM: the out-of-memory killer.
 *
 * This kernel has no reclaim and no swap (docs/GAP_ANALYSIS.md), so when the
 * machine is out of frames the only way to get any back is to hand a
 * process's memory back to the buddy -- which means ending the process that
 * holds the most of it.  Every allocation a user task can reach is also
 * gated by cred_mem_quota(), and that ceiling stops a *process* before the
 * machine runs dry; what it cannot stop is the machine running dry anyway
 * (several processes each inside their own quota, or one root task with no
 * quota at all) while somebody asks for one more page.
 *
 * That ask arrives as a #PF with no frame to give.  The fault's default
 * answer -- SIGSEGV -- is paid by whichever task happened to fault, which is
 * rarely the one that emptied the pool, and killing it frees nothing the
 * machine can hand out.  So before refusing, pick the task holding the most
 * user pages and SIGKILL it.  It dies through the ordinary exit path
 * (writeback, close, reparent, reap), its address space is destroyed when it
 * is reaped, and the frames it held are back in the buddy for whoever asks
 * next.
 *
 * What this deliberately does NOT do:
 *
 *   - trigger on a failure that can be ANSWERED.  brk answers ENOMEM, kmalloc
 *     answers NULL, a page table that cannot be built answers an error, and
 *     T-004/T-032 pin all of those as returns -- /bin/systest's brk.oom block
 *     depends on brk draining the buddy and surviving it.  A caller holding
 *     an answer has a way out (shrink, retry, report); a transient ENOMEM is
 *     not a death sentence.  Only the fault path, whose alternative is
 *     certain death, spends a life on it.
 *
 *   - kill twice for one shortage.  A kill sets a latch that only
 *     oom_frame_granted() -- called on every frame actually handed out by
 *     pmm_alloc_order() -- releases, so a fault storm cannot walk the process
 *     table one victim at a time while memory is still dry.  The kill cannot
 *     be *used* by the fault that asked for it either: the victim's frames
 *     come back when it exits and is reaped, which takes a context switch a
 *     #PF handler cannot make.  The next attempt (the signal handler's
 *     re-fault, the next brk) is what the kill is for.
 *
 *   - touch a task that has nothing to give back: idle and init hold the
 *     machine together, a kxld task IS the kernel's side of it, an embryo or
 *     a zombie is not (or no longer) a live process, one already carrying
 *     SIGKILL is on its way out and cannot be killed twice, and a task with
 *     no user pages would free nothing.
 *
 * Selection is a pure function of the task table (oom_select_victim()), so
 * the heuristic can be pinned by ktest without allocating a single frame;
 * oom_kill_largest() is the whole policy: choose, announce, SIGKILL, latch.
 */
#include <sys/sched.h>
#include <mm/mm.h>
#include <console.h>

/* Set by a kill, cleared when a frame is granted again.  Relaxed atomics:
 * the worst race is one extra or one missing kill on another CPU, never a
 * torn read, and both callers run with the same intent either way. */
static bool oom_killed;

void oom_frame_granted(void)
{
    __atomic_store_n(&oom_killed, false, __ATOMIC_RELAXED);
}

/* Would killing `t` hand frames back?  Every "no" is a task the system
 * cannot afford to lose or one that would lose nothing. */
static bool oom_killable(const struct task *t)
{
    if (t->pid <= 1) return false;                 /* idle (0) and init (1) */
    if (t->cred.kind == CRED_KXLD) return false;   /* kernel threads */
    if (t->state == T_EMBRYO || t->state == T_ZOMBIE) return false;
    if (t->signal_pending & (1ULL << SIGKILL)) return false; /* already dying */
    if (t->mem_pages == 0) return false;           /* nothing to reclaim */
    return true;
}

/* The victim: whoever holds the most user pages.  Ties go to the first slot
 * the scan meets, so the choice is a function of the table and never of
 * timing.  `current` is a candidate like any other -- if the faulting task
 * is the hog, it is exactly the task that should die, and SIGKILL only makes
 * the death its own fault was already going to deliver uncatchable. */
struct task *oom_select_victim(void)
{
    struct task *best = NULL;
    int i = 0;
    for (struct task *t = task_iter(&i); t; t = task_iter(&i)) {
        if (!oom_killable(t)) continue;
        if (!best || t->mem_pages > best->mem_pages) best = t;
    }
    return best;
}

/* The trigger, called where a user fault could not get a frame.  Returns
 * true when a victim was signalled (the caller still refuses the fault: the
 * frames are not back yet), false when nothing could be done about it.
 *
 * The scan and the signal are not one critical section: between them the
 * victim may exit and its slot be recycled, which is exactly the window
 * sys_kill() already has (send_signal() rechecks T_UNUSED, and a slot
 * recycled in between takes the signal -- the price of not holding the
 * task-table lock across a wakeup that may enqueue). */
bool oom_kill_largest(void)
{
    if (__atomic_load_n(&oom_killed, __ATOMIC_RELAXED)) return false;

    struct task *v = oom_select_victim();
    if (!v) return false;

    __atomic_store_n(&oom_killed, true, __ATOMIC_RELAXED);
    kprintf("[oom] frame allocation failed: killing pid %u (%s), %lu pages\n",
            (unsigned)v->pid, v->name, v->mem_pages);
    send_signal(v, SIGKILL);
    return true;
}
