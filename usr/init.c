/* /init -- first user process. Like Linux: userland drives everything;
 * the kernel just provides fork/exec/wait and files. */
#include "ulib.h"

static char buf[256];

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* wire stdin/stdout/stderr to the console */
    long c = kopen("/dev/console", 2);
    if (c >= 0) {
        if (c != 0) {
            kdup2(c, 0);
            kclose(c);
        }
        kdup2(0, 1);
        kdup2(0, 2);
    }

    /* disk smoke test through IDE+FAT32 */
    int dfd = kopen("/README.md", 0);
    if (dfd >= 0) {
        long n = kread(dfd, buf, sizeof(buf));
        if (n > 0) kwrite(1, buf, n);
        kclose(dfd);
    } else {
        xputs("init: no /README.md\n");
    }

    /* Hand the console to a userspace terminal exactly once.  If the
     * terminal dies (e.g. `kill <pid>`), it stays dead: respawning it
     * would make the terminal unkillable.  init itself keeps running so
     * pid 1 still exists to reparent orphans. */
    long pid = kfork();
    if (pid == 0) {
        char *sh_argv[] = {"/bin/sh", 0};
        char *envp[] = {0};
        if (kexecve("/bin/sh", sh_argv, envp) < 0) {
            xputs("init: exec /bin/sh failed\n");
            kexit(1);
        }
    }
    int st = 0;
    kwaitpid(pid, &st, 0);
    xputs("\ninit: terminal exited\n");

    for (;;) ksleep_ms(3600000, 0);
    return 0;
}
