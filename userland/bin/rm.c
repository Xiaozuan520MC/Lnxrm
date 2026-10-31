/* /bin/rm -- remove files or directories.
 * rm <file>       delete a file
 * rm -p <dir>     delete a directory (recursively)
 */
#include "ulib.h"

static int rm_dir(const char *path);

static int rm_file(const char *path)
{
    long r = kunlink(path);
    if (r < 0) {
        xputs("rm: cannot remove '");
        xputs(path);
        xputs("'\n");
        return 1;
    }
    return 0;
}

static int rm_dir(const char *path)
{
    /* read all entries, recursively delete children */
    long fd = kopen(path, 0);
    if (fd < 0) {
        xputs("rm: cannot open '");
        xputs(path);
        xputs("'\n");
        return 1;
    }

    struct lnxrm_dirent entries[64];
    int count = 0;
    struct lnxrm_dirent e;
    while (count < 64 && kgetdent(fd, &e, sizeof(e)) > 0) entries[count++] = e;
    kclose(fd);

    int err = 0;
    char child[256];

    for (int i = 0; i < count; i++) {
        /* build child path: path/name */
        int j = 0;
        const char *p = path;
        while (*p && j < 250) child[j++] = *p++;
        if (j > 0 && child[j - 1] != '/') child[j++] = '/';
        const char *n = entries[i].d_name;
        while (*n && j < 250) child[j++] = *n++;
        child[j] = 0;

        if (entries[i].d_type == 4)
            err |= rm_dir(child);
        else
            err |= rm_file(child);
    }

    /* remove the now-empty directory itself */
    long r = krmdir(path);
    if (r < 0) {
        xputs("rm: cannot remove directory '");
        xputs(path);
        xputs("'\n");
        return 1;
    }
    return err;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        xputs("rm: missing operand\n");
        return 1;
    }

    int recursive = 0;
    int start = 1;

    if (argc >= 3 && argv[1][0] == '-' && argv[1][1] == 'p') {
        recursive = 1;
        start = 2;
    }

    int err = 0;
    for (int i = start; i < argc; i++) {
        if (recursive)
            err |= rm_dir(argv[i]);
        else
            err |= rm_file(argv[i]);
    }
    return err;
}
