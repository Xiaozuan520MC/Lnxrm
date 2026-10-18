/* initramfs -- the newc cpio archive that `make INITRAMFS=` links into the
 * kernel image, served read-only as "/" when no disk can supply /bin/init.
 *
 * The archive is never unpacked.  lookup, getdent and read all address the
 * very bytes the loader already had to carry into memory, so the only RAM
 * this costs is one index record per archive entry (24 bytes for bin/init),
 * there is no heap traffic to fail halfway through a mount, and no copy that
 * could disagree with the archive.  The price is one linear scan per lookup,
 * which is what a boot that opens a handful of paths can afford.
 *
 * newc wire format (Documentation/filesystems/ramfs-rootfs-initramfs.rst):
 * every record is a 110-byte ASCII header -- '070701' or '070702' followed
 * by thirteen fields of eight hex digits -- then the NUL-terminated name,
 * then the file body, each part padded to a 4-byte boundary measured from
 * the start of the archive.  A record named TRAILER!!! closes the archive.
 *
 * Read-only, deliberately: the alternative is unpacking into a heap-backed
 * ramfs, which turns a "boot from a fallback image" path into an allocator
 * dependency and doubles the memory an initramfs costs.  Every mutating
 * fs_ops entry answers LNXRM_EACCES, so a shell that tries to write gets an
 * error instead of the kernel walking off the end of a const buffer. */
#include <sys/vfs.h>
#include <console.h>
#include <mm/mm.h>

/* Both symbols come from arch/kernel.ld, which defines them unconditionally:
 * with no INITRAMFS= the .initramfs section is empty and the two coincide,
 * which is how this file tells "no archive was linked" from "an empty one".
 * They are VMAs, and the archive sits in .image -- part of the region
 * pmm_init() keeps out of the buddy -- so the pointer is usable as-is. */
extern const u8 __initramfs_start[];
extern const u8 __initramfs_end[];

#define HDR_SIZE 110
#define F_MODE   14 /* header field offsets: magic is 6 bytes, then 8 per */
#define F_FILESZ 54 /* field, in the order below */
#define F_NAMESZ 94

#define S_IFMT  0170000
#define S_IFDIR 0040000
#define S_IFREG 0100000

/* Published to vfs.c, which owns the mount table; declared here because
 * irfs_lookup() stamps it into every vnode it hands out. */
extern struct fs_ops irfs_ops;

struct irfs_ent {
    const char *name; /* NUL-terminated, points into the archive */
    u32 name_len;
    u64 data; /* offset of the file body inside the archive */
    u32 size;
    u32 mode;
    bool dir;
};

struct irfs {
    const u8 *base;
    u64 size;
    u32 nent;
    struct irfs_ent *ents;
};

/* 8 lower-case or upper-case hex digits; the format accepts either and a
 * header that is not hex at all is a truncated or non-cpio image, which the
 * caller has to stop on rather than carry into an offset. */
static int hex8(const char *s, u32 *out)
{
    u32 v = 0;
    for (int i = 0; i < 8; i++) {
        char c = s[i];
        u32 d;
        if (c >= '0' && c <= '9') d = (u32)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (u32)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (u32)(c - 'A' + 10);
        else return -1;
        v = (v << 4) | d;
    }
    *out = v;
    return 0;
}

/* Walk every record of the archive.
 *
 * With ents == NULL this is the counting pass; otherwise it stores up to
 * `cap` records and returns the same total, so the two passes must agree or
 * the index would be shorter than the archive it indexes -- the caller
 * checks that instead of trusting either number.  Every offset is derived
 * from the header and then range-checked against the archive, so a
 * corrupt size field ends the scan where it is rather than producing an
 * entry that points outside the image. */
static u32 irfs_scan(const u8 *base, u64 size, struct irfs_ent *ents, u32 cap)
{
    u32 n = 0;
    u64 off = 0;

    while (off + HDR_SIZE <= size) {
        const char *h = (const char *)(base + off);
        /* '070701' is plain newc, '070702' is the checksummed variant: the
         * checksum field is the one thing this driver does not verify, so
         * both are accepted and read identically. */
        if (memcmp(h, "070701", 6) && memcmp(h, "070702", 6)) break;

        u32 mode, fsize, nsize;
        if (hex8(h + F_MODE, &mode) < 0) break;
        if (hex8(h + F_FILESZ, &fsize) < 0) break;
        if (hex8(h + F_NAMESZ, &nsize) < 0) break;

        u64 name_off = off + HDR_SIZE;
        if (!nsize || name_off + nsize > size) break;
        const char *name = (const char *)(base + name_off);
        if (name[nsize - 1] != 0) break; /* not NUL-terminated: refuse */

        u64 data_off = ALIGN_UP(name_off + nsize, 4);
        if (data_off > size || data_off + fsize > size) break;

        if (!strcmp(name, "TRAILER!!!")) return n; /* archive ends here */

        if (ents && n < cap) {
            struct irfs_ent *e = &ents[n];
            e->name = name;
            e->name_len = nsize - 1;
            e->data = data_off;
            e->size = fsize;
            e->mode = mode;
            e->dir = (mode & S_IFMT) == S_IFDIR;
        }
        n++;
        off = ALIGN_UP(data_off + fsize, 4);
    }
    return n;
}

/* Mount the linked-in archive.  NULL means "there is nothing to mount": no
 * INITRAMFS= at build time (the two linker symbols coincide), or bytes that
 * do not begin with a newc header.  The caller turns that into ENOENT. */
void *irfs_mount(void)
{
    const u8 *base = __initramfs_start;
    u64 size = (u64)(__initramfs_end - __initramfs_start);
    if (size < HDR_SIZE) return NULL;

    u32 n = irfs_scan(base, size, NULL, 0);
    if (!n) {
        kprintf("[initramfs] %llu bytes but no newc records -- not an archive\n", size);
        return NULL;
    }

    struct irfs_ent *ents = kmalloc(sizeof(*ents) * n);
    if (!ents) return NULL;
    if (irfs_scan(base, size, ents, n) != n) {
        /* The two passes saw different archives, i.e. something wrote into
         * the image between them.  Half an index is worse than none. */
        kfree(ents);
        return NULL;
    }

    struct irfs *fs = kmalloc(sizeof(*fs));
    if (!fs) {
        kfree(ents);
        return NULL;
    }
    fs->base = base;
    fs->size = size;
    fs->nent = n;
    fs->ents = ents;
    kprintf("[initramfs] %u records, %llu KiB, read-only at %p\n", n, size >> 10, base);
    return fs;
}

/* A path as the VFS hands it to a mounted filesystem: no mount prefix, but
 * possibly a leading or trailing '/' (and the shell does open "/.").
 *
 * Returns the length after stripping, or IRFS_ROOT for "this names the
 * root" -- which has no archive record of its own, since archives usually
 * carry a '.' entry and synthesising the root keeps that cosmetic detail out
 * of every listing -- or IRFS_TOO_LONG when the path does not fit.  A path
 * that was cut short would be a *different* path (this is the same hazard
 * vfs.c's abs_path() refuses, C46), so it is refused rather than truncated
 * into something that might name a real entry. */
#define IRFS_ROOT     ((size_t)-1)
#define IRFS_TOO_LONG ((size_t)-2)

static size_t irfs_norm(const char *path, char *buf, size_t bufsz)
{
    size_t n = 0;
    while (*path == '/') path++;
    while (*path) {
        if (n + 1 >= bufsz) return IRFS_TOO_LONG;
        buf[n++] = *path++;
    }
    while (n && buf[n - 1] == '/') n--;
    buf[n] = 0;
    if (n == 1 && buf[0] == '.') n = 0; /* "/." is the root, not a name */
    return n ? n : IRFS_ROOT;
}

static int irfs_lookup(void *mnt, const char *path, struct vnode *out)
{
    struct irfs *fs = mnt;
    char norm[128];
    size_t len = irfs_norm(path, norm, sizeof(norm));

    if (len == IRFS_TOO_LONG) return LNXRM_ENAMETOOLONG;

    if (len == IRFS_ROOT) { /* the root directory */
        out->type = V_DIR;
        out->size = 0;
        out->fs_data = NULL; /* NULL names the root; getdent copes */
        out->ops = &irfs_ops;
        out->mnt_data = mnt;
        return 0;
    }

    for (u32 i = 0; i < fs->nent; i++) {
        struct irfs_ent *e = &fs->ents[i];
        if (e->name_len != len) continue;
        if (strncmp(e->name, norm, len)) continue;
        out->type = e->dir ? V_DIR : V_REG;
        out->size = e->size;
        out->fs_data = e;
        out->ops = &irfs_ops;
        out->mnt_data = mnt;
        return 0;
    }
    return LNXRM_ENOENT;
}

static int irfs_read(void *mnt, void *node, u64 off, void *buf, size_t n)
{
    struct irfs *fs = mnt;
    struct irfs_ent *e = node;
    if (!e || e->dir) return LNXRM_EFAIL; /* not a byte stream */
    if (off >= e->size) return 0;         /* EOF */
    if (n > e->size - off) n = e->size - off;
    /* fs_ops.read returns int and reg_read() passes it straight on, so keep
     * a request that a huge file could overflow inside int range; nothing in
     * this kernel reads that much at once (kernel_spawn slurps whole
     * images, and the heap caps those long before this matters). */
    if (n > 0x7FFFFFFFu) n = 0x7FFFFFFFu;
    memcpy(buf, fs->base + e->data + off, n);
    return (int)n;
}

/* One entry of `dir`'s children.  *cookie is the archive index to resume
 * from and is advanced past what is returned, so a listing costs one scan
 * and a caller that stops early does not pay for the rest. */
static int irfs_getdent(void *mnt, void *dirnode, u64 *cookie, struct dirent_out *d)
{
    struct irfs *fs = mnt;
    const struct irfs_ent *dir = dirnode; /* NULL == the root */
    const char *prefix = dir ? dir->name : "";
    size_t plen = dir ? dir->name_len : 0;

    for (u64 i = *cookie; i < fs->nent; i++) {
        const struct irfs_ent *e = &fs->ents[i];
        if (e->name_len <= plen) continue;
        if (strncmp(e->name, prefix, plen)) continue;
        if (plen && e->name[plen] != '/') continue;
        const char *rest = e->name + plen + (plen ? 1 : 0);
        size_t rlen = e->name_len - plen - (plen ? 1 : 0);
        if (strchr(rest, '/')) continue; /* a child of a deeper directory */
        if (!rlen || rlen >= sizeof(d->name)) continue;

        memset(d, 0, sizeof(*d));
        memcpy(d->name, rest, rlen);
        /* Same d_type encoding fat32_dir.c reports: 4 = directory,
         * 8 = regular file.  The shell and ls.c switch on those. */
        d->type = e->dir ? 4 : 8;
        d->ino = i + 1; /* stable within the archive, never 0 */
        *cookie = i + 1;
        return 1;
    }
    return 0; /* the listing ended */
}

/* ---- read-only ---------------------------------------------------------- *
 * Every mutator answers EACCES rather than being left NULL: vfs.c calls
 * these through the ops table without a NULL check (reg_write does it
 * unconditionally), so a hole in the table is a page fault in the kernel,
 * not an error in the shell. */
static int irfs_write(void *mnt, void *node, u64 off, const void *buf, size_t n)
{
    (void)mnt;
    (void)node;
    (void)off;
    (void)buf;
    (void)n;
    return LNXRM_EACCES;
}
static int irfs_create(void *mnt, const char *path)
{
    (void)mnt;
    (void)path;
    return LNXRM_EACCES;
}
static int irfs_mkdir(void *mnt, const char *path)
{
    (void)mnt;
    (void)path;
    return LNXRM_EACCES;
}
static int irfs_unlink(void *mnt, const char *path)
{
    (void)mnt;
    (void)path;
    return LNXRM_EACCES;
}
static int irfs_rmdir(void *mnt, const char *path)
{
    (void)mnt;
    (void)path;
    return LNXRM_EACCES;
}
static int irfs_rename(void *mnt, const char *oldpath, const char *newpath)
{
    (void)mnt;
    (void)oldpath;
    (void)newpath;
    return LNXRM_EACCES;
}
static u64 irfs_freespace(void *mnt)
{
    (void)mnt;
    return 0;
}

struct fs_ops irfs_ops = {
    .lookup = irfs_lookup,
    .getdent = irfs_getdent,
    .read = irfs_read,
    .write = irfs_write,
    .create = irfs_create,
    .mkdir = irfs_mkdir,
    .unlink = irfs_unlink,
    .rmdir = irfs_rmdir,
    .rename = irfs_rename,
    .freespace = irfs_freespace,
};
