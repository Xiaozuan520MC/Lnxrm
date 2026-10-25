/* /bin/mkdir -- create a directory. */
#include "ulib.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        xputs("mkdir: missing operand\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        long r = kmkdir(argv[i]);
        if (r < 0) {
            xputs("mkdir: cannot create directory '");
            xputs(argv[i]);
            xputs("'\n");
            return 1;
        }
    }
    return 0;
}
