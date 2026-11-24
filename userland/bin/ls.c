// SPDX-License-Identifier: Apache-2.0

/* /bin/ls -- list directory entries, dirs get trailing /. */
#include "ulib.h"

int main(int argc, char **argv)
{
    /* No operand means "here", not "/": cd moved the shell's cwd (chdir
     * 49) and fork copied it into this process, so ask the kernel (getcwd
     * 50) where "here" is.  The old hard-coded "/" opened the root from
     * every directory, which made a bare ls lie after a cd.  One 128-byte
     * buffer always holds the answer: chdir refuses to store a cwd longer
     * than 127 bytes, so getcwd cannot report ERANGE here and the "/"
     * below is a defensive fallback for a failed syscall, not a default. */
    char cwdbuf[128];
    const char *path;

    if (argc >= 2) {
        path = argv[1];
    } else if (kgetcwd(cwdbuf, sizeof(cwdbuf)) == 0) {
        path = cwdbuf;
    } else {
        path = "/";
    }
    long fd = kopen(path, 0);
    if (fd < 0) {
        xputs("ls: cannot open '");
        xputs(path);
        xputs("'\n");
        return 1;
    }
    struct lnxrm_dirent entries[64];
    int count = 0;
    struct lnxrm_dirent e;
    while (count < 64 && kgetdent(fd, &e, sizeof(e)) > 0) entries[count++] = e;
    kclose(fd);

    if (count == 0) return 0;

    for (int i = 0; i < count; i++) {
        if (i > 0) putc_(' ');
        xputs(entries[i].d_name);
        if (entries[i].d_type == 4) putc_('/');
    }
    putc_('\n');
    return 0;
}
