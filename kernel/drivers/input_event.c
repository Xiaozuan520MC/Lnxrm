/* input_event.c: the unified input event queue, syscalls 39-40.
 *
 * One 63-slot stream wrapping every producer the machine has -- the IRQ1
 * keyboard decode path and the PS/2 aux packet tail -- so a compositor
 * pops input_pop (39) instead of touching hardware rings per device.  The
 * existing rings stay exactly what they are (console inbuf for the shell,
 * msring for mouse_get), fed by the same code as ever: this queue is an
 * overlay consumer of their producers, not a rerouting.
 *
 * What a producer means here:
 *  - kbd_process_sc (isr.c): one KEY per decoded make-code, carrying the
 *    shift/capslock state as flags.  Break codes and shift/capslock
 *    themselves produce nothing, and the kbd_inject 35 path types console
 *    bytes only -- synthetic terminal input is not a keystroke.
 *  - ms_feed_byte (ps2mouse.c): one packet becomes at most three events,
 *    in the order BTN, REL, WHEEL: a BTN only when the button mask
 *    changed (a packet that repeats a held button is not a click), a REL
 *    only when the packet moved, a WHEEL only when it scrolled.  A packet
 *    with zero deltas, no wheel and no change is silence.
 *
 * Overruns drop the NEW event and count it in `dropped` -- the same
 * honest policy as both rings it rides beside (a queue full to a producer
 * means input nobody read).  Everything is non-blocking: pop answers
 * -1 (the syscall turns that into EAGAIN) and push never sleeps.  Callers
 * that want to block instead sit on the queue's private wait list
 * (waiters link themselves under evq_lock); `input_push_*` evicts one
 * waiter on each accepted event and wakes it AFTER dropping evq_lock,
 * so wakeups never nest runq_lock inside evq_lock.  The waiter rechecks
 * events / signals / timeout on wake and never dead-locks: events remain
 * in the queue until the woken caller pops them.  Serial RX bytes are
 * console-only for now -- folding them in is too. */
#include <types.h>
#include <sys/spinlock.h>
#include <sys/input.h>
#include <sys/sched.h>
#include <sys/cpu.h>

#define IEVQ_SZ 64 /* 63 usable, the kbd/ms full-at-(w+1)==r convention */
static struct lnxrm_input_event evq[IEVQ_SZ];
static volatile u32 ev_r, ev_w;
static volatile u32 ev_pushed, ev_dropped;
static volatile u32 ev_keys, ev_rels, ev_btns, ev_wheels;
static spinlock_t evq_lock = SPINLOCK_INIT;

/* Queue-private state, both owned by the lock: seq gives every accepted
 * event its strictly-increasing id, and prev_buttons is the last mouse
 * packet's mask, the baseline BTN changes are measured against. */
static u32 ev_seq;
static u32 ev_prev_buttons;

/* Wait list for input_wait(41): park-first singly linked list threaded
 * through task->wait_next, head-only.  Owned by evq_lock.  Producers add
 * no waiter; consumers enqueue themselves and park under the same lock
 * that producers drain, so an event cannot arrive between a consumer's
 * "queue empty" check and its park (no lost wakeup).  A producer evicts
 * exactly ONE waiter per accepted event -- the event itself stays in the
 * queue until the woken caller pops it, so an empty re-check loops back
 * to re-park or to the signal/timeout verdict instead of dead-locking. */
static struct task *ev_waiters;

/* One event takes a slot; the callers hold the lock, so this decides
 * only what a *full* queue means: the event that arrived last is the one
 * dropped, and the bookkeeping stays exact -- seq and the counters move
 * for accepted events only, so pushed_total remains the exact sum of the
 * by-type counters.
 *
 * When an event is accepted and a waiter is parked on ev_waiters, the
 * waiter is unlinked here (still under evq_lock) and returned via *wakee;
 * the caller wakes it AFTER dropping evq_lock, so runq_lock is never
 * taken while evq_lock is held. */
static void evq_wake(struct task *wakees);

static void evq_put(u32 type, int a, int b, u32 flags, struct task **wakee)
{
    u32 w = ev_w, r = __atomic_load_n(&ev_r, __ATOMIC_SEQ_CST);
    if ((w + 1) % IEVQ_SZ == r) {
        __atomic_add_fetch(&ev_dropped, 1u, __ATOMIC_RELAXED);
        return;
    }
    evq[w].seq = ++ev_seq;
    evq[w].type = type;
    evq[w].a = a;
    evq[w].b = b;
    evq[w].flags = flags;
    evq[w].resv = 0;
    __atomic_store_n(&ev_w, (w + 1) % IEVQ_SZ, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&ev_pushed, 1u, __ATOMIC_RELAXED);
    switch (type) {
    case INPUT_EV_KEY: __atomic_add_fetch(&ev_keys, 1u, __ATOMIC_RELAXED); break;
    case INPUT_EV_REL: __atomic_add_fetch(&ev_rels, 1u, __ATOMIC_RELAXED); break;
    case INPUT_EV_BTN: __atomic_add_fetch(&ev_btns, 1u, __ATOMIC_RELAXED); break;
    case INPUT_EV_WHEEL: __atomic_add_fetch(&ev_wheels, 1u, __ATOMIC_RELAXED); break;
    default: break; /* a producer lied about the type: it took a slot too */
    }
    /* Evict exactly one waiter per accepted event; the event itself stays
     * in the queue until the woken caller pops it. */
    if (ev_waiters && wakee) {
        struct task *t = ev_waiters;
        ev_waiters = t->wait_next;
        t->wait_next = *wakee;
        *wakee = t;
    }
}

void input_push_key(int key, u32 flags)
{
    u64 f;
    struct task *wakees = NULL;
    spin_lock_irqsave(&evq_lock, &f);
    evq_put(INPUT_EV_KEY, key, 0, flags, &wakees);
    spin_unlock_irqrestore(&evq_lock, f);
    evq_wake(wakees);
}

void input_push_mouse(int dx, int dy, u32 buttons, int wheel)
{
    u64 f;
    struct task *wakees = NULL;
    spin_lock_irqsave(&evq_lock, &f);
    /* The baseline follows the wire no matter what the queue had room
     * for: dropped events are lost, button state is not -- or the next
     * packet would replay today's press as tomorrow's release. */
    bool changed = buttons != ev_prev_buttons;
    ev_prev_buttons = buttons;
    if (changed) evq_put(INPUT_EV_BTN, (int)buttons, 0, 0, &wakees);
    if (dx || dy) evq_put(INPUT_EV_REL, dx, dy, 0, &wakees);
    if (wheel) evq_put(INPUT_EV_WHEEL, wheel, 0, 0, &wakees);
    spin_unlock_irqrestore(&evq_lock, f);
    evq_wake(wakees);
}

void input_inject(const struct lnxrm_input_event *ev, u32 n)
{
    u64 f;
    struct task *wakees = NULL;
    spin_lock_irqsave(&evq_lock, &f);
    for (u32 i = 0; i < n; i++)
        evq_put(ev[i].type, ev[i].a, ev[i].b, ev[i].flags, &wakees);
    spin_unlock_irqrestore(&evq_lock, f);
    evq_wake(wakees);
}

/* Wake every waiter collected on the local list (built under evq_lock,
 * drained here with evq_lock dropped so sched_wake_task's runq_lock is
 * never nested inside it).  Each waiter rechecks events/signals/timeout
 * on wake and re-parks or returns -- a second consumer racing ahead that
 * drained the queue just loops back to re-park or time out. */
static void evq_wake(struct task *wakees)
{
    struct task *t = wakees;
    while (t) {
        struct task *next = t->wait_next;
        t->wait_next = NULL;
        sched_wake_task(t);
        t = next;
    }
}

long input_event_pop(struct lnxrm_input_event *out)
{
    u64 f;
    spin_lock_irqsave(&evq_lock, &f);
    u32 r = ev_r;
    if (r == ev_w) {
        spin_unlock_irqrestore(&evq_lock, f);
        return -1;
    }
    *out = evq[r]; /* 24 bytes under the lock: the only reader is the syscall */
    __atomic_store_n(&ev_r, (r + 1) % IEVQ_SZ, __ATOMIC_SEQ_CST);
    spin_unlock_irqrestore(&evq_lock, f);
    return 0;
}

/* Blocking pop for syscall 41 (input_wait).  Returns 1 with *out filled,
 * LNXRM_EINTR when an actionable signal lands, 0 on ms deadline, -EFAULT
 * when copying the event out fails.  ms==0 is the non-blocking pop shape
 * (1 event / LNXRM_EAGAIN); ms==(u64)-1 waits for an event or a signal
 * only (the tick never time-wakes a sleeper whose sleep_until is -1). */
long input_event_wait(struct lnxrm_input_event *out, u64 ms)
{
    /* ms==0: exactly the non-blocking pop that syscall 39 performs. */
    if (ms == 0) {
        struct lnxrm_input_event e;
        if (input_event_pop(&e) != 0) return LNXRM_EAGAIN;
        if (copy_to_user(out, &e, sizeof(e)) < 0) return LNXRM_EFAULT;
        return 1;
    }

    /* Deadline in jiffies, mirroring nanosleep's tick rounding.  (u64)-1
     * means "never": sched_tick's `jiffies >= sleep_until` can never be
     * true, so only a producer or a signal wakes us. */
    const u64 until = ms == (u64)-1 ? (u64)-1
                                    : jiffies + ms * (u64)HZ / 1000 + 1;

    for (;;) {
        /* Fast path: an event is already waiting. */
        {
            u64 f;
            spin_lock_irqsave(&evq_lock, &f);
            u32 r = ev_r;
            if (r != ev_w) {
                struct lnxrm_input_event e = evq[r];
                __atomic_store_n(&ev_r, (r + 1) % IEVQ_SZ, __ATOMIC_SEQ_CST);
                spin_unlock_irqrestore(&evq_lock, f);
                if (copy_to_user(out, &e, sizeof(e)) < 0) return LNXRM_EFAULT;
                return 1;
            }
            spin_unlock_irqrestore(&evq_lock, f);
        }

        /* An actionable signal (not pending-but-ignored) cuts the wait. */
        {
            u64 ready = current->signal_pending & ~current->sig_blocked;
            bool interrupted = false;
            for (int s = 1; ready && s < _NSIG; s++) {
                if ((ready & (1ULL << s)) && !signal_ignored(current, s)) {
                    interrupted = true;
                    break;
                }
            }
            if (interrupted) return LNXRM_EINTR;
        }

        /* Time's up. */
        if (ms != (u64)-1 && (i64)(jiffies - until) >= 0) return 0;

        /* Park: link onto the wait list under evq_lock, then sleep.
         * The unlink-on-wake below keeps the list free of stale entries. */
        u64 f;
        spin_lock_irqsave(&evq_lock, &f);
        /* Recheck under the lock: a producer may have arrived between the
         * unlocked checks above and this one. */
        {
            u32 r = ev_r;
            if (r != ev_w) {
                struct lnxrm_input_event e = evq[r];
                __atomic_store_n(&ev_r, (r + 1) % IEVQ_SZ, __ATOMIC_SEQ_CST);
                spin_unlock_irqrestore(&evq_lock, f);
                if (copy_to_user(out, &e, sizeof(e)) < 0) return LNXRM_EFAULT;
                return 1;
            }
        }
        current->wait_next = ev_waiters;
        ev_waiters = current;
        current->sleep_until = until;
        current->state = T_SLEEPING;
        runqueue_remove(current); /* safe no-op: current is not on a runq */
        spin_unlock_irqrestore(&evq_lock, f);
        schedule();

        /* Woken (producer, signal, or tick).  Unlink ourselves from the
         * wait list so a later producer never evicts a no-longer-sleeping
         * task and starve a real waiter behind it. */
        spin_lock_irqsave(&evq_lock, &f);
        {
            struct task **p = &ev_waiters;
            while (*p && *p != current) p = &(*p)->wait_next;
            if (*p == current) *p = current->wait_next;
            current->wait_next = NULL;
        }
        spin_unlock_irqrestore(&evq_lock, f);
        continue;
    }
}

u32 input_event_pending(void)
{
    u32 r = __atomic_load_n(&ev_r, __ATOMIC_SEQ_CST);
    u32 w = __atomic_load_n(&ev_w, __ATOMIC_SEQ_CST);
    return (w - r) % IEVQ_SZ;
}

void input_event_stats(struct lnxrm_input_info *m)
{
    if (!m) return;
    m->capacity = IEVQ_SZ - 1;
    m->pending = input_event_pending();
    m->pushed_total = __atomic_load_n(&ev_pushed, __ATOMIC_RELAXED);
    m->dropped = __atomic_load_n(&ev_dropped, __ATOMIC_RELAXED);
    m->keys = __atomic_load_n(&ev_keys, __ATOMIC_RELAXED);
    m->rel = __atomic_load_n(&ev_rels, __ATOMIC_RELAXED);
    m->btn = __atomic_load_n(&ev_btns, __ATOMIC_RELAXED);
    m->wheel = __atomic_load_n(&ev_wheels, __ATOMIC_RELAXED);
}

#ifdef CONFIG_KTEST
/* ktest-driven internal hooks: waitlist add/take atomicity.
 * Exercised only with hand-built fake tasks (state == T_SLEEPING, never
 * enqueued on any runqueue): ktest can't run runqueue_add on fakes, and
 * this file's own waitlist is a separate list from run queues. */
struct task *input_waitlist_head_for_test(void)
{
    return ev_waiters;
}

void input_waitlist_push_for_test(struct task *t)
{
    u64 f;
    spin_lock_irqsave(&evq_lock, &f);
    t->wait_next = ev_waiters;
    ev_waiters = t;
    spin_unlock_irqrestore(&evq_lock, f);
}

struct task *input_waitlist_take_one_for_test(void)
{
    u64 f;
    struct task *t;
    spin_lock_irqsave(&evq_lock, &f);
    t = ev_waiters;
    if (t) ev_waiters = t->wait_next;
    spin_unlock_irqrestore(&evq_lock, f);
    return t;
}
#endif
