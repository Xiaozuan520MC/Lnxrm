/* /bin/cat -- read a file and print to stdout. */
#include "ulib.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        xputs("cat: missing file operand\n");
        return 1;
    }
    long fd = kopen(argv[1], 0);
    if (fd < 0) {
        xputs("cat: cannot open '");
        xputs(argv[1]);
        xputs("'\n");
        return 1;
    }
    char buf[256];
    long n;
    while ((n = kread(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = 0;
        xputs(buf);
    }
    if (n < 0) {
        xputs("cat: '");
        xputs(argv[1]);
        xputs("': Is a directory or error\n");
    }
    kclose(fd);
    return n < 0 ? 1 : 0;
}
