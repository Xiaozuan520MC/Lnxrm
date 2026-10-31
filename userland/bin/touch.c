/* /bin/touch -- create an empty file. */
#include "ulib.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        xputs("touch: missing file operand\n");
        return 1;
    }
    long fd = kopen(argv[1], 0100 | 1);
    if (fd < 0) {
        xputs("touch: cannot create '");
        xputs(argv[1]);
        xputs("'\n");
        return 1;
    }
    kclose(fd);
    return 0;
}
