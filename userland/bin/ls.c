/* /bin/ls -- list directory entries, dirs get trailing /. */
#include "ulib.h"

int main(int argc, char **argv)
{
    const char *path = argc >= 2 ? argv[1] : "/";
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
