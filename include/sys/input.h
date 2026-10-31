/* The unified input event queue (kernel/drivers/input_event.c, syscalls 39-40):
 * one stream -- keyboard makes, mouse deltas, button transitions, wheel
 * -- that a compositor pops instead of touching per-device rings.  The
 * existing rings (console inbuf, msring) keep working as independent
 * consumers of the same producers, so nothing reroutes underneath the
 * 34-38 surface.  Producer semantics live on the producers (isr.c decode
 * path, ps2mouse.c packet tail) and in input_event.c's header comment. */
#pragma once
#include <types.h>
#include <abi/lnxrm_abi.h>

struct task; /* input_event.c waitlist hooks (CONFIG_KTEST only) */

#ifdef __cplusplus
extern "C" {
#endif

/* producers */
void input_push_key(int key, u32 flags); /* one decoded keystroke + mods */
void input_push_mouse(int dx, int dy, u32 buttons, int wheel); /* one packet */
/* ktest-driven: push fully-formed events verbatim, past every producer
 * and its per-packet policy.  Kernel-internal on purpose, not a syscall
 * -- a task able to synthesize arbitrary keys into the queue a
 * compositor reads is input spoofing, exactly what kbd_inject's
 * CAP_SYS_ADMIN gate exists to refuse.  The queue still owns seq and
 * zeroes resv: only a/b/type/flags ride through untouched. */
void input_inject(const struct lnxrm_input_event *ev, u32 n);

/* consumer */
long input_event_pop(struct lnxrm_input_event *out); /* 0 ok, -1 empty */
/* Blocking variant of pop: ms==0 answers like pop (1 event / -11 EAGAIN);
 * ms>0 parks on the queue's wait list until an event arrives, an
 * actionable signal lands (-> LNXRM_EINTR), or the ms deadline passes
 * (-> 0).  ms == (u64)-1 waits forever (only a signal or an event wakes). */
long input_event_wait(struct lnxrm_input_event *out, u64 ms);
u32 input_event_pending(void);

#ifdef CONFIG_KTEST
/* waitlist add/take atomicity for ktest (fake tasks only, never enqueued
 * on any runqueue). */
struct task *input_waitlist_head_for_test(void);
void input_waitlist_push_for_test(struct task *t);
struct task *input_waitlist_take_one_for_test(void);
#endif
void input_event_stats(struct lnxrm_input_info *out);

#ifdef __cplusplus
}
#endif
