/* /bin/kbd -- keyboard probe: read keys live, or self-test the pipe.
 *   kbd          -- listen: print every key as it arrives (Esc/Ctrl-C quits)
 *   kbd info     -- one statistics snapshot (kbd 36), then exit
 *   kbd test     -- kkbd_inject -> kkbd_get roundtrip, then exit
 * kbd_get (34) is non-blocking, so listen() sleeps 10ms between polls. */
#include "ulib.h"

static unsigned u32v(const struct lnxrm_kbd_info *k)
{ return (unsigned)k->pending; }

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void hex2(char c)
{
    static const char h[] = "0123456789abcdef";
    putc_(h[(c >> 4) & 0xF]);
    putc_(h[c & 0xF]);
}

static void snapshot(const char *what, struct lnxrm_kbd_info *k)
{
    xprintf("kbd.info.%s=%d\n", what, kkbd_info(k));
    xprintf("kbd.info.pending=%d\n", (long)u32v(k));
    xprintf("kbd.info.capacity=%d\n", (long)k->capacity);
    xprintf("kbd.info.pushed=%d\n", (long)k->pushed_total);
    xprintf("kbd.info.dropped=%d\n", (long)k->dropped);
    xprintf("kbd.info.scans=%d\n", (long)k->ps2_scans);
}

static int listen_mode(void)
{
    struct lnxrm_kbd_info a = {0}, b = {0};
    snapshot("before", &a);
    xputs("kbd: listening, Esc or Ctrl-C quits\n");
    for (;;) {
        long c = kkbd_get();
        if (c == LNXRM_EAGAIN) {
            if (ksleep_ms(10, 0) < 0) break;
            continue;
        }
        if (c < 0) break; /* -13 without the gate */
        xputs("kbd: key=");
        hex2((char)c);
        if (c >= 32 && c < 127) xprintf(" '%c'(%d)\n", (int)c, (int)c);
        else putc_('\n');
        if (c == 27 /* Esc */ || c == 3 /* Ctrl-C */) break;
    }
    kkbd_info(&b);
    xprintf("kbd.listen.keys=%d\n", (long)(b.pushed_total - a.pushed_total));
    xprintf("kbd.listen.dropped=%d\n", (long)(b.dropped - a.dropped));
    xprintf("kbd.listen.scans=%d\n", (long)(b.ps2_scans - a.ps2_scans));
    return 0;
}

static int test_mode(void)
{
    struct lnxrm_kbd_info k = {0};
    snapshot("before", &k);
    if (k.pending != 0) {
        xprintf("kbd.skip=%d\n", (long)k.pending);
        return 1;
    }
    static const char msg[] = "kbd0"; /* 4 synthetic keys, no NUL */
    long r = kkbd_inject(msg, 4);
    xprintf("kbd.inject=%d\n", r);
    if (r != 4) return 1;

    long last[4];
    long n = 0, c;
    for (;;) {
        c = kkbd_get();
        if (c == LNXRM_EAGAIN) break;
        if (n > 4096) break; /* runaway console traffic: fail loudly */
        last[n++ & 3] = c;
    }
    xprintf("kbd.drain.n=%d\n", n);
    int ok = (n >= 4);
    for (int i = 0; ok && i < 4; i++)
        if (last[i] != msg[i]) ok = 0;
    xprintf("kbd.roundtrip=%d\n", ok ? 1 : 0);
    xprintf("kbd.eagain.after=%d\n", kkbd_get());
    kkbd_info(&k);
    xprintf("kbd.info.after.pending=%d\n", (long)k.pending);
    return (ok && k.pending == 0) ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc > 2) {
        xputs("kbd: usage: kbd [info|test]\n");
        return 1;
    }
    if (argc == 2 && streq(argv[1], "info")) {
        struct lnxrm_kbd_info k = {0};
        snapshot("one", &k);
        return 0;
    }
    if (argc == 2 && streq(argv[1], "test")) return test_mode();
    if (argc == 2) {
        xprintf("kbd: unknown mode\n");
        return 1;
    }
    return listen_mode();
}
