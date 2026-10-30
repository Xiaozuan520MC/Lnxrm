/* input 39-40 surface: the unified queue driven through the real dispatch
 * (syscall_probe = gates + pointer checks + error codes).  Nothing feeds
 * this queue on a headless boot, and the other input cases (t_kbd ASCII
 * injection, t_mouse packets) may run before or after this file -- link
 * order, not source order -- so every case here starts from a counters +
 * pending snapshot, borrows nothing in silence and restores exactly what
 * it took: foreign events drained, the console ring returned empty
 * (t_kbd, and every later reader, starts from an empty ring), the aux
 * ring returned empty (t_mouse's own invariant), and the shift/capslock
 * state left as it was found (an even number of toggles).  Gate refusals
 * are deliberately NOT probed here for the same reason t_kbd/t_mouse
 * skip them: setuid mutates the caller's identity, and the demoted-tier
 * answers (-13) are user/systest's lines.  (2026-10-30) */
#include <sys/ktest.h>
#include <console.h>
#include <mm/mm.h>
#include <sys/sched.h>
#include <sys/cpu.h>
#include <sys/input.h>
#include <sys/vfs.h>
#include <abi/lnxrm_abi.h>

#define EVT_VA   (USER_BASE + 0xF000) /* nobody else in ktest/ maps this */
#define EV_VA    (EVT_VA + 0x40)      /* one-event receiver */
#define EINFO_VA (EVT_VA + 0x80)      /* input_info receiver */
#define KINJ_VA  (EVT_VA + 0xC0)      /* kbd_inject scratch */
#define BAD_VA   (USER_BASE + 0xC000) /* kept unmapped for EFAULT probes */

/* scancode set 1 constants, the decode path's own (see isr.c) */
#define SC_A_MAKE   0x1E
#define SC_LSHIFT   0x2A
#define SC_CAPSLOCK 0x3A
#define SC_BREAK(sc) ((u8)((sc) | 0x80))

static void uput(u64 va, const void *src, size_t n)
{
    u64 ac = smap_enter();
    memcpy((void *)va, src, n);
    smap_leave(ac);
}

static void uget(void *dst, u64 va, size_t n)
{
    u64 ac = smap_enter();
    memcpy(dst, (const void *)va, n);
    smap_leave(ac);
}

static u64 page_map(void)
{
    u64 pa = pmm_alloc();
    /* K_ASSERT's bare `return` cannot leave a value in a u64 function, so
     * the two invariants here speak to the check functions directly; a
     * failure already made the case unrecoverable (return 0 -> the first
     * use panics), and the normal path is what any reader needs to know. */
    if (!ktest_check(!!(pa != 0), __FILE__, __LINE__, "pa != 0")) return 0;
    ktest_check_eq(vmm_map_user(current->pml4, EVT_VA, pa, true, true, false),
                   0, __FILE__, __LINE__, "vmm_map_user(EVT_VA)", "0");
    return pa;
}

static void page_unmap(u64 pa)
{
    K_EXPECT_EQ(vmm_unmap_user(current->pml4, EVT_VA), pa);
    pmm_free(pa);
    K_EXPECT_EQ(vmm_translate_in(current->pml4, EVT_VA), 0);
}

/* Empty the queue on the kernel side and account for everything that
 * stood in it: every event carried a real type, a strictly-rising seq
 * and a zero tail, no matter which case (or neighbour file) pushed it. */
static u32 evq_drain(void)
{
    struct lnxrm_input_event e;
    u32 n = 0, last = 0;
    bool first = true;
    while (input_event_pop(&e) == 0) {
        if (!first) K_EXPECT(e.seq > last);
        K_EXPECT(e.type != INPUT_EV_NONE && e.type <= INPUT_EV_WHEEL);
        K_EXPECT(e.flags == 0 || e.type == INPUT_EV_KEY); /* mods: KEY only */
        K_EXPECT_EQ(e.resv, 0u);
        last = e.seq;
        first = false;
        n++;
    }
    return n;
}

static void test_input_syscall_surface(void)
{
    u64 pa = page_map();
    u8 page[256]; /* kernel stack: a pointer the user-range check refuses */

    /* drain everything first, so "empty" is a fact, not a hope */
    u32 before = input_event_pending();
    K_EXPECT_EQ(evq_drain(), before);
    K_EXPECT_EQ(input_event_pending(), 0);

    /* empty answers: EAGAIN to a good receiver, EFAULT to every bad
     * pointer even with nothing to pop -- validation is atomic with the
     * attempt, exactly mouse_get's shape.  (The -13 gate answer belongs
     * to user/systest; ktest tasks hold CAP_SYS_ADMIN and get through.) */
    K_EXPECT_EQ(syscall_probe(SYS_input_pop, EV_VA, 0, 0), LNXRM_EAGAIN);
    K_EXPECT_EQ(syscall_probe(SYS_input_pop, 0, 0, 0), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_input_pop, 0xdeadbeef, 0, 0), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_input_pop, (u64)page, 0, 0), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_input_pop, 0x8000000000000000ULL, 0, 0),
                LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_input_pop, BAD_VA, 0, 0), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_input_pop, EVT_VA + 0xFFF, 0, 0),
                LNXRM_EFAULT); /* 24 bytes straddle into the unmapped page */

    /* the round trip: 1 means "one event", and the 24 bytes arrive whole
     * -- fields intact, seq assigned by the queue, resv always 0 */
    struct lnxrm_input_event in = {.type = INPUT_EV_KEY,
                                   .a = 'q',
                                   .flags = INPUT_MOD_SHIFT};
    input_inject(&in, 1);
    K_EXPECT_EQ(input_event_pending(), 1u);
    K_EXPECT_EQ(syscall_probe(SYS_input_pop, EV_VA, 0, 0), 1);
    struct lnxrm_input_event e = {0};
    uget(&e, EV_VA, sizeof(e));
    K_EXPECT_EQ(e.type, INPUT_EV_KEY);
    K_EXPECT_EQ(e.a, 'q');
    K_EXPECT_EQ(e.b, 0);
    K_EXPECT_EQ(e.flags, (u32)INPUT_MOD_SHIFT);
    K_EXPECT(e.seq != 0);
    K_EXPECT_EQ(e.resv, 0u);
    K_EXPECT_EQ(syscall_probe(SYS_input_pop, EV_VA, 0, 0), LNXRM_EAGAIN);
    K_EXPECT_EQ(input_event_pending(), 0u); /* single-shot, honestly empty */

    /* input_info: the open reader's shape and its own EFAULT sweep */
    struct lnxrm_input_info ii = {0};
    K_EXPECT_EQ(syscall_probe(SYS_input_info, EINFO_VA, 0, 0), 0);
    uget(&ii, EINFO_VA, sizeof(ii));
    K_EXPECT_EQ((long)ii.capacity, 63);
    K_EXPECT_EQ((long)ii.pending, 0);
    K_EXPECT(ii.dropped <= ii.pushed_total);
    K_EXPECT_EQ((long)(ii.keys + ii.rel + ii.btn + ii.wheel),
                (long)ii.pushed_total); /* every event counted by its type */
    K_EXPECT_EQ(syscall_probe(SYS_input_info, 0, 0, 0), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_input_info, (u64)page, 0, 0), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_input_info, 0x8000000000000000ULL, 0, 0),
                LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_input_info, BAD_VA, 0, 0), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_input_info, EVT_VA + 0xFFF, 0, 0),
                LNXRM_EFAULT); /* 32 bytes straddle the page edge */

    /* teardown: exactly the frame we mapped, queue left as found */
    page_unmap(pa);
    K_EXPECT_EQ(input_event_pending(), 0);
    K_EXPECT_EQ(evq_drain(), 0);
}

static void test_input_keyboard_events(void)
{
    struct lnxrm_kbd_info c0, c1;
    struct lnxrm_input_info s0, s1, mid;
    u64 pa = page_map();
    struct lnxrm_input_event e;
    u32 last_seq = 0;
    bool seen = false;

#define POP_KEY_EVT(ch, want_flags)                                            \
    do {                                                                       \
        K_EXPECT_EQ(syscall_probe(SYS_input_pop, EV_VA, 0, 0), 1);             \
        uget(&e, EV_VA, sizeof(e));                                            \
        K_EXPECT_EQ(e.type, INPUT_EV_KEY);                                     \
        K_EXPECT_EQ(e.a, (long)(ch));                                          \
        K_EXPECT_EQ(e.flags, (u32)(want_flags));                               \
        K_EXPECT_EQ(e.b, 0);                                                   \
        K_EXPECT_EQ(e.resv, 0u);                                               \
        if (seen) K_EXPECT(e.seq > last_seq);                                  \
        last_seq = e.seq;                                                      \
        seen = true;                                                           \
    } while (0)

    K_ASSERT_EQ((long)kbd_pending(), 0); /* the ring we borrow starts empty */
    u32 q0 = input_event_pending();
    K_EXPECT_EQ(evq_drain(), q0);

    kbd_stats(&c0);
    input_event_stats(&s0);

    /* a modifier down is control state, not a keystroke: nothing for the
     * console, nothing for the queue */
    kbd_inject_scancode(SC_LSHIFT);
    K_EXPECT_EQ((long)kbd_pending(), 0);

    /* 'a' with shift held: the console sees the transformed character and
     * the queue sees it with the recipe behind it */
    kbd_inject_scancode(SC_A_MAKE);
    K_EXPECT_EQ(syscall_probe(SYS_kbd_get, 0, 0, 0), 'A');
    POP_KEY_EVT('A', INPUT_MOD_SHIFT);

    /* break codes are state, not input: never an event */
    kbd_inject_scancode(SC_BREAK(SC_A_MAKE));
    kbd_inject_scancode(SC_BREAK(SC_LSHIFT));
    K_EXPECT_EQ((long)kbd_pending(), 0);

    /* plain 'a', shift released: no flags */
    kbd_inject_scancode(SC_A_MAKE);
    K_EXPECT_EQ(syscall_probe(SYS_kbd_get, 0, 0, 0), 'a');
    POP_KEY_EVT('a', 0);

    /* capslock: no event of its own, but the letters it rewrites carry
     * MOD_CAPS (and nothing else: shift is up) */
    kbd_inject_scancode(SC_CAPSLOCK);
    kbd_inject_scancode(SC_A_MAKE);
    K_EXPECT_EQ(syscall_probe(SYS_kbd_get, 0, 0, 0), 'A');
    POP_KEY_EVT('A', INPUT_MOD_CAPS);
    kbd_inject_scancode(SC_CAPSLOCK); /* even toggles: caps ends as it began */
    kbd_inject_scancode(SC_A_MAKE);
    K_EXPECT_EQ(syscall_probe(SYS_kbd_get, 0, 0, 0), 'a');
    POP_KEY_EVT('a', 0);

    /* kbd_inject 35 types console bytes and stays out of the queue --
     * synthetic terminal input is not a keystroke: only the real decode
     * path produces these */
    uput(KINJ_VA, (u8[]){'x'}, 1);
    input_event_stats(&mid);
    K_EXPECT_EQ(syscall_probe(SYS_kbd_inject, KINJ_VA, 1, 0), 1);
    K_EXPECT_EQ(syscall_probe(SYS_kbd_get, 0, 0, 0), 'x');
    input_event_stats(&s1);
    K_EXPECT_EQ((long)s1.pushed_total, (long)mid.pushed_total);
    K_EXPECT_EQ((long)s1.pending, (long)mid.pending);
    K_EXPECT_EQ(syscall_probe(SYS_kbd_get, 0, 0, 0), LNXRM_EAGAIN);
    K_EXPECT_EQ((long)kbd_pending(), 0); /* console ring still clean */

    /* accounting: four KEY events, nothing else, no drops, and scancode
     * injection faithfully left kbd_scans (the hardware detector) exactly
     * where it was */
    input_event_stats(&s1);
    kbd_stats(&c1);
    K_EXPECT_EQ((long)s1.keys, (long)s0.keys + 4);
    K_EXPECT_EQ((long)s1.rel, (long)s0.rel);
    K_EXPECT_EQ((long)s1.btn, (long)s0.btn);
    K_EXPECT_EQ((long)s1.wheel, (long)s0.wheel);
    K_EXPECT_EQ((long)s1.pushed_total, (long)s0.pushed_total + 4);
    K_EXPECT_EQ((long)s1.dropped, (long)s0.dropped);
    K_EXPECT_EQ((long)s1.pending, 0);
    K_EXPECT_EQ((long)c1.ps2_scans, (long)c0.ps2_scans);
    K_EXPECT_EQ((long)c1.pushed_total, (long)c0.pushed_total + 5);
    K_EXPECT_EQ((long)c1.pending, 0);

#undef POP_KEY_EVT
    page_unmap(pa);
}

static void test_input_mouse_events(void)
{
    struct lnxrm_input_info s0, s1;
    int native = ms_bytes_per_packet();

    K_ASSERT_EQ((long)ms_pending(), 0); /* the aux ring we borrow: empty */
    u64 pa = page_map();
    u32 q0 = input_event_pending();
    K_EXPECT_EQ(evq_drain(), q0); /* isolation first */
    input_event_stats(&s0);

    struct lnxrm_input_event e;
    u32 last_seq = 0;
    bool seen = false;

#define POP_EVT(tp, aa, bb)                                                    \
    do {                                                                       \
        K_EXPECT_EQ(syscall_probe(SYS_input_pop, EV_VA, 0, 0), 1);             \
        uget(&e, EV_VA, sizeof(e));                                            \
        K_EXPECT_EQ(e.type, (tp));                                             \
        K_EXPECT_EQ(e.a, (long)(aa));                                          \
        K_EXPECT_EQ(e.b, (long)(bb));                                          \
        K_EXPECT_EQ(e.flags, 0u); /* mouse events carry no key modifiers */    \
        if (seen) K_EXPECT(e.seq > last_seq);                                  \
        last_seq = e.seq;                                                      \
        seen = true;                                                           \
    } while (0)

    /* press+move through the real packet path: one packet, two events,
     * BTN-then-REL -- the wire says LB held while it moved 3 right and
     * 2 down (dy -2 on the wire, flipped to screen-space at the exit) */
    ms_set_packet_bytes(3);
    ms_feed_byte(0x29);
    ms_feed_byte(0x03);
    ms_feed_byte(0xFE);
    POP_EVT(INPUT_EV_BTN, 1, 0); /* LB went down */
    POP_EVT(INPUT_EV_REL, 3, 2);

    /* a repeated button is not a click; the move is */
    ms_feed_byte(0x09);
    ms_feed_byte(0x01);
    ms_feed_byte(0x00);
    POP_EVT(INPUT_EV_REL, 1, 0);

    /* zero deltas, no wheel, unchanged buttons: a packet of silence */
    ms_feed_byte(0x08);
    ms_feed_byte(0x00);
    ms_feed_byte(0x00);

    /* release: the mask change is the event (flags 0x08: sanity bit
     * only -- no motion, no sign bits, buttons released) */
    ms_feed_byte(0x08);
    ms_feed_byte(0x00);
    ms_feed_byte(0x00);
    POP_EVT(INPUT_EV_BTN, 0, 0);

    /* wheel mode: scroll only -- no REL for a parked pointer */
    ms_set_packet_bytes(4);
    ms_feed_byte(0x08);
    ms_feed_byte(0x00);
    ms_feed_byte(0x00);
    ms_feed_byte(0x01);
    POP_EVT(INPUT_EV_WHEEL, 1, 0);

    /* wheel down: 0x0F is the 4-bit two's-complement -1 */
    ms_feed_byte(0x08);
    ms_feed_byte(0x00);
    ms_feed_byte(0x00);
    ms_feed_byte(0x0F);
    POP_EVT(INPUT_EV_WHEEL, -1, 0);

    /* move and scroll together: REL then WHEEL, buttons untouched */
    ms_feed_byte(0x08);
    ms_feed_byte(0x05);
    ms_feed_byte(0x00);
    ms_feed_byte(0x02);
    POP_EVT(INPUT_EV_REL, 5, 0);
    POP_EVT(INPUT_EV_WHEEL, 2, 0);

    /* counters: 8 events exactly -- 3 REL, 2 BTN, 3 WHEEL, 0 KEY */
    input_event_stats(&s1);
    K_EXPECT_EQ((long)s1.rel, (long)s0.rel + 3);
    K_EXPECT_EQ((long)s1.btn, (long)s0.btn + 2);
    K_EXPECT_EQ((long)s1.wheel, (long)s0.wheel + 3);
    K_EXPECT_EQ((long)s1.keys, (long)s0.keys);
    K_EXPECT_EQ((long)s1.pushed_total, (long)s0.pushed_total + 8);
    K_EXPECT_EQ((long)s1.dropped, (long)s0.dropped);
    K_EXPECT_EQ((long)s1.pending, 0);

    /* the msring served both consumers and is now empty: its 7 packets
     * came back through mouse_get, deltas untouched by the new consumer */
    static const int dx7[7] = {3, 1, 0, 0, 0, 0, 5};
    struct lnxrm_mouse_state m;
    for (int i = 0; i < 7; i++) {
        K_EXPECT_EQ(mouse_pop(&m), 0);
        K_EXPECT_EQ((long)m.dx, dx7[i]);
    }
    K_EXPECT_EQ(mouse_pop(&m), -1);
    K_EXPECT_EQ((long)ms_pending(), 0);

#undef POP_EVT
    ms_set_packet_bytes(native);
    page_unmap(pa);
}

static void test_input_queue_fill_drop_order(void)
{
    struct lnxrm_input_info s0, s1, s2;
    struct lnxrm_kbd_info c0, c1;

    u32 q0 = input_event_pending();
    K_EXPECT_EQ(evq_drain(), q0); /* start from exactly 0 */
    kbd_stats(&c0);
    input_event_stats(&s0);

    /* the fill pattern, injected verbatim past the producers: 63 events
     * = 16 KEY + 16 REL + 16 BTN + 15 WHEEL, each with its own payload */
    static const u32 pattern[4] = {INPUT_EV_KEY, INPUT_EV_REL, INPUT_EV_BTN,
                                   INPUT_EV_WHEEL};
    struct lnxrm_input_event ev[63];
    for (u32 i = 0; i < 63; i++) {
        ev[i].type = pattern[i % 4];
        ev[i].a = (int32_t)i;
        ev[i].b = (int32_t)(1000 + i);
        ev[i].flags = i & INPUT_MOD_MASK;
        ev[i].seq = 0; /* the queue owns seq; inject must ignore ours */
        ev[i].resv = 0;
    }
    input_inject(ev, 0); /* a zero-length inject is a legal no-op */
    K_EXPECT_EQ(input_event_pending(), 0u);
    input_inject(ev, 63);
    K_EXPECT_EQ(input_event_pending(), 63u); /* exactly full */

    input_event_stats(&s1);
    K_EXPECT_EQ((long)s1.pushed_total, (long)s0.pushed_total + 63);
    K_EXPECT_EQ((long)s1.dropped, (long)s0.dropped);
    K_EXPECT_EQ((long)s1.keys, (long)s0.keys + 16);
    K_EXPECT_EQ((long)s1.rel, (long)s0.rel + 16);
    K_EXPECT_EQ((long)s1.btn, (long)s0.btn + 16);
    K_EXPECT_EQ((long)s1.wheel, (long)s0.wheel + 15);

    /* the 64th is honestly dropped: NEW is discarded, the ring keeps the
     * events it owns, and the counters see it all */
    input_inject(ev, 1);
    input_event_stats(&s2);
    K_EXPECT_EQ((long)s2.dropped, (long)s1.dropped + 1);
    K_EXPECT_EQ((long)s2.pending, 63);
    K_EXPECT_EQ((long)s2.pushed_total, (long)s1.pushed_total);

    /* the old rings never heard of any of this */
    kbd_stats(&c1);
    K_EXPECT_EQ((long)c1.pending, 0);
    K_EXPECT_EQ((long)c1.pushed_total, (long)c0.pushed_total);
    K_EXPECT_EQ((long)ms_pending(), 0);

    /* and the FIFO gives the injection back 1:1, seq climbing by exactly
     * one per event (no producer can interpose inside a test case) */
    struct lnxrm_input_event e;
    u32 last = 0;
    for (u32 i = 0; i < 63; i++) {
        K_ASSERT_EQ(input_event_pop(&e), 0);
        K_EXPECT_EQ(e.type, ev[i].type); /* pattern survives the ring */
        K_EXPECT_EQ(e.a, (long)ev[i].a);
        K_EXPECT_EQ(e.b, (long)ev[i].b);
        K_EXPECT_EQ(e.flags, (u32)ev[i].flags);
        K_EXPECT_EQ(e.resv, 0u);
        if (i) K_EXPECT_EQ(e.seq, last + 1);
        last = e.seq;
    }
    K_EXPECT_EQ(input_event_pop(&e), -1);
    K_EXPECT_EQ(input_event_pending(), 0u);

    /* a queue is a queue, not a latch: after the drain it accepts again */
    input_inject(ev, 1);
    K_EXPECT_EQ(input_event_pending(), 1u);
    K_EXPECT_EQ(input_event_pop(&e), 0);
    K_EXPECT_EQ(input_event_pending(), 0u);
}

KTEST("input", KTEST_LATE, test_input_syscall_surface);
KTEST("input", KTEST_LATE, test_input_keyboard_events);
KTEST("input", KTEST_LATE, test_input_mouse_events);
KTEST("input", KTEST_LATE, test_input_queue_fill_drop_order);
