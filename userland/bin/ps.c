/* /bin/ps -- list running processes from a kernel-filled buffer. */
#include "ulib.h"

static const char *state_name(int s)
{
    switch (s) {
    case LNXRM_TS_RUNNABLE: return "runnable";
    case LNXRM_TS_RUNNING: return "running";
    case LNXRM_TS_SLEEPING: return "sleeping";
    case LNXRM_TS_ZOMBIE: return "zombie";
    case LNXRM_TS_STOPPED: return "stopped";
    case LNXRM_TS_EMBRYO: return "embryo";
    default: return "-";
    }
}

/* T-030: privilege tier names for the KIND column */
static const char *kind_name(int k)
{
    switch (k) {
    case LNXRM_CRED_KXLD: return "kxld";
    case LNXRM_CRED_ROOT: return "root";
    case LNXRM_CRED_USER: return "user";
    default: return "???";
    }
}

/* right-align `v` in `w` columns (xprintf has no field widths) */
static void pad_i(long v, int w)
{
    char buf[16];
    int i = sizeof(buf) - 1;
    long n = v;
    int len;

    buf[i] = 0;
    if (n < 0) n = -n;
    if (n == 0) buf[--i] = '0';
    while (n) {
        buf[--i] = (char)('0' + n % 10);
        n /= 10;
    }
    if (v < 0) buf[--i] = '-';
    len = (int)xstrlen(&buf[i]);
    while (len++ < w) putc_(' ');
    xputs(&buf[i]);
}

int main(int argc, char **argv)
{
    struct lnxrm_ps_entry procs[64];
    long n;

    (void)argc;
    (void)argv;

    n = kps(procs, 64);
    if (n < 0) {
        xputs("ps: failed\n");
        return 1;
    }

    /* MEM/PEAK (T-032): KiB of user pages the kernel mapped for the task,
     * and the largest it has ever held.  Idle shows 0/0 -- it owns no user
     * space at all. */
    xputs("  PID PPID  CPU STATE      KIND  UID   MEM  PEAK NAME\n");
    for (long i = 0; i < n; i++) {
        pad_i(procs[i].pid, 5);
        putc_(' ');
        pad_i(procs[i].ppid, 4);
        putc_(' ');
        pad_i(procs[i].cpu, 4);
        putc_(' ');
        xputs(state_name(procs[i].state));
        for (int k = (int)xstrlen(state_name(procs[i].state)); k < 10; k++) putc_(' ');
        putc_(' ');
        xputs(kind_name(procs[i].kind));
        for (int k = (int)xstrlen(kind_name(procs[i].kind)); k < 4; k++) putc_(' ');
        putc_(' ');
        pad_i(procs[i].uid, 4);
        putc_(' ');
        pad_i(procs[i].mem_kib, 6);
        putc_(' ');
        pad_i(procs[i].peak_kib, 6);
        putc_(' ');
        xputs(procs[i].name);
        putc_('\n');
    }
    return 0;
}
