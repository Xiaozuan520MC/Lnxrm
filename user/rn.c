/* /bin/rn -- change the name of a file or directory (SYS_rename). */
#include "ulib.h"

static void report(const char *why, const char *from, const char *to, long err)
{
    xputs("rn: ");
    xputs(why);
    xputs(" '");
    xputs(from);
    xputs("' -> '");
    xputs(to);
    xputs("' (errno ");
    xprinti(-err);
    xputs(")\n");
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        xputs("usage: rn <old-name> <new-name>\n");
        return 1;
    }
    long r = krename(argv[1], argv[2]);
    if (r < 0) {
        report("cannot rename", argv[1], argv[2], r);
        return 1;
    }
    return 0;
}
