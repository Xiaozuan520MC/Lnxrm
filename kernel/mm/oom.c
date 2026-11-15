/* T-OOM: the out-of-memory killer.
 *
 * No reclaim, no swap (docs/GAP_ANALYSIS.md): the only way back from a real
 * shortage is to end the process holding the most user pages.
 * cred_mem_quota() stops a *process* before the machine runs dry, but not
 * several processes each inside their own quota (or one root task with no
 * quota).  That ask arrives as a #PF with no frame to give, and the default
 * SIGSEGV would be paid by whichever task happened to fault -- rarely the one
 * that emptied the pool -- so pick the largest holder and SIGKILL it.  It dies
 * through the ordinary exit path and its frames return when it is reaped.
 *
 * Deliberately NOT:
 *
 *   - triggering on a failure that can be ANSWERED (brk's ENOMEM, kmalloc's
 *     NULL, an unbuildable page table -- T-004/T-032 pin all of those as
 *     returns).  A caller with an answer has a way out; only the fault path,
 *     whose alternative is certain death, spends a life on it.
 *
 *   - killing twice for one shortage: a kill latches until
 *     oom_frame_granted() sees a frame handed out, so a fault storm cannot
 *     walk the process table one victim at a time.  The victim's frames only
 *     return when it is reaped -- a context switch a #PF handler cannot make
 *     -- so the kill is for the NEXT attempt.
 *
 *   - touching a task with nothing to give back: idle and init hold the
 *     machine together, a kxld task IS the kernel's side of it, an embryo or
 *     zombie is not live, one already carrying SIGKILL cannot be killed twice.
 *
 * Selection is a pure function of the task table (oom_select_victim()), so
 * ktest pins the heuristic without allocating a frame; oom_kill_largest() is
 * the whole policy: choose, announce, SIGKILL, latch. */
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
