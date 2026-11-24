// SPDX-License-Identifier: Apache-2.0

/* /init -- first user process. Like Linux: userland drives everything;
 * the kernel just provides fork/exec/wait and files. */
#include "ulib.h"

static char buf[256];

/* S1b: spawn every module in /mod (each one a .km file).
 *
 * A module is an ordinary static ELF that happens to sit in /mod instead
 * of /bin.  init does not link against it, does not know what it does and
 * does not wait for it -- it only has to be its reaper, which the loop at
 * the bottom of main already is.  Discovery by directory scan instead of
 * a link-time list IS the point: adding a driver becomes adding a file to
 * the image, not rebuilding the kernel.
 *
 * Nothing here is fatal.  No /mod on the image, an empty directory, a file
 * that will not exec -- each one is a reason to move on, not to take pid 1
 * down with it.  But NONE of them may be SILENT: an init that skips a file
 * without saying why produces exactly the failure this code once had -- the
 * modules sit on the disk, unspawned, and `ps` shows nothing to explain it. */

/* The ".km" test, case-insensitively.
 *
 * The check itself has not changed since S1b, only its rationale: /mod now
 * sits on an ext2 root whose path resolution IS case-sensitive, and the
 * image always ships lowercase ".km" names.  Keeping the filter blind to
 * case still costs nothing -- a stray "X.KM" would pass here and then fail
 * loudly at exec (ENOENT), which is where the complaint belongs -- and it
 * keeps matching FAT volumes, where a short name's case comes from the NT
 * bits in the directory record rather than from the file.  The three bytes
 * are matched end-anchored, so "x.km" passes and "x.kmx" does not. */
static int is_module_name(const char *name, size_t n)
{
    char a, b, c;

    if (n < 4) return 0;
    a = name[n - 3];
    b = name[n - 2];
    c = name[n - 1];
    if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
    if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return a == '.' && b == 'k' && c == 'm';
}

static void spawn_modules(void)
{
    long mdfd = kopen("/mod", 0);
    if (mdfd < 0) {
        /* The one line that tells "the image carries no modules" apart from
         * "the modules are there and something else is wrong".  Before this
         * the whole scan vanished without a trace. */
        xputs("init: no /mod on this image -- nothing to spawn\n");
        return;
    }

    struct lnxrm_dirent e;
    char path[64]; /* "/mod/" + a 55-byte name + NUL, well inside 64 */
    int scanned = 0;
    int spawned = 0;

    for (;;) {
        long got = kgetdent(mdfd, &e, sizeof(e));
        if (got == 0) break; /* end of the listing */
        if (got < 0) {       /* a dead read is not an empty directory */
            xputs("init: /mod listing failed, scan stopped\n");
            break;
        }
        if (e.d_type == 4) continue; /* a subdirectory, not a module */

        scanned++;
        size_t n = xstrlen(e.d_name);
        if (!is_module_name(e.d_name, n)) continue;
        if (6 + n > sizeof(path)) { /* cannot be named safely */
            xputs("init: module name too long, skipped: ");
            xputs(e.d_name);
            xputs("\n");
            continue;
        }

        path[0] = '/';
        path[1] = 'm';
        path[2] = 'o';
        path[3] = 'd';
        path[4] = '/';
        for (size_t i = 0; i < n; i++) path[5 + i] = e.d_name[i];
        path[5 + n] = 0;

        /* The "init: module <path>" line that used to print here is gone on
         * purpose (2026-11-18): a module that loads correctly should be
         * invisible -- `ps` already lists it.  Every way of NOT loading one
         * still speaks, below and after the loop; silence now means
         * "everything worked". */

        long pid = kfork();
        if (pid < 0) {
            /* Said out loud: a fork that never happens leaves the same
             * empty `ps` as a file that never existed. */
            xputs("init: fork failed for ");
            xputs(path);
            xputs("\n");
            continue;
        }
        if (pid == 0) {
            /* The child keeps `path` -- fork COWs the stack, so the parent
             * overwriting it for the next entry cannot move it out from
             * under an execve that has not run yet.  The directory fd is
             * ours to drop: the module wants the channel, not the listing. */
            kclose((int)mdfd);
            char *margv[] = {path, 0};
            char *menvp[] = {0};
            if (kexecve(path, margv, menvp) < 0) {
                xputs("init: exec ");
                xputs(path);
                xputs(" failed\n");
                kexit(1);
            }
            kexit(0); /* not reached: kexecve only returns on failure */
        }
        spawned++;
    }
    kclose((int)mdfd);

    /* Silence on success.  This line is the reader for the ORIGINAL bug in
     * this function -- two modules on the disk, none spawned, nothing said
     * -- so it prints only when the scan's outcome is worth a sentence:
     * nothing matched, or a file was skipped.  A healthy 2-files/2-spawned
     * boot says nothing, which is what a driver loading should look like. */
    if (spawned == 0) {
        xputs("init: ");
        xprinti(scanned);
        xputs(" file(s) in /mod, 0 spawned -- no module was started\n");
    }
}

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

    /* disk smoke test through AHCI+FAT32 */
    int dfd = kopen("/README.md", 0);
    if (dfd >= 0) {
        long n = kread(dfd, buf, sizeof(buf));
        if (n > 0) kwrite(1, buf, n);
        kclose(dfd);
    } else {
        xputs("init: no /README.md\n");
    }

    /* Before the terminal, so every line this scan can still produce (the
     * failure ones) lands ahead of the shell prompt instead of inside a
     * command's output.  The happy path prints nothing at all. */
    spawn_modules();

    /* Hand the console to a userspace terminal exactly once.  If the
     * terminal dies (e.g. `kill <pid>`), it stays dead: respawning it
     * would make the terminal unkillable.  init itself keeps running so
     * pid 1 still exists to reparent orphans. */
    long pid = kfork();
    if (pid == 0) {
        char *lsh_argv[] = {"/bin/lsh", 0};
        char *envp[] = {0};
        if (kexecve("/bin/lsh", lsh_argv, envp) < 0) {
            xputs("init: exec /bin/lsh failed\n");
            kexit(1);
        }
    }
    int st = 0;
    kwaitpid(pid, &st, 0);
    xputs("\ninit: terminal exited\n");

    /* P0-2: init is every orphan's reaper.  After this loop no corpse has
     * to outlive its reaper; with no children at all ECHILD would spin the
     * loop, so breathe once per second instead. */
    for (;;)
        if (kwaitpid(-1, &st, 0) == LNXRM_ECHILD) ksleep_ms(1000, 0);
    return 0;
}
