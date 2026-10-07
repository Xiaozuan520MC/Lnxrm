/* /bin/mv -- move a file or directory into another directory, or to a new
 * name when the destination is not a directory.  This is the usual mv
 * behaviour; the path join (destdir/<basename>) happens here, so the kernel
 * only ever sees SYS_rename. */
#include "ulib.h"

/* Does `path` exist and is it a directory?  sys_getdent() rejects
 * non-directories with -EBADF, so a successful call proves it is one. */
static int is_dir(const char *path)
{
    long fd = kopen(path, 0);
    if (fd < 0) return 0;
    struct lnxrm_dirent e;
    long r = kgetdent(fd, &e, sizeof(e));
    kclose(fd);
    return r >= 0;
}

/* Trailing slashes are meaningless on either end of a move. */
static void strip_trailing_slashes(char *p)
{
    size_t n = xstrlen(p);
    while (n > 1 && p[n - 1] == '/') p[--n] = 0;
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t i;
    for (i = 0; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

/* out := <destdir>/<basename(src)>; `src` is stripped in place so the same
 * buffer can be handed to rename as the old path.  0, or -EINVAL. */
static long build_dest(char *src, const char *destdir, char *out, size_t cap)
{
    char dbuf[128];
    const char *base = src;
    size_t i, dl, bl;

    strip_trailing_slashes(src);
    copy_str(dbuf, sizeof(dbuf), destdir);
    strip_trailing_slashes(dbuf);

    for (const char *p = src; *p; p++)
        if (*p == '/') base = p + 1;
    if (!*base) return LNXRM_EINVAL; /* refused to move the root */

    dl = xstrlen(dbuf);
    bl = xstrlen(base);
    if (dl + bl + 2 > cap) return LNXRM_EINVAL;
    for (i = 0; i < dl; i++) out[i] = dbuf[i];
    if (dl == 0 || dbuf[dl - 1] != '/') out[dl++] = '/';
    for (i = 0; i <= bl; i++) out[dl + i] = base[i];
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        xputs("usage: mv <source> <dest-dir | new-name>\n");
        return 1;
    }
    const char *src = argv[1];
    const char *dst = argv[2];
    size_t n = xstrlen(dst);
    int into_dir = (n > 0 && dst[n - 1] == '/') || is_dir(dst);

    long r;
    if (into_dir) {
        char sbuf[128], dest[256];
        copy_str(sbuf, sizeof(sbuf), src);
        r = build_dest(sbuf, dst, dest, sizeof(dest));
        if (r == 0) r = krename(sbuf, dest);
    } else {
        r = krename(src, dst);
    }

    if (r < 0) {
        xputs("mv: cannot move '");
        xputs(src);
        xputs("' to '");
        xputs(dst);
        xputs("' (errno ");
        xprinti(-r);
        xputs(")\n");
        return 1;
    }
    return 0;
}
