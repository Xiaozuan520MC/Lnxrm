/* /bin/echo -- print arguments to stdout. */
#include "ulib.h"

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (i > 1) putc_(' ');
        xputs(argv[i]);
    }
    putc_('\n');
    return 0;
}
