/* Virtual filesystem: path routing to a FAT32 root mounted at /,
 * plus synthesised device nodes under /dev and the fd table & surface. */
#include <sys/vfs.h>
#include <console.h>
#include <mm/mm.h>
#include <sys/sched.h>
#include <io.h>
#include <sys/cpu.h>
#include <disk.h>

/* ---- fat32 backend (fs/fat32.c) ---- */
extern struct fs_ops fat32_ops;
extern void *fat_mount(struct blkdev *b);

#define DISK_PREFIX "/"

/* Mounted filesystem info */
struct mounted_fs {
    struct fs_ops *ops;
    void *priv;
    char prefix[32];
    int prefix_len;
    bool active;
};

#define MAX_MOUNTS 4
static struct mounted_fs mounts[MAX_MOUNTS];
static int mount_count;
static bool disk_ready;

long console_read(struct file *f, void *buf, size_t n);
long console_write(struct file *f, const void *buf, size_t n);
int fat_dir_iter(struct dir_iter *it, struct dirent_out *d);

int copy_from_user(void *, const void *, size_t);
int copy_to_user(void *, const void *, size_t);
bool user_ptr_ok(u64 p, u64 n);

/* ---------------- routing ---------------- */
static struct fs_ops *fs_for_path(const char **path)
{
    /* Check each mounted filesystem */
    for (int i = 0; i < mount_count; i++) {
        if (!mounts[i].active) continue;

        if (!strncmp(*path, mounts[i].prefix, mounts[i].prefix_len)) {
            const char *rest = *path + mounts[i].prefix_len;
            /* root mount "/" matches everything */
            if (mounts[i].prefix_len == 1 && mounts[i].prefix[0] == '/') {
                while (*rest == '/') rest++;
                *path = rest;
                return mounts[i].ops;
            }
            if (*rest == '/' || *rest == '\0') {
                while (*rest == '/') rest++;
                *path = rest;
                return mounts[i].ops;
            }
        }
        /* Also match without leading '/' (e.g. "mnt" matches "/mnt") */
        if (mounts[i].prefix[0] == '/' &&
            !strncmp(*path, mounts[i].prefix + 1, mounts[i].prefix_len - 1)) {
            const char *rest = *path + mounts[i].prefix_len - 1;
            if (*rest == '/' || *rest == '\0') {
                while (*rest == '/') rest++;
                *path = rest;
                return mounts[i].ops;
            }
        }
    }
    return NULL; /* no mount owns this path */
}

static void *mount_priv(void *ops)
{
    for (int i = 0; i < mount_count; i++) {
        if (!mounts[i].active) continue;
        if (mounts[i].ops == ops) return mounts[i].priv;
    }
    return NULL;
}

/* Register a filesystem mount */
static int register_mount(struct fs_ops *ops, void *priv, const char *prefix)
{
    if (mount_count >= MAX_MOUNTS) return LNXRM_EFAIL;

    mounts[mount_count].ops = ops;
    mounts[mount_count].priv = priv;
    strncpy(mounts[mount_count].prefix, prefix, sizeof(mounts[mount_count].prefix) - 1);
    mounts[mount_count].prefix[sizeof(mounts[mount_count].prefix) - 1] = 0;
    mounts[mount_count].prefix_len = strlen(mounts[mount_count].prefix);
    mounts[mount_count].active = true;
    mount_count++;

    return 0;
}

/* Normalise a path: absolute paths pass through untouched (same pointer),
 * relative ones get '/' prefixed into `buf`.  A path that does not fit is
 * refused (NULL) rather than cut short -- a truncated path is a *different*
 * path, so the kernel would look up a file the caller never named (C46).
 * NULL means "no room": every caller turns it into LNXRM_ENAMETOOLONG.
 * `bufsz < 2` cannot hold "/" plus its terminator, which is the same
 * refusal; nothing is written in that case (every real caller passes
 * 128 bytes).  The boot self-test (ktest/t_fs.c) pins both ends:
 * a path that fits is byte-exact and never touches the byte after the
 * terminator, an overlong one is refused without being partially written. */
static const char *abs_path(const char *path, char *buf, size_t bufsz)
{
    if (path[0] == '/') return path;
    if (bufsz < 2) return NULL;
    buf[0] = '/';
    size_t i;
    for (i = 1; i < bufsz - 2 && path[i - 1]; i++) buf[i] = path[i - 1];
    buf[i] = 0;
    /* the loop stopped at the buffer, not at the end of the path -> no room */
    if (i >= bufsz - 2 && path[i - 1]) return NULL;
    return buf;
}

/* published for the boot self-test (ktest/t_fs.c): NULL when the
 * normalised path would not fit `buf` (C46 / LNXRM_ENAMETOOLONG) */
const char *vfs_abs_path(const char *path, char *buf, size_t bufsz)
{
    return abs_path(path, buf, bufsz);
}

/* ---- synthesised device nodes ----
 * FAT32 has no device files, so the nodes under /dev are built here instead
 * of living in a whole in-memory filesystem.  Elsewhere returns ENOENT. */
static int dev_getdent(void *mnt, void *dirnode, u64 *cookie, struct dirent_out *d)
{
    (void)mnt;
    (void)dirnode;
    if (*cookie) return LNXRM_EFAIL; /* exhausted */
    *cookie = 1;
    memset(d, 0, sizeof(*d));
    strncpy(d->name, "console", sizeof(d->name) - 1);
    d->name[sizeof(d->name) - 1] = 0;
    d->type = 3; /* DT_CHR */
    /* Synthesised node: there is no on-disk record, so give it a small
     * id from the reserved low range instead of leaving d_ino at 0. */
    d->ino = 1;
    return 0;
}

static struct fs_ops dev_ops = {
    .getdent = dev_getdent,
};

static int dev_lookup(const char *path, struct vnode *vn)
{
    char p[32];
    strncpy(p, path, sizeof(p) - 1);
    p[sizeof(p) - 1] = 0;
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] == '/') p[--n] = 0;

    if (!strcmp(p, "/dev")) {
        vn->type = V_DIR;
        vn->fs_data = NULL;
        vn->ops = &dev_ops;
        return 0;
    }
    if (!strcmp(p, "/dev/console")) {
        vn->type = V_CHR;
        vn->fs_data = NULL;
        return 0;
    }
    return LNXRM_ENOENT;
}

/* Resolve a path to a vnode, honouring O_CREAT and O_TRUNC. */
static long vfs_lookup(const char *path, int flags, struct vnode *vn)
{
    const char *p = path;
    memset(vn, 0, sizeof(*vn));
    if (!strcmp(path, "/")) {
        /* disk root when mounted at "/", otherwise nothing to serve */
        if (disk_ready) {
            struct fs_ops *ops = fs_for_path(&p);
            void *mnt = mount_priv(ops);
            if (ops && ops->lookup(mnt, p, vn) == 0) return 0;
        }
        return LNXRM_ENOENT;
    }

    struct fs_ops *ops = fs_for_path(&p);
    if (!ops) return dev_lookup(path, vn); /* unmounted: virtual nodes only */
    void *mnt = mount_priv(ops);
    /* defensive: a corrupted ops table must not kill the system */
    extern bool kern_text_ptr(u64 p);
    if (!kern_text_ptr((u64)ops->lookup)) return LNXRM_ENOENT;
    int lrc = ops->lookup(mnt, p, vn);
    if (lrc < 0) {
        if (dev_lookup(path, vn) == 0) return 0; /* e.g. /dev/console */
        /* T-005: "the disk would not answer" is not "there is no such file"
         * and not "you may not create it" -- an unreadable directory has to
         * reach the caller as EIO, or open() hides a dead disk behind ENOENT. */
        if (lrc == LNXRM_EIO) return LNXRM_EIO;
        if (!(flags & O_CREAT)) return LNXRM_ENOENT;      /* ENOENT */
        int crc = ops->create(mnt, p);
        if (crc < 0)
            return crc == LNXRM_EIO ? LNXRM_EIO : LNXRM_EACCES; /* EACCES-ish */
        int rrc = ops->lookup(mnt, p, vn);
        if (rrc < 0) return rrc == LNXRM_EIO ? LNXRM_EIO : LNXRM_ENOENT;
        vn->size = 0;
    } else if ((flags & O_TRUNC) && vn->type == V_REG) {
        /* T-005: a truncation that did not happen must fail the open --
         * returning the file would hand the caller something it believes
         * is empty (and O_TRUNC on a file that cannot be written is an
         * I/O error, not a silent success). */
        if (ops->write(mnt, vn->fs_data, 0, NULL, 0) < 0) return LNXRM_EIO;
        vn->size = 0;
    }
    return 0;
}

/* Wrap a resolved vnode in a struct file of the right flavour. */
static struct file *file_alloc(const struct vnode *vn, int flags)
{
    struct file *f = kmalloc(sizeof(*f));
    if (!f) return NULL;
    memset(f, 0, sizeof(*f));
    f->flags = flags;
    f->refcnt = 1;

    if (vn->type == V_CHR) {
        extern struct file_ops console_fops;
        f->ops = &console_fops;
        f->is_dir = false;
    } else if (vn->type == V_DIR) {
        extern struct file_ops dir_fops;
        f->ops = &dir_fops;
        f->is_dir = true;
        struct dir_iter *it = kmalloc(sizeof(*it));
        if (!it) { /* T-004: NULL is a real answer now, memset(NULL) is not */
            kfree(f);
            return NULL;
        }
        memset(it, 0, sizeof(*it));
        it->node = vn->fs_data;
        it->ops = vn->ops;
        it->mnt_data = mount_priv(vn->ops);
        f->priv = it;
    } else {
        extern struct file_ops reg_fops;
        f->ops = &reg_fops;
        f->priv = kmalloc(sizeof(*vn));
        if (!f->priv) {
            kfree(f);
            return NULL;
        }
        memcpy(f->priv, vn, sizeof(*vn));
    }
    return f;
}

long vfs_open_file(const char *path, int flags, struct file **out)
{
    char abuf[128];
    struct vnode vn;
    const char *p = abs_path(path, abuf, sizeof(abuf));
    if (!p) return LNXRM_ENAMETOOLONG;
    long err = vfs_lookup(p, flags, &vn);
    if (err < 0) return err;

    struct file *f = file_alloc(&vn, flags);
    if (!f) return LNXRM_ENOMEM;
    *out = f;
    return 0;
}

size_t vfs_file_size(struct file *f)
{
    /* C42: a character device carries no backing vnode -- file_alloc()
     * leaves priv NULL for V_CHR -- so there is no size to report. */
    if (!f || f->is_dir) return 0;
    struct vnode *vn = f->priv;
    if (!vn) return 0;
    return vn->size;
}

long vfs_read_file(struct file *f, void *buf, size_t n)
{ return f->ops->read(f, buf, n); }

void vfs_close_file(struct file *f)
{
    if (__sync_fetch_and_sub(&f->refcnt, 1) > 1) return;
    if (f->ops && f->ops->close) f->ops->close(f);
    kfree(f);
}

/* ---------------- fd-level operations ---------------- */
static struct file *fd_get(int fd)
{
    if (fd < 0 || fd >= NR_FDS || !current->fds[fd]) return NULL;
    return current->fds[fd];
}

long sys_open(const char *path, int flags)
{
    struct file *f = NULL;
    long err = vfs_open_file(path, flags, &f);
    if (err < 0) return err;
    for (int i = 0; i < NR_FDS; i++) {
        if (!current->fds[i]) {
            current->fds[i] = f;
            return i;
        }
    }
    vfs_close_file(f);
    return LNXRM_EMFILE;
}

long sys_close(int fd)
{
    struct file *f = fd_get(fd);
    if (!f) return LNXRM_EBADF;
    current->fds[fd] = NULL;
    vfs_close_file(f);
    return 0;
}

long sys_dup2(int oldfd, int newfd)
{
    if (oldfd == newfd) return fd_get(oldfd) ? newfd : LNXRM_EBADF;
    struct file *f = fd_get(oldfd);
    if (!f || newfd < 0 || newfd >= NR_FDS) return LNXRM_EBADF;
    if (current->fds[newfd]) sys_close(newfd);
    current->fds[newfd] = f;
    __sync_fetch_and_add(&f->refcnt, 1);
    return newfd;
}

long sys_lseek(int fd, long off, int whence)
{
    struct file *f = fd_get(fd);
    long base;

    if (!f) return LNXRM_EBADF;
    switch (whence) {
    case LNXRM_SEEK_SET: base = 0; break;
    case LNXRM_SEEK_CUR: base = (long)f->pos; break;
    case LNXRM_SEEK_END: base = (long)vfs_file_size(f); break;
    default: return LNXRM_EINVAL;
    }
    if (base + off < 0) return LNXRM_EINVAL; /* no negative offsets */
    f->pos = (u64)(base + off);
    return (long)f->pos;
}

long sys_mkdir(const char *path)
{
    char abuf[128];
    const char *norm = abs_path(path, abuf, sizeof(abuf));
    if (!norm) return LNXRM_ENAMETOOLONG;
    path = norm;
    const char *p = path;
    struct fs_ops *ops = fs_for_path(&p);
    void *mnt = mount_priv(ops);
    if (!ops || !ops->mkdir) return LNXRM_ENOSYS;
    return ops->mkdir(mnt, p);
}

long sys_unlink(const char *path)
{
    char abuf[128];
    const char *norm = abs_path(path, abuf, sizeof(abuf));
    if (!norm) return LNXRM_ENAMETOOLONG;
    path = norm;
    const char *p = path;
    struct fs_ops *ops = fs_for_path(&p);
    void *mnt = mount_priv(ops);
    if (!ops || !ops->unlink) return LNXRM_ENOSYS;
    return ops->unlink(mnt, p);
}

long sys_rmdir(const char *path)
{
    char abuf[128];
    const char *norm = abs_path(path, abuf, sizeof(abuf));
    if (!norm) return LNXRM_ENAMETOOLONG;
    path = norm;
    const char *p = path;
    struct fs_ops *ops = fs_for_path(&p);
    void *mnt = mount_priv(ops);
    if (!ops || !ops->rmdir) return LNXRM_ENOSYS;
    return ops->rmdir(mnt, p);
}

/* Route a two-path namespace operation to the filesystem owning both ends. */
static long vfs_rename_path(const char *oldpath, const char *newpath)
{
    char abuf1[128], abuf2[128];
    const char *o = abs_path(oldpath, abuf1, sizeof(abuf1));
    const char *n = abs_path(newpath, abuf2, sizeof(abuf2));
    if (!o || !n) return LNXRM_ENAMETOOLONG;
    oldpath = o;
    newpath = n;
    const char *p1 = oldpath, *p2 = newpath;
    struct fs_ops *o1 = fs_for_path(&p1);
    struct fs_ops *o2 = fs_for_path(&p2);
    if (!o1 || !o2) return LNXRM_ENOENT;
    if (o1 != o2) return LNXRM_EXDEV; /* rename(2) across filesystems */
    if (!o1->rename) return LNXRM_ENOSYS;
    return o1->rename(mount_priv(o1), p1, p2);
}

long sys_rename(const char *oldpath, const char *newpath)
{ return vfs_rename_path(oldpath, newpath); }

long sys_read(int fd, void *ubuf, size_t n)
{
    struct file *f = fd_get(fd);
    if (!f) return LNXRM_EBADF;
    if (f->is_dir) return LNXRM_EFAIL;
    /* Per-syscall stack buffer: the old static kbuf was shared by all
     * tasks and broke under concurrent reads (SMP + preemption). */
    char kbuf[4096];
    if (n > sizeof(kbuf)) n = sizeof(kbuf);
    long r = f->ops->read(f, kbuf, n);
    if (r > 0 && copy_to_user(ubuf, kbuf, r) < 0) return LNXRM_EFAULT;
    return r;
}

long sys_write(int fd, const void *ubuf, size_t n)
{
    struct file *f = fd_get(fd);
    if (!f) return LNXRM_EBADF;
    char kbuf[4096];
    if (n > sizeof(kbuf)) n = sizeof(kbuf);
    if (copy_from_user(kbuf, ubuf, n) < 0) return LNXRM_EFAULT;
    return f->ops->write(f, kbuf, n);
}

long sys_getdent(int fd, void *ubuf, size_t len)
{
    struct file *f = fd_get(fd);
    if (!f || !f->is_dir) return LNXRM_EBADF;
    char kbuf[1024];
    if (len > sizeof(kbuf)) len = sizeof(kbuf);
    long r = f->ops->getdent(f, kbuf, len);
    if (r > 0 && copy_to_user(ubuf, kbuf, r) < 0) return LNXRM_EFAULT;
    return r;
}

/* ---------------- generic file/dir fops bridging to fs_ops -------------- */

static long reg_read(struct file *f, void *buf, size_t n)
{
    struct vnode *vn = f->priv;
    long r = vn->ops->read(vn->mnt_data, vn->fs_data, f->pos, buf, n);
    if (r > 0) f->pos += r;
    return r;
}

static long reg_write(struct file *f, const void *buf, size_t n)
{
    struct vnode *vn = f->priv;
    long r = vn->ops->write(vn->mnt_data, vn->fs_data, f->pos, buf, n);
    if (r > 0) f->pos += r;
    return r;
}

static long dir_getdent(struct file *f, void *buf, size_t n)
{
    struct dir_iter *it = f->priv;
    struct dirent_out d;
    long cnt = 0;
    char *p = buf;
    /* T-005: the scan already died -- keep saying so.  Restarting from the
     * old cookie would present the unread remainder as "end of directory". */
    if (it->err < 0) return it->err;
    while ((size_t)(cnt + (long)sizeof(struct lnxrm_dirent)) <= n) {
        int (*fn)(void *, void *, u64 *, struct dirent_out *) =
            it->ops ? it->ops->getdent : NULL;

        if (!fn) break;

        /* Zero first: d_ino must never be whatever was on the stack if a
         * filesystem leaves it unset. */
        memset(&d, 0, sizeof(d));
        int rc = fn(it->mnt_data, it->node, &it->cookie, &d);
        if (rc < 0) {
            /* Surface it now if nothing was gathered yet, else on the next
             * call -- either way a failed read is never a clean end-of-dir. */
            it->err = rc;
            return cnt ? cnt : rc;
        }
        if (rc == 0) break; /* the filesystem says the listing ended */
        struct lnxrm_dirent e;
        memset(&e, 0, sizeof(e));
        e.d_ino = d.ino;
        strncpy(e.d_name, d.name, 55);
        e.d_type = d.type;
        memcpy(p + cnt, &e, sizeof(e));
        cnt += sizeof(e);
    }
    return cnt;
}

int fat_dir_iter(struct dir_iter *it, struct dirent_out *d);

static int noop_close(struct file *f)
{ return 0; }
static long noop_read(struct file *f, void *buf, size_t n)
{
    (void)f;
    (void)buf;
    (void)n;
    return LNXRM_EFAIL;
}
static long noop_write(struct file *f, const void *buf, size_t n)
{
    (void)f;
    (void)buf;
    (void)n;
    return LNXRM_EFAIL;
}

static int dir_close(struct file *f)
{
    kfree(f->priv);
    return 0;
}

struct file_ops console_fops = {
    .read = console_read,
    .write = console_write,
    .close = noop_close,
};

struct file_ops reg_fops = {
    .read = reg_read,
    .write = reg_write,
    .close = noop_close,
};

struct file_ops dir_fops = {
    .read = noop_read,
    .write = noop_write,
    .getdent = dir_getdent,
    .close = dir_close,
};

/* console character device */
long console_read(struct file *f, void *buf, size_t n)
{
    char *p = buf;
    size_t got = 0;
    extern int lnxrm_uart_trygetc(void);
    while (got < n) {
        int c = input_pop();
        if (c < 0) c = lnxrm_uart_trygetc(); /* polled fallback */
        if (c >= 0) {
            p[got++] = (char)c;
            if (c == '\n') break;
            continue;
        }

        /* No input available.  A signal (EINTR) beats sleeping. */
        if (current && current->signal_pending) return got ? (long)got : -4; /* -EINTR */

        /* Sleep until the next tick (1 jiffy safety net) or until
         * input_push() wakes us via console_waiter.  Arm the waiter
         * AFTER setting SLEEPING so a racing push either sees
         * T_SLEEPING and wakes us, or leaves a char that the recheck
         * below / the next loop iteration will pick up. */
        if (!current) return got ? (long)got : -5; /* -EIO, no task context */

        current->state = T_SLEEPING;
        current->sleep_until = jiffies + 1;
        console_waiter_arm(current);
        /* Recheck once more with the waiter armed: an input_push that
         * landed between the empty check and T_SLEEPING would not have
         * woken us (state wasn't SLEEPING yet) — but the char is now
         * in the ring. */
        c = input_pop();
        if (c < 0) c = lnxrm_uart_trygetc();
        if (c >= 0) {
            current->state = T_RUNNING;
            console_waiter_disarm(current);
            p[got++] = (char)c;
            if (c == '\n') break;
            continue;
        }

        runqueue_remove(current); /* off-queue invariant before sleep */
        schedule();
        console_waiter_disarm(current);
        current->state = T_RUNNING;
    }
    return got;
}

long console_write(struct file *f, const void *buf, size_t n)
{
    /* Shares print_lock with kprintf so lines from two CPUs cannot
     * interleave. Chunked: console_putc polls the UART, so one big
     * write would otherwise hold IRQs off for its whole duration. */
    const char *p = buf;
    size_t left = n;
    while (left) {
        size_t chunk = left > 64 ? 64 : left;
        u64 flags;
        console_out_lock(&flags);
        for (size_t i = 0; i < chunk; i++) console_putc(p[i]);
        console_out_unlock(flags);
        p += chunk;
        left -= chunk;
    }
    return n;
}

/* ---------------- boot-time mounting ---------------- */
void vfs_init(void)
{
    blk_cache_init();
}

/* True once a real filesystem answers at "/".  Lets the boot self-test skip
 * root-path assertions on a diskless boot instead of reporting them failed. */
bool vfs_root_ready(void) { return disk_ready; }

struct blkdev *blk_first;
static struct blkdev *blk_list[8];
static int blk_count;

void blk_register(struct blkdev *b)
{
    if (blk_count < 8) blk_list[blk_count++] = b;
    if (!blk_first) blk_first = b;
    kprintf("[blk] registered %s (%llu sectors, %llu MiB)\n", b->name, b->num_sectors,
            b->num_sectors / 2048);
}

int blk_list_all(void *ubuf, int max)
{
    int n = blk_count < max ? blk_count : max;
    for (int i = 0; i < n; i++) {
        struct blkdev *b = blk_list[i];
        /* pack: name[16] + sector_size(u32) + num_sectors(u64) = 28 bytes */
        char tmp[28];
        memset(tmp, 0, sizeof(tmp));
        int len = 0;
        while (b->name[len] && len < 15) tmp[len] = b->name[len], len++;
        *(u32 *)(tmp + 16) = b->sector_size;
        *(u64 *)(tmp + 20) = b->num_sectors;
        if (copy_to_user((char *)ubuf + i * 28, tmp, 28) < 0) return i;
    }
    return n;
}

/* Try ONE device: whole-disk FAT32 first, then its MBR partitions.  Returns 0
 * as soon as something is mounted at "/". */
static int vfs_try_mount_one(struct blkdev *bd)
{
    kprintf("[vfs] attempting disk mount on %s (%llu sectors, %llu MiB)\n", bd->name,
            bd->num_sectors, bd->num_sectors / 2048);

    /* Flush cache before mounting to ensure consistent state */
    blk_cache_flush(bd);

    /* 1. Try whole disk as FAT32 (most common: no MBR partition table) */
    kprintf("[vfs] trying FAT32 (whole disk)...\n");
    void *m = fat_mount(bd);
    if (m) {
        register_mount(&fat32_ops, m, DISK_PREFIX);
        disk_ready = true;
        kprintf("[vfs] FAT32 mounted at %s (whole disk)\n", DISK_PREFIX);
        blk_cache_flush(bd);
        return 0;
    }

    /* 2. Check for MBR partition table */
    kprintf("[vfs] trying MBR partition table...\n");
    struct mbr_info mbr;
    if (mbr_parse(bd, &mbr) == 0 && mbr.part_count > 0) {
        kprintf("[vfs] MBR detected, %d partitions\n", mbr.part_count);

        for (int i = 0; i < mbr.part_count; i++) {
            if (mbr.parts[i].type == PART_TYPE_NONE) continue;

            kprintf("[vfs] partition %d: type=0x%02X (%s)\n", i, mbr.parts[i].type,
                    mbr_type_name(mbr.parts[i].type));

            /* Heap-allocate: fat_mount stores dev pointer in fat_priv */
            struct blkdev *part_dev = kmalloc(sizeof(*part_dev));
            if (!part_dev) continue;

            if (mbr_get_partition(&mbr, i, part_dev, bd) == 0) {
                void *pm = NULL;

                if (mbr.parts[i].type == PART_TYPE_FAT32 ||
                    mbr.parts[i].type == PART_TYPE_FAT32_LBA) {
                    kprintf("[vfs]   trying FAT32 on partition %d...\n", i);
                    pm = fat_mount(part_dev);
                    if (pm) {
                        register_mount(&fat32_ops, pm, DISK_PREFIX);
                        disk_ready = true;
                        kprintf("[vfs] FAT32 mounted at %s (partition %d)\n", DISK_PREFIX, i);
                        blk_cache_flush(bd);
                        return 0;
                    }
                }
            }
            kfree(part_dev);
        }

        if (disk_ready) {
            blk_cache_flush(bd);
            return 0;
        }
    }

    return LNXRM_EFAIL;
}

int vfs_try_mount_disk(void)
{
    /* T-081 / C28: this used to be "mount blk_first, or give up", and
     * blk_first is whichever driver called blk_register() first.  ide_init()
     * runs before ahci_init() (main.c), so on hardware where the legacy IDE
     * port answers but has nothing usable, a speculative probe that found a
     * phantom disk locked out the real one behind it.  The policy is now
     * "first device that actually mounts" -- what the function's name always
     * promised -- and every device gets its own attempt line, so a machine
     * that has to fall through to the second disk says so in the log. */
    if (!blk_count) return LNXRM_EFAIL;
    for (int i = 0; i < blk_count; i++)
        if (vfs_try_mount_one(blk_list[i]) == 0) return 0;
    return LNXRM_EFAIL;
}
