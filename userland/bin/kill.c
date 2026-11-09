/* /bin/kill -- send signal to a process. */
#include "ulib.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        xputs("usage: kill <pid> [signal]\n");
        /* derived, not spelled out: when the ABI grows a signal this
         * line and the range check below move together with it */
        xprintf("  signal: 0 (probe) .. %d, default %d (SIGTERM)\n",
                (long)(LNXRM_NR_SIGNALS - 1), (long)SIGTERM);
        return 1;
    }

    long pid = 0;
    const char *s = argv[1];
    while (*s >= '0' && *s <= '9') pid = pid * 10 + (*s++ - '0');
    if (pid <= 0 || *s) {
        xputs("kill: invalid pid\n");
        return 1;
    }

    long sig = SIGTERM;
    if (argc >= 3) {
        sig = 0;
        const char *t = argv[2];
        while (*t >= '0' && *t <= '9') sig = sig * 10 + (*t++ - '0');
        if (*t || sig >= LNXRM_NR_SIGNALS) {
            xprintf("kill: signal must be 0..%d\n", (long)(LNXRM_NR_SIGNALS - 1));
            return 1;
        }
    }

    long r = kkill(pid, sig);
    if (r < 0) {
        /* say WHICH gate said no: -13 = kxld rule / uid+cap gate,
         * -3 = no such pid.  "failed" hides both. */
        if (r == LNXRM_EACCES)
            xputs("kill: permission denied\n");
        else if (r == LNXRM_ESRCH)
            xputs("kill: no such process\n");
        else
            xputs("kill: failed\n");
        return 1;
    }
    if (sig == 0) xputs("kill: process exists\n");
    return 0;
}
