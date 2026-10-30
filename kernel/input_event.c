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
 * -1 (the syscall turns that into EAGAIN) and push never sleeps; a
 * blocking variant with waiters is the obvious future extension.  Serial
 * RX bytes are console-only for now -- folding them in is too. */
#include <types.h>
#include <sys/spinlock.h>
#include <sys/input.h>

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

/* One event takes a slot; the callers hold the lock, so this decides
 * only what a *full* queue means: the event that arrived last is the one
 * dropped, and the bookkeeping stays exact -- seq and the counters move
 * for accepted events only, so pushed_total remains the exact sum of the
 * by-type counters. */
static void evq_put(u32 type, int a, int b, u32 flags)
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
}

void input_push_key(int key, u32 flags)
{
    u64 f;
    spin_lock_irqsave(&evq_lock, &f);
    evq_put(INPUT_EV_KEY, key, 0, flags);
    spin_unlock_irqrestore(&evq_lock, f);
}

void input_push_mouse(int dx, int dy, u32 buttons, int wheel)
{
    u64 f;
    spin_lock_irqsave(&evq_lock, &f);
    /* The baseline follows the wire no matter what the queue had room
     * for: dropped events are lost, button state is not -- or the next
     * packet would replay today's press as tomorrow's release. */
    bool changed = buttons != ev_prev_buttons;
    ev_prev_buttons = buttons;
    if (changed) evq_put(INPUT_EV_BTN, (int)buttons, 0, 0);
    if (dx || dy) evq_put(INPUT_EV_REL, dx, dy, 0);
    if (wheel) evq_put(INPUT_EV_WHEEL, wheel, 0, 0);
    spin_unlock_irqrestore(&evq_lock, f);
}

void input_inject(const struct lnxrm_input_event *ev, u32 n)
{
    u64 f;
    spin_lock_irqsave(&evq_lock, &f);
    for (u32 i = 0; i < n; i++)
        evq_put(ev[i].type, ev[i].a, ev[i].b, ev[i].flags);
    spin_unlock_irqrestore(&evq_lock, f);
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
