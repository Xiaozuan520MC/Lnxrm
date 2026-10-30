/* evq: the unified input event queue (39-40), read from ring 3.
 * Pops KEY/REL/BTN/WHEEL events until ESC (or 64 events) and prints one
 * line per event, then shows the queue-end counters.  Run it and type;
 * move the pointer; click.  A plain user answers EACCES -- the queue
 * carries raw keys, so consuming it is root's surface. */
#include "ulib.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    xputs("evq: reading the unified input event queue -- ESC quits\n");

    struct lnxrm_input_event e;
    int n = 0;
    int quit = 0; /* userland has no stdbool; 0/1 is the house style */
    for (;;) {
        long r = kinput_pop(&e);
        if (r == LNXRM_EAGAIN) { /* nothing new: brief nap, back to polling */
            ksleep_ms(50, NULL);
            continue;
        }
        if (r < 0) {
            xprintf("evq: input_pop failed %d\n", r);
            return 1;
        }
        n++;
        xprintf("[%d] seq=%d ", n, (long)e.seq);
        switch (e.type) {
        case INPUT_EV_KEY:
            xprintf("KEY a=%d%s%s", (long)e.a,
                    (e.flags & INPUT_MOD_SHIFT) ? " shift" : "",
                    (e.flags & INPUT_MOD_CAPS) ? " caps" : "");
            if (e.a >= 32 && e.a < 127) {
                xputs(" '");
                putc_((char)e.a);
                xputs("'");
            }
            if (e.a == 27) quit = true; /* ESC: still printed, then exit */
            break;
        case INPUT_EV_REL:
            xprintf("REL dx=%d dy=%d", (long)e.a, (long)e.b);
            break;
        case INPUT_EV_BTN:
            xprintf("BTN mask=%ld%s%s%s", (long)e.a,
                    (e.a & 1) ? " LB" : "", (e.a & 2) ? " MB" : "",
                    (e.a & 4) ? " RB" : "");
            break;
        case INPUT_EV_WHEEL:
            xprintf("WHEEL %d", (long)e.a);
            break;
        default: /* cannot happen: every accepted event has a known type */
            xprintf("TYPE-%d", (long)e.type);
            break;
        }
        kwrite(1, "\n", 1);
        if (quit || n >= 64) break;
    }

    struct lnxrm_input_info in;
    if (kinput_info(&in) == 0) {
        xprintf("queue: cap=%d pending=%d pushed=%d dropped=%d",
                (long)in.capacity, (long)in.pending, (long)in.pushed_total,
                (long)in.dropped);
        xprintf(" keys=%d rel=%d btn=%d wheel=%d\n", (long)in.keys,
                (long)in.rel, (long)in.btn, (long)in.wheel);
    }
    return 0;
}
