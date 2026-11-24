// SPDX-License-Identifier: Apache-2.0

/* /bin/cat -- print a file, or standard input when no operand is given
 * (that is what lets it sit at the end of a shell pipeline). */
#include "ulib.h"

int main(int argc, char **argv)
{
    const char *name = "stdin";
    long fd = 0; /* the terminal, or the pipe the shell wired in */

    if (argc >= 2) {
        name = argv[1];
        fd = kopen(name, 0);
        if (fd < 0) {
            xputs("cat: cannot open '");
            xputs(name);
            xputs("'\n");
            return 1;
        }
    }

    char buf[256];
    long n;
    while ((n = kread(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = 0;
        xputs(buf);
    }
    if (n < 0) {
        xputs("cat: '");
        xputs(name);
        xputs("': Is a directory or error\n");
    }
    if (fd > 0)
        kclose(fd); /* never close the inherited stdin */
    return n < 0 ? 1 : 0;
}
