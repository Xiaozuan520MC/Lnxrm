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
#include <fat32.h> /* C51: fat_priv, set here when a volume takes "/" */

/* ---- initramfs backend (fs/initramfs.c) ---- */
extern struct fs_ops irfs_ops;
extern void *irfs_mount(void);

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

int copy_from_user(void *, const void *, size_t);
int copy_to_user(void *, const void *, size_t);
bool user_ptr_ok(u64 p, u64 n);
bool user_ptr_writable(u64 p, u64 n);

/* ---------------- routing ----------------
 *
 * C39/C47: a path is answered by ONE mount entry, and the entry -- not the
 * ops table -- is what the caller keeps.  ops+priv are a pair: looking them
 * up separately (ops first, then "the priv of whichever mount has that ops")
 * is what let two mounts of the same filesystem hand back the wrong volume,
 * and it is why vnode.mnt_data used to be a placeholder rather than the
 * mount the path was actually resolved through.
 *
 * Longest matching prefix wins.  This is not cosmetic: the root mount "/"
 * is a prefix of *every* absolute path, so a first-match scan would never
 * reach "/alt" -- the second mount point could be registered and then be
 * permanently unreachable, which is the state C47 is about.  A prefix only
 * matches on a component boundary, so "/alt" cannot claim "/alphabet". */
static struct mounted_fs *fs_for_path(const char **path)
{
    struct mounted_fs *best = NULL;
    const char *best_rest = NULL;

    for (int i = 0; i < mount_count; i++) {
        struct mounted_fs *m = &mounts[i];
        if (!m->active) continue;
        if (strncmp(*path, m->prefix, m->prefix_len)) continue;
        const char *rest = *path + m->prefix_len;
        /* root "/" is a prefix of everything and means exactly that */
        if (m->prefix_len > 1 && *rest != '/' && *rest != '\0') continue;
        if (best && best->prefix_len >= m->prefix_len) continue;
        best = m;
        best_rest = rest;
    }
    if (!best) return NULL; /* no mount owns this path */
    while (*best_rest == '/') best_rest++;
    *path = best_rest;
    return best;
}

/* "a filesystem answers at /" is a property of the table, not a flag each
 * caller remembers to set (C51): every mount and every umount rederives it
 * from the active entries.  That is what stops a deactivated root from
 * leaving vfs_root_ready() claiming otherwise, and it is why no caller
 * writes disk_ready directly any more. */
static void refresh_root(void)
{
    disk_ready = false;
    for (int i = 0; i < mount_count; i++)
        if (mounts[i].active && mounts[i].prefix_len == 1 && mounts[i].prefix[0] == '/')
            disk_ready = true;
}

/* Register a filesystem mount */
static int register_mount(struct fs_ops *ops, void *priv, const char *prefix)
{
    if (!ops || !priv || !prefix || prefix[0] != '/') return LNXRM_EINVAL;
    /* Two active entries under one prefix would make routing depend on
     * registration order, and umount() could not say which one it removed. */
    int slot = -1;
    for (int i = 0; i < mount_count; i++) {
        if (mounts[i].active && !strcmp(mounts[i].prefix, prefix)) return LNXRM_EFAIL;
        if (slot < 0 && !mounts[i].active) slot = i;
    }
    /* A deactivated slot is free again (C51).  This used to append
     * unconditionally, so every mount/umount pair spent a slot of a fixed
     * array: with several volumes to place and test cases that mount and
     * unmount, the fourth pair would lock mounting out of a table standing
     * three quarters empty.  Order carries no meaning -- routing takes the
     * longest matching prefix, and two mounts may not share a prefix at all
     * (checked above). */
    if (slot < 0) {
        if (mount_count >= MAX_MOUNTS) return LNXRM_EFAIL;
        slot = mount_count++;
    }

    mounts[slot].ops = ops;
    mounts[slot].priv = priv;
    strncpy(mounts[slot].prefix, prefix, sizeof(mounts[slot].prefix) - 1);
    mounts[slot].prefix[sizeof(mounts[slot].prefix) - 1] = 0;
    mounts[slot].prefix_len = strlen(mounts[slot].prefix);
    mounts[slot].active = true;
    refresh_root();

    return 0;
}

/* Publish {ops, priv} under `prefix`.  There is no mount(2): mounting stays
 * a kernel decision (the disk probe and vfs_mount_initramfs() make it), and
 * this is the one entry point they share so that a second mount point
 * cannot be wired up in a way umount() then cannot undo (C47). */
int vfs_mount_fs(struct fs_ops *ops, void *priv, const char *prefix)
{ return register_mount(ops, priv, prefix); }

/* Deactivate every mount at `prefix` (C47).
 *
 * Routing is all it does, and that is deliberate: the mount data is *shared*
 * with any other mount of the same volume (the root mount of a disk still
 * holds it), so releasing it here would free memory out from under a mount
 * that stays.  There is no fs-level release hook to call -- unmounting a
 * volume out of the table is a change of namespace, not of ownership.
 *
 * Sync first: after this returns the path is gone, so everything written
 * through it has to be durable before the last reference to the route is. */
static int umount_prefix(const char *prefix, const char *why)
{
    if (!prefix || prefix[0] != '/') return LNXRM_EINVAL;
    int n = 0;
    for (int i = 0; i < mount_count; i++) {
        if (!mounts[i].active || strcmp(mounts[i].prefix, prefix)) continue;
        mounts[i].active = false;
        n++;
        kprintf("[vfs] unmounted %s (%s)\n", prefix, why);
    }
    if (!n) return LNXRM_ENOENT;
    blk_cache_sync_all();

    /* "a filesystem answers at /" must be recomputed, not assumed: dropping
     * the root mount must not leave vfs_root_ready() claiming there is one,
     * or the boot self-test would assert on a root that no longer resolves. */
    refresh_root();
    return 0;
}

int vfs_umount(const char *prefix)
{ return umount_prefix(prefix, "umount"); }

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
        vn->mnt_data = NULL; /* not a real filesystem: there is no mount */
        return 0;
    }
    if (!strcmp(p, "/dev/console")) {
        vn->type = V_CHR;
        vn->fs_data = NULL;
        vn->mnt_data = NULL;
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
            struct mounted_fs *m = fs_for_path(&p);
            if (m && m->ops->lookup(m->priv, p, vn) == 0) {
                vn->mnt_data = m->priv; /* C39: the mount this node came from */
                return 0;
            }
        }
        return LNXRM_ENOENT;
    }

    struct mounted_fs *m = fs_for_path(&p);
    if (!m) return dev_lookup(path, vn); /* unmounted: virtual nodes only */
    struct fs_ops *ops = m->ops;
    void *mnt = m->priv;
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
    /* C39: every node that leaves here -- resolved, created or truncated --
     * carries the mount it was resolved through, so read/write/size/close
     * below never have to ask "which volume was that?" by matching ops. */
    vn->mnt_data = m->priv;
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
        it->mnt_data = vn->mnt_data; /* C39: same mount the node came from */
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

/* C43: dropping a reference must never take the count below zero.
 *
 * The plain `fetch_and_sub(...) > 1` form answers "you were the last holder"
 * for *every* value <= 1, so a close that arrived after the count was already
 * 0 -- a double close, or a caller that forgot to pair its dup -- ran the
 * count to -1 and freed a struct file someone might still hold.  Nothing in
 * the fd layer ever produces that state, which is exactly why it went
 * unnoticed: the invariant lived in "sys_close clears the slot first", not
 * here.
 *
 * The compare-and-swap loop only commits a drop the caller actually owns.
 * A count of 0 or less means there is nothing left to drop: the file is
 * left exactly as it was and the extra close is reported, so the bug shows
 * up as a log line rather than as a use-after-free.  The count can still be
 * *written* negative by a test driving it directly -- it just can never be
 * driven further down from here. */
void vfs_close_file(struct file *f)
{
    if (!f) return;
    int prev;
    do {
        prev = f->refcnt;
        if (prev <= 0) {
            kprintf("[vfs] close: refcnt=%d, nothing left to drop -- ignoring "
                    "the extra close (%p)\n",
                    prev, f);
            return;
        }
    } while (!__sync_bool_compare_and_swap(&f->refcnt, prev, prev - 1));
    if (prev > 1) return;
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
    /* C27: a read/write file's last fd going away is the natural moment
     * to promise durability -- the door is cheap when nothing is staged.
     * Directories and character devices have no data to lose. */
    bool data = !f->is_dir && f->priv != NULL;
    current->fds[fd] = NULL;
    vfs_close_file(f);
    if (data) blk_cache_sync_all();
    return 0;
}

/* C27: SYS_sync -- hand every cached dirty sector of every device to its
 * device before returning, then report what the last failing one said.
 * Nothing is staged anymore when this answers 0. */
long sys_sync(void)
{
    return blk_cache_sync_all();
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
    struct mounted_fs *m = fs_for_path(&p);
    if (!m || !m->ops->mkdir) return LNXRM_ENOSYS;
    return m->ops->mkdir(m->priv, p);
}

long sys_unlink(const char *path)
{
    char abuf[128];
    const char *norm = abs_path(path, abuf, sizeof(abuf));
    if (!norm) return LNXRM_ENAMETOOLONG;
    path = norm;
    const char *p = path;
    struct mounted_fs *m = fs_for_path(&p);
    if (!m || !m->ops->unlink) return LNXRM_ENOSYS;
    return m->ops->unlink(m->priv, p);
}

long sys_rmdir(const char *path)
{
    char abuf[128];
    const char *norm = abs_path(path, abuf, sizeof(abuf));
    if (!norm) return LNXRM_ENAMETOOLONG;
    path = norm;
    const char *p = path;
    struct mounted_fs *m = fs_for_path(&p);
    if (!m || !m->ops->rmdir) return LNXRM_ENOSYS;
    return m->ops->rmdir(m->priv, p);
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
    struct mounted_fs *m1 = fs_for_path(&p1);
    struct mounted_fs *m2 = fs_for_path(&p2);
    if (!m1 || !m2) return LNXRM_ENOENT;
    /* C39: the ends are compared as *mounts*, not as ops tables.  Two
     * mounts of one filesystem type are still two volumes, and a rename
     * that crossed them would have to move a cluster chain between
     * devices -- EXDEV is what rename(2) says about that. */
    if (m1 != m2) return LNXRM_EXDEV;
    if (!m1->ops->rename) return LNXRM_ENOSYS;
    return m1->ops->rename(m1->priv, p1, p2);
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
    /* Validate the whole (clamped) destination BEFORE reading: a bad
     * pointer must fail without consuming the file position.  The old
     * order read first and copied after, so read(fd, bad, 16) answered
     * EFAULT having already skipped 16 bytes -- data lost on a typo.
     * (2026-10-20) */
    if (n && !user_ptr_writable((u64)ubuf, n)) return LNXRM_EFAULT;
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
    /* Same rule as sys_read, for the directory cookie instead of a file
     * offset: the destination is validated before the iterator moves, so
     * a bad pointer cannot eat a directory entry. (2026-10-20) */
    if (len && !user_ptr_writable((u64)ubuf, len)) return LNXRM_EFAULT;
    long r = f->ops->getdent(f, kbuf, len);
    if (r > 0 && copy_to_user(ubuf, kbuf, r) < 0) return LNXRM_EFAULT;
    return r;
}

/* ---------------- generic file/dir fops bridging to fs_ops -------------- */

/* C41: the size the fs reports right now -- the fs size op when one is
 * provided, else the open-time snapshot (e.g. a dev vnode has none). */
static u64 vn_size_now(struct file *f)
{
    struct vnode *vn = f->priv;
    if (!vn || f->is_dir) return 0;
    return vn->ops && vn->ops->size ? vn->ops->size(vn->mnt_data, vn->fs_data)
                                    : vn->size;
}

/* C41: an fd's access mode decides what it serves.  An O_RDONLY descriptor
 * that could be written used to hand a stale or foreign record a silent
 * rewrite; O_APPEND was simply ignored, letting a write clobber wherever
 * the descriptor happened to be seeked. */
static long reg_read(struct file *f, void *buf, size_t n)
{
    int mode = f->flags & (O_RDONLY | O_WRONLY | O_RDWR);
    if (mode != O_RDONLY && mode != O_RDWR) return LNXRM_EBADF;
    struct vnode *vn = f->priv;
    long r = vn->ops->read(vn->mnt_data, vn->fs_data, f->pos, buf, n);
    if (r > 0) f->pos += r;
    return r;
}

static long reg_write(struct file *f, const void *buf, size_t n)
{
    int mode = f->flags & (O_RDONLY | O_WRONLY | O_RDWR);
    if (mode != O_WRONLY && mode != O_RDWR) return LNXRM_EBADF;
    /* C41: O_APPEND lands at the fs's *current* end -- the snapshot the
     * open took can already be behind, and other handles may have grown
     * the file since. */
    if (f->flags & O_APPEND)
        f->pos = vn_size_now(f);
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
    /* C44: a buffer that cannot hold one record is not an empty answer --
     * a caller wanting data would busy-wait on 0 forever.  Refuse it; a
     * dead scan still outranks the refusal once the answer is possible. */
    if (n < sizeof(struct lnxrm_dirent)) return LNXRM_EINVAL;
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

static int noop_close(struct file *f)
{ (void)f; return 0; }

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
    struct dir_iter *it = f->priv;
    if (it && it->ops && it->ops->close_node) /* C40: release the fs node too */
        it->ops->close_node(it->mnt_data, it->node);
    kfree(f->priv);
    return 0;
}

/* C40: a regular file's close releases everything the open took: the fs
 * node (the fs decides how, via close_node) and the ~48-byte vnode copy
 * here.  The refcount gate in vfs_close_file guarantees this runs exactly
 * once. */
static int reg_close(struct file *f)
{
    struct vnode *vn = f->priv;
    if (vn && vn->ops && vn->ops->close_node)
        vn->ops->close_node(vn->mnt_data, vn->fs_data);
    kfree(f->priv);
    f->priv = NULL;
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
    .close = reg_close,
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

/* Is that root the on-disk FAT32 volume, as opposed to the ram-backed
 * initramfs that also answers at "/"?  The boot self-test asks before
 * filling the volume to provoke ENOSPC: a ramfs has no bottom to hit and
 * filling it would walk straight into the OOM tests' territory. (2026-10-20) */
bool vfs_root_is_fat(void)
{
    for (int i = 0; i < mount_count; i++) {
        if (!mounts[i].active || mounts[i].prefix_len != 1 || mounts[i].prefix[0] != '/')
            continue;
        return mounts[i].ops == &fat32_ops;
    }
    return false;
}

/* The block registry: every device the system knows about, in the order
 * diskinfo reports them.  A driver's device and a parsed partition are
 * registered the same way -- a partition *is* a device, with its own name
 * and its own bounds (C28).
 *
 * The table has a fixed capacity because it is a fixed array, not because
 * a full table is a usable state: once it fills, blk_register() refuses the
 * newcomer and says so.  What it never does is accept the device and then
 * drop it on the floor, which is what "the slot is full but the log said
 * registered" used to amount to.
 *
 * Returns 0 when `b` is in the table, LNXRM_EFAIL when it could not be
 * taken (full or not a device).  Callers that must have their device
 * visible -- the boot disks -- get a loud refusal instead of silently
 * having no disk. */
static struct blkdev *blk_list[BLK_MAX];
static int blk_count;

int blk_register(struct blkdev *b)
{
    if (!b || !b->name) return LNXRM_EFAIL;
    if (blk_count >= BLK_MAX) {
        kprintf("[blk] %s NOT registered: the %d-slot table is full\n", b->name, BLK_MAX);
        return LNXRM_EFAIL;
    }
    blk_list[blk_count++] = b;
    kprintf("[blk] registered %s (%llu sectors, %llu MiB)\n", b->name, b->num_sectors,
            b->num_sectors / 2048);
    return 0;
}

#ifdef CONFIG_KTEST
/* C28 test support: hand the registry over to a case (the table is emptied
 * and the real devices parked here) and take it back.  A case that drives
 * blk_register() to capacity must not leave its dummy devices behind --
 * diskinfo publishes whatever is left in the table.  Never runs while a
 * task exists, so nothing can observe the empty table. */
static struct blkdev *blk_parked[BLK_MAX];
static int blk_parked_n;

int blk_registry_take(void)
{
    blk_parked_n = blk_count;
    for (int i = 0; i < blk_count; i++) blk_parked[i] = blk_list[i];
    blk_count = 0;
    return blk_parked_n;
}

void blk_registry_put(void)
{
    blk_count = blk_parked_n < BLK_MAX ? blk_parked_n : BLK_MAX;
    for (int i = 0; i < blk_count; i++) blk_list[i] = blk_parked[i];
    blk_parked_n = 0;
}

int blk_registry_count(void) { return blk_count; }

struct blkdev *blk_registry_at(int i)
{ return (i >= 0 && i < blk_count) ? blk_list[i] : NULL; }
#endif

int blk_list_all(void *ubuf, int max)
{
    if (max < 0) return LNXRM_EINVAL;
    int n = blk_count < max ? blk_count : max;
    /* Validate the whole record range up front: the per-entry copy below
     * used to answer a bad destination with the count written so far --
     * 0, indistinguishable from "no devices registered". (2026-10-20) */
    if (n && !user_ptr_writable((u64)ubuf, (u64)n * 28)) return LNXRM_EFAULT;
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

/* ---- C51: which volume goes where ------------------------------------
 *
 * Two rules, and the second is the one that makes the first safe:
 *   1. a volume is placed by home_volume(): "/" only to a volume that
 *      carries /bin/init -- the disk this kernel can actually spawn pid 1
 *      from -- and every other volume to the first free /mnt[N];
 *   2. "a filesystem answers at /" is derived from the table
 *      (refresh_root()), never asserted by a caller.
 * Before this, the first mountable volume took "/" whatever was on it: a
 * data disk could not boot anyone (no /bin/init), yet it squatted at the
 * root path, and the initramfs fallback then had to unmount it (C47) to
 * make room for itself. */

/* Is `prefix` already an active mount? */
static bool prefix_taken(const char *prefix)
{
    for (int i = 0; i < mount_count; i++)
        if (mounts[i].active && !strcmp(mounts[i].prefix, prefix)) return true;
    return false;
}

/* How many mounts are live -- the table's real occupancy, since a slot an
 * umount left behind is reusable (C51, register_mount()). */
static int mounts_active(void)
{
    int n = 0;
    for (int i = 0; i < mount_count; i++)
        if (mounts[i].active) n++;
    return n;
}

/* Does this volume carry the program the kernel spawns as pid 1?  Asked by
 * a direct lookup, not by mounting the candidate at "/" and looking there:
 * that would put a data disk in the root slot first and displace it
 * afterwards, and every path lookup in between would resolve through a
 * volume that cannot be booted from. */
static bool volume_has_init(struct fs_ops *ops, void *priv)
{
    if (!ops || !ops->lookup || !priv) return false;
    struct vnode vn;
    memset(&vn, 0, sizeof(vn)); /* a failed lookup hands out no node (fs_data stays NULL) */
    int rc = ops->lookup(priv, "bin/init", &vn);
    if (vn.fs_data && ops->close_node) ops->close_node(priv, vn.fs_data);
    return rc == 0 && vn.type == V_REG;
}

/* Where a volume may be mounted, in preference order.  Four entries is not
 * a coincidence: it is MAX_MOUNTS, so "some prefix here is free" and "the
 * table still has room" are the same statement. */
static const char *const home_prefixes[MAX_MOUNTS] = {"/", "/mnt", "/mnt2", "/mnt3"};

static const char *home_volume(bool has_init)
{
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (prefix_taken(home_prefixes[i])) continue;
        /* "/" is not a data slot: an init-less disk mounted there would be
         * displaced by the initramfs fallback rather than booted, and until
         * that happened every path would resolve through a volume with no
         * pid 1 in it. */
        if (i == 0 && !has_init) continue;
        return home_prefixes[i];
    }
    return NULL;
}

/* Find the first volume on `bd` that can be mounted -- whole disk first
 * (the shipped image is a superfloppy), then each MBR partition -- WITHOUT
 * mounting it.  Partitions are published as devices on the way (C28), so
 * diskinfo sees every one of them whether or not this volume is the one
 * that gets used.  `whole`/`part` say where the answer came from, for the
 * mount line. */
static int find_volume(struct blkdev *bd, struct fs_ops **ops, void **priv,
                       bool *whole, int *part)
{
    kprintf("[vfs] attempting disk mount on %s (%llu sectors, %llu MiB)\n", bd->name,
            bd->num_sectors, bd->num_sectors / 2048);

    /* Flush cache before mounting to ensure consistent state */
    blk_cache_flush(bd);

    /* 1. Try whole disk as FAT32 (most common: no MBR partition table) */
    kprintf("[vfs] trying FAT32 (whole disk)...\n");
    void *m = fat_mount(bd);
    if (m) {
        *ops = &fat32_ops;
        *priv = m;
        *whole = true;
        *part = 0;
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

            /* C28: a partition is published as a device *before* anything is
             * tried on it, so diskinfo sees every partition of every disk
             * whether or not this volume happens to be one we can mount --
             * "the disk has a Linux partition on it" is exactly the case the
             * old code hid.  Extended partitions are the exception: they are
             * containers holding a link record, so they are not devices
             * either (Linux does not export them as /dev nodes either). */
            if (mbr.parts[i].is_extended) continue;

            /* Heap-allocate: fat_mount stores dev pointer in fat_priv */
            struct blkdev *part_dev = kmalloc(sizeof(*part_dev));
            if (!part_dev) continue;

            if (mbr_get_partition(&mbr, i, part_dev, bd) == 0) {
                if (blk_register(part_dev) != 0) {
                    /* Nobody owns it after a refusal: the mount never saw it
                     * and the registry did not take it. */
                    kfree(part_dev->drv);
                    kfree(part_dev);
                    continue;
                }
                /* From here on the registry owns it -- it must outlive a
                 * failed mount attempt, or diskinfo would be publishing a
                 * pointer this loop then freed.  Ownership moves to the
                 * mount only when fat_mount() succeeds, and even then it
                 * stays in the table. */

                void *pm = NULL;

                /* Try FAT32 regardless of the partition's *type byte*.  That
                 * byte is a label written by whatever formatted the disk and
                 * it lies routinely: Windows puts FAT32 in 0x06 as often as
                 * 0x0B, a GPT protective MBR stamps 0xEE over a perfectly
                 * good FAT32 partition, and hand-built/USB images carry
                 * whatever the tool felt like.  Restricting to 0x0B/0x0C
                 * meant a mountable disk was rejected on a formality.
                 * What actually decides "is this FAT32" is the BPB
                 * validation inside fat_mount(), which reads the boot sector
                 * and checks the geometry -- so a Linux or NTFS partition
                 * fails there in one sector read instead of by name. */
                kprintf("[vfs]   trying FAT32 on partition %d...\n", i);
                pm = fat_mount(part_dev);
                if (pm) {
                    *ops = &fat32_ops;
                    *priv = pm;
                    *whole = false;
                    *part = i;
                    return 0;
                }
                continue;
            }
            kfree(part_dev);
        }
    }

    return LNXRM_EFAIL;
}

/* Mount one device's first volume where the C51 rules put it.
 * Returns 0 = it became the root, 1 = a data volume, <0 = nothing to mount;
 * `mounted` (optional) receives the mount object so the ktest case behind
 * vfs_probe_home() can release it -- fs_ops has no release hook (C47). */
static int vfs_try_mount_one(struct blkdev *bd, void **mounted)
{
    if (mounted) *mounted = NULL;

    /* Placement needs a free prefix, and a volume with nowhere to go has to
     * be *refused*, not mounted and dropped: fs_ops has no release hook, so
     * an unplaceable volume would leak its whole mount object.  The check
     * runs before the probe for that reason, and it cannot go stale -- the
     * mount probe is single-threaded. */
    if (mounts_active() >= MAX_MOUNTS) {
        kprintf("[vfs] mount table full, %s not probed\n", bd->name);
        return LNXRM_EFAIL;
    }

    struct fs_ops *ops;
    void *priv;
    bool whole;
    int part;
    if (find_volume(bd, &ops, &priv, &whole, &part) != 0) return LNXRM_EFAIL;

    bool has_init = volume_has_init(ops, priv);
    const char *where = home_volume(has_init);
    if (!where) { /* unreachable: the occupancy check above took this path */
        kprintf("[vfs] no place left for %s\n", bd->name);
        return LNXRM_EFAIL;
    }
    if (register_mount(ops, priv, where) != 0) return LNXRM_EFAIL;
    if (mounted) *mounted = priv;

    /* C51: the FAT32 helpers that take no mount argument work through
     * fat_priv, and with it every boot self-test that drives them -- they
     * must name the volume paths under "/" resolve through, never whichever
     * volume happened to mount first or last.  fat_mount() itself claims the
     * pointer only when it is empty (see fat32.c), so probe order alone
     * cannot decide this on a machine with a data disk enumerated first. */
    if (where[1] == 0) fat_priv = priv;

    if (whole)
        kprintf("[vfs] FAT32 mounted at %s (whole disk, %s)\n", where,
                has_init ? "has /bin/init" : "no /bin/init");
    else
        kprintf("[vfs] FAT32 mounted at %s (partition %d, %s)\n", where, part,
                has_init ? "has /bin/init" : "no /bin/init");
    blk_cache_flush(bd);
    return where[1] == 0 ? 0 : 1;
}

/* Place every registered disk volume (C51), and report whether one of them
 * became the root: 0 = "/" has a disk on it, negative = it does not.  A data
 * volume that mounted is not that answer -- main.c tells the two apart with
 * vfs_data_volume_ready().
 *
 * T-081 / C28: this used to be "mount the first registered device, or give
 * up" -- and the first registered device is whichever driver called
 * blk_register() first.  ide_init() runs before ahci_init() (main.c), so on
 * hardware where the legacy IDE port answers but has nothing usable, a
 * speculative probe that found a phantom disk locked out the real one behind
 * it.  The policy is "every device that actually mounts", and each gets its
 * own attempt line, so a machine that has to fall through to the second disk
 * says so in the log.
 *
 * C51: the loop does NOT stop at the device that filled "/" -- a data disk
 * behind the boot disk still gets placed at /mnt..., and a boot disk behind
 * a data disk still wins "/" because only its volume carries /bin/init.
 *
 * The bound is snapshotted because an attempt *grows* the table: the
 * partitions of disk N are registered while disk N is being tried, and a
 * bound read fresh each round would then walk into those partitions and try
 * to parse a partition table on them. */
int vfs_try_mount_disk(void)
{
    int n = blk_count;
    if (!n) return LNXRM_EFAIL;
    int root = LNXRM_EFAIL;
    for (int i = 0; i < n; i++)
        if (vfs_try_mount_one(blk_list[i], NULL) == 0) root = 0;
    return root;
}

/* C51: does any volume answer *below* "/"?  This is what separates "no disk"
 * from "a disk, just not one that can be booted from" -- two cases that look
 * identical through vfs_root_ready(), which only ever knows about "/". */
bool vfs_data_volume_ready(void)
{
    for (int i = 0; i < mount_count; i++)
        if (mounts[i].active && mounts[i].prefix_len > 1) return true;
    return false;
}

#ifdef CONFIG_KTEST
/* C51 test support (ktest/t_vfs.c): the root mount's {ops, priv}, so a case
 * can take "/" down and put the very same volume back.  umount deactivates a
 * mount, it never releases it (C47), so the object handed out here stays
 * valid for the whole run. */
int vfs_root_mount(struct fs_ops **ops, void **priv)
{
    for (int i = 0; i < mount_count; i++) {
        if (!mounts[i].active || mounts[i].prefix_len != 1 || mounts[i].prefix[0] != '/')
            continue;
        *ops = mounts[i].ops;
        *priv = mounts[i].priv;
        return 0;
    }
    return LNXRM_ENOENT;
}

/* C51: drive the placement rules directly -- find `bd`'s first volume and
 * mount it where home_volume() says.  Returns 0 = root, 1 = data prefix,
 * <0 = nothing mountable; *mounted receives the mount object when one was
 * created so the case can release it after unmounting. */
int vfs_probe_home(struct blkdev *bd, void **mounted)
{
    return vfs_try_mount_one(bd, mounted);
}
#endif

bool vfs_file_exists(const char *path)
{
    struct file *f = NULL;
    if (vfs_open_file(path, O_RDONLY, &f) < 0 || !f) return false;
    vfs_close_file(f);
    return true;
}

/* Free space of the filesystem that owns `path`, in bytes -- the caller that
 * makes fs_ops->freespace real (C39: it existed and had nobody calling it).
 * Routed through fs_for_path() like every other path operation, so the
 * answer comes from the mount the path resolves to.  Returns a negative
 * LNXRM error when no mount answers the path or the filesystem has no such
 * hook; 0 is a legitimate answer for a volume that is full or read-only. */
long vfs_free_space(const char *path)
{
    char abuf[128];
    const char *p = abs_path(path, abuf, sizeof(abuf));
    if (!p) return LNXRM_ENAMETOOLONG;
    struct mounted_fs *m = fs_for_path(&p);
    if (!m || !m->ops->freespace) return LNXRM_ENOSYS;
    return (long)m->ops->freespace(m->priv);
}

/* Mount the initramfs linked into the kernel image at "/".
 *
 * The archive is a *root*, not a subtree: /bin/init is opened by that name
 * and the shell resolves everything else from "/", so there is no useful
 * place to mount it when a disk already owns "/".  When the caller asks for
 * this it is because that disk mounted but cannot boot -- it has no
 * /bin/init -- and nothing has opened a file from it yet: this runs after
 * vfs_try_mount_disk() and before sched_init(), i.e. before any task exists
 * to hold a struct file.  So the disk root is deactivated first, rather than
 * left mounted underneath and reachable through two roots at once. */
int vfs_mount_initramfs(void)
{
    void *m = irfs_mount();
    if (!m) return LNXRM_ENOENT;

    /* Same deactivation umount() performs, so "the disk root is gone" means
     * one thing no matter which caller asked for it (C47): the table is
     * synced, disk_ready is recomputed, and nothing is left mounted twice
     * under "/".  A disk volume without /bin/init no longer reaches "/" at
     * all (C51), so this only ever disposes of a root that appeared some
     * other way -- the call stays because two roots under "/" is exactly the
     * state this function must never leave behind. */
    umount_prefix("/", "it had no /bin/init");

    register_mount(&irfs_ops, m, DISK_PREFIX);
    /* disk_ready means "a filesystem answers at /", which vfs_lookup()'s
     * root case and vfs_root_ready() both key off -- a diskless machine
     * that boots from initramfs has a root, and the boot self-test should
     * exercise it instead of skipping its root assertions.  register_mount()
     * derives it from the table (refresh_root()); nothing here asserts it. */
    return 0;
}
