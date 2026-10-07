/* /bin/hello -- demonstrates ELF loading, fork/exec, sleep and exit. */
#include "ulib.h"

int main(int argc, char **argv)
{
    xprintf("hello from userland! pid=%u ppid=%u\n", (long)kgetpid(), (long)kgetppid());

    long pid = kfork();
    if (pid == 0) {
        xprintf("  child here (pid=%u)\n", (long)kgetpid());
        ksleep_ms(200, NULL); /* exercise sys_nanosleep + schedule/idle path */
        kexit(7);
    }
    int st = 0;
    long got = kwaitpid(pid, &st, 0);
    xprintf("  parent: child %u exited with %u\n", got, st);

    /* brk() growth test */
    char *old = (char *)kbrk(0);
    char *nw = (char *)kbrk((long)old + 8192);
    if ((long)nw > (long)old) xprintf("  heap grown by %u bytes\n", (long)(nw - old));

    xputs("bye\n");
    return 42;
}
