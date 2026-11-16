/* shm_open(51) / shm_unlink(52): named shared-memory objects (S0).
 *
 * The design in one paragraph.  A shm object is a name in its OWN table
 * (never a path, never a directory entry) bound to ONE struct file, and
 * every shm_open of that name hands out a reference to that same file.
 * That single choice is what buys everything else for free: mm/filepage.c
 * keys its shared-frame cache on the struct file POINTER, so two openers
 * necessarily hash to one entry and both mappings land on one physical
 * frame; sys_mmap's "!priv / type != V_REG" gate opens because the object
 * carries a real vnode in file->priv; vfs_pread/vfs_pwrite/vfs_file_size*
 * all resolve through that vnode's fs_ops; and fork's vma_fork_shared()
 * keeps the pair shared because it copies the same file pointer.  Nothing
 * in mm/ is touched by this file at all.
 *
 * The vnode is a synthesised V_REG with no mount behind it (mnt_data NULL,
 * dev 0), so it is a regular file to every VFS door and a non-file to the
 * filesystem, which never sees it.
 *
 * Lifetime.  The entry holds ONE reference on the file that no descriptor
 * ever takes away, and shm_unlink() is the only thing that drops it.  Until
 * then the object survives every close -- POSIX's "lives until unlinked",
 * and the property a rendezvous needs when the two ends never overlap their
 * open windows.  fs_ops->close_node runs exactly once, behind
 * vfs_close_file()'s refcount gate, when the file's last reference goes:
 * unlink has happened AND the last descriptor and mapping are gone.  That
 * is where the entry is freed.  Two lines of lifetime, one door:
 *
 *   ent (the name)  : in the table until shm_unlink() takes it out
 *   file (the bytes): entry reference + descriptors + mappings, until all
 *                     three are gone -- which is what close_node reports
 *
 * Known limitation, deliberate: an object nobody ever unlinks stays forever
 * (name, file, frames).  That is POSIX behaviour, and LNXRM_SHM_MAX_OBJECTS
 * is the pressure valve -- creation past it answers ENOSPC instead of
 * growing without bound. */
#include <sys/shm.h>
#include <sys/vfs.h>
#include <sys/spinlock.h>
#include <sys/sched.h>
#include <console.h> /* memset / memcpy / strlen / strcmp / kprintf */
#include <mm/mm.h>   /* kmalloc / kfree */

/* sys/vfs.h's O_CREAT/O_EXCL are what this file tests `flags` against;
 * abi/lnxrm_abi.h's LNXRM_O_* are what userland passes, because userland
 * cannot include sys/vfs.h.  The two pairs are maintained by hand in two
 * headers, so pin them here rather than trusting that. */
LNXRM_STATIC_ASSERT(LNXRM_O_CREAT == O_CREAT && LNXRM_O_EXCL == O_EXCL,
                    "shm_open flags: abi and vfs.h disagree");

#define SHM_BUCKETS 32 /* power of two: the hash masks instead of dividing */

struct shm_ent {
    /* Canonical key: no leading '/', never empty, no interior '/'. */
    char name[LNXRM_SHM_NAME_MAX];
    u64 size;             /* page-rounded at creation, fixed forever */
    bool linked;          /* still in shm_tab[]?  Cleared before the entry's
                           * reference is dropped, so close_node can tell the
                           * normal case from a bug at a glance */
    struct file *file;    /* THE file: every opener shares this one */
    struct shm_ent *next;
};

static struct shm_ent *shm_tab[SHM_BUCKETS];
static spinlock_t shm_lock = SPINLOCK_INIT;
static u32 shm_live; /* entries created and not yet freed by close_node */

/* ---- table (every one of these runs with shm_lock held) ---- */

static u32 shm_hash(const char *key)
{
    u32 h = 5381;
    for (const char *p = key; *p; p++) h = h * 33 + (u8)*p;
    return h & (SHM_BUCKETS - 1);
}

static struct shm_ent *shm_find(const char *key)
{
    for (struct shm_ent *e = shm_tab[shm_hash(key)]; e; e = e->next)
        if (strcmp(e->name, key) == 0) return e;
    return NULL;
}

static void shm_remove(struct shm_ent *e)
{
    struct shm_ent **p = &shm_tab[shm_hash(e->name)];
    while (*p && *p != e) p = &(*p)->next;
    if (*p) *p = e->next;
}

/* ---- the vnode's four doors ---- */

static u64 shm_size(void *mnt, void *node)
{
    /* Fixed at creation, so this is also what sys_stat and MAP_SHARED
     * write-back see: the object never grows and never shrinks. */
    return ((struct shm_ent *)node)->size;
}

/* The reader exists for mm/filepage.c, which asks for one page at a
 * page-aligned offset on the FIRST touch of that offset and never again (a
 * cached frame is served straight from the cache after that).  The frame is
 * zeroed before this is called, so zero-filling the request is what makes an
 * untouched object read as all zeroes -- and the returned count is never
 * above LNXRM_SHM_MAX_SIZE (1 GiB), which keeps it inside `int`. */
static int shm_node_read(void *mnt, void *node, u64 off, void *buf, size_t n)
{
    struct shm_ent *e = node;
    memset(buf, 0, n);
    if (off >= e->size) return 0;
    size_t left = (size_t)(e->size - off);
    if (left > n) left = n;
    return (int)left;
}

/* Store nothing, answer success: the mapped frames ARE the object's bytes,
 * so a write-back has nowhere to put them and no reason to try.  This is the
 * door vma_writeback() walks, and answering `n` is what lets it clear PG_D
 * and count the page instead of retrying it forever.  Deliberately NOT an
 * error: the mapping is writable by design, and refusing the flush would
 * leave every shared page dirty until exit. */
static int shm_node_write(void *mnt, void *node, u64 off, const void *buf, size_t n)
{
    if (n > 0x7fffffffU) return 0; /* `int` return: nothing this big is mapped */
    return (int)n;
}

/* The entry's release door.  Runs EXACTLY once -- vfs_close_file()'s
 * refcount gate sits in front of reg_close(), which is what calls it -- and
 * only when the file's last reference has gone.  The entry's own reference
 * is dropped solely by shm_unlink(), so by the time we are here the name has
 * always already left the table; taking it out again costs one chain walk
 * and keeps the "cannot happen" honest rather than assumed. */
static void shm_close_node(void *mnt, void *node)
{
    struct shm_ent *e = node;
    if (!e) return;

    u64 iflags;
    bool named;
    spin_lock_irqsave(&shm_lock, &iflags);
    named = e->linked;
    if (named) {
        shm_remove(e);
        e->linked = false;
    }
    spin_unlock_irqrestore(&shm_lock, iflags);
    /* Outside the lock: kprintf must never run under a spinlock. */
    if (named) kprintf("[shm] %s released while still named\n", e->name);

    shm_live--;
    kfree(e);
}

/* All twelve fields spelled out: clang's -Wmissing-field-initializers (under
 * -Wextra) flags a partial one, so a NULL door is written as a NULL door. */
static struct fs_ops shm_fs_ops = {
    .lookup = NULL,
    .getdent = NULL,
    .read = shm_node_read,
    .write = shm_node_write,
    .size = shm_size,
    .close_node = shm_close_node,
    .create = NULL,
    .mkdir = NULL,
    .unlink = NULL,
    .rmdir = NULL,
    .rename = NULL,
    .freespace = NULL,
};

/* The descriptor's doors.  No read and no write on purpose: sys_read and
 * sys_write answer EBADF for a NULL door (the same answer a wrong pipe end
 * gets), which is the honest one here -- the pair would move the ONE cursor
 * every opener of this object shares, and a write has nowhere to go even if
 * they did.  The byte range stays reachable through mmap, vfs_pread and
 * vfs_pwrite, which take an explicit offset and never touch f->pos.
 *
 * close is C40's, shared with every other regular flavour so the "hand the
 * node back, then free the vnode copy" rule has one implementation. */
static struct file_ops shm_fops = {
    .read = NULL,
    .write = NULL,
    .getdent = NULL,
    .close = reg_close,
};

/* Build the ONE file an entry owns.  refcnt starts at 1: that is the
 * entry's own reference, and shm_open adds one more per descriptor. */
static struct file *shm_file_new(struct shm_ent *e)
{
    struct file *f = kmalloc(sizeof(*f));
    struct vnode *vn = kmalloc(sizeof(*vn));
    if (!f || !vn) {
        kfree(f);
        kfree(vn);
        return NULL;
    }

    memset(vn, 0, sizeof(*vn));
    vn->type = V_REG;      /* sys_mmap's second gate (kernel/core/syscall.c) */
    vn->size = e->size;    /* vfs_file_size()'s answer, the open-time snapshot */
    vn->fs_data = e;       /* what close_node receives, back to front */
    vn->ops = &shm_fs_ops; /* ...and what vfs_file_size_now/pread/pwrite use */
    vn->mnt_data = NULL;   /* no volume: an object is RAM, not a file on one,
                            * so stat's st_dev is 0 ("no volume behind this") */
    vn->ino = 0;
    vn->dev = 0;
    vn->ro = false;

    memset(f, 0, sizeof(*f));
    f->ops = &shm_fops;
    /* ONE access mode for the object, shared by every opener -- which is
     * what lets mmap(MAP_SHARED | PROT_WRITE) clear sys_mmap's "is this
     * descriptor open for writing?" check whoever created the object.  It
     * is also why shm_open's O_RDONLY/O_WRONLY/O_RDWR are ignored: there is
     * no per-opener mode to record. */
    f->flags = O_RDWR;
    f->refcnt = 1; /* the entry's reference */
    f->priv = vn;  /* C42: priv set, drv left NULL by the memset above */
    return f;
}

/* Canonical form: drop leading slashes (POSIX spells the name "/x"), refuse
 * what is left if it is empty or still contains one, and bound it.  "/x" and
 * "x" therefore name ONE object; "x/y" is EINVAL rather than an object whose
 * name looks like a path it is not. */
static long shm_norm(const char *in, char out[LNXRM_SHM_NAME_MAX])
{
    if (!in || !in[0]) return LNXRM_EINVAL;
    const char *p = in;
    while (*p == '/') p++;
    if (!*p) return LNXRM_EINVAL; /* "///": no name under the slashes */
    size_t n = strlen(p);
    if (n >= LNXRM_SHM_NAME_MAX) return LNXRM_ENAMETOOLONG;
    for (size_t i = 0; i < n; i++)
        if (p[i] == '/') return LNXRM_EINVAL;
    memcpy(out, p, n + 1);
    return 0;
}

/* shm_open(name, flags, size) -> descriptor. */
long sys_shm_open(const char *name, int flags, u64 size)
{
    char key[LNXRM_SHM_NAME_MAX];
    long rc = shm_norm(name, key);
    if (rc < 0) return rc;

    /* The access mode is not a choice (shm_file_new), so the POSIX create
     * pair is all that can be asked for: O_RDONLY/O_WRONLY/O_RDWR are
     * accepted for source compatibility and ignored, and every other bit is
     * refused rather than silently dropped. */
    if (flags & ~(O_CREAT | O_EXCL | O_WRONLY | O_RDWR)) return LNXRM_EINVAL;

    /* A descriptor slot BEFORE anything is created: an object nobody can be
     * handed is worse than a refusal.  Nothing below blocks or sleeps, so
     * the slot found here is still free when it is filled. */
    int slot = -1;
    for (int i = 0; i < NR_FDS; i++)
        if (!current->fds[i]) {
            slot = i;
            break;
        }
    if (slot < 0) return LNXRM_EMFILE;

    u64 iflags;

    /* ---- hit ---- */
    spin_lock_irqsave(&shm_lock, &iflags);
    struct shm_ent *e = shm_find(key);
    if (e) {
        if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) {
            spin_unlock_irqrestore(&shm_lock, iflags);
            return LNXRM_EEXIST;
        }
        /* THE descriptor reference.  Every opener of one object holds the
         * same struct file, which is the whole reason two mappings share a
         * frame: mm/filepage.c keys on this pointer. */
        struct file *f = e->file;
        __sync_fetch_and_add(&f->refcnt, 1);
        spin_unlock_irqrestore(&shm_lock, iflags);
        current->fds[slot] = f;
        return slot;
    }
    spin_unlock_irqrestore(&shm_lock, iflags);

    /* ---- miss ---- */
    if (!(flags & O_CREAT)) return LNXRM_ENOENT;
    if (!size) return LNXRM_EINVAL;
    if (size > LNXRM_SHM_MAX_SIZE) return LNXRM_ENOMEM;
    /* Rounded so every mapped page lies wholly inside the object, which is
     * what keeps shm_node_read from ever having to answer a short count for
     * a page the demand pager asked for whole.  The cap above is a multiple
     * of PAGE_SIZE, so the round-up cannot push past it. */
    size = ALIGN_UP(size, PAGE_SIZE);

    /* Allocated OUTSIDE the lock on purpose: kmalloc takes a lock of its own
     * and this runs with interrupts off, so nesting it under shm_lock would
     * be an ordering nobody has proven.  The price is re-checking for a
     * creator that published the same name while we were allocating. */
    struct shm_ent *ne = kmalloc(sizeof(*ne));
    if (!ne) return LNXRM_ENOMEM;
    memset(ne, 0, sizeof(*ne));
    memcpy(ne->name, key, strlen(key) + 1);
    ne->size = size;

    struct file *nf = shm_file_new(ne);
    if (!nf) {
        kfree(ne);
        return LNXRM_ENOMEM;
    }

    spin_lock_irqsave(&shm_lock, &iflags);
    e = shm_find(key);
    if (e) {
        /* Somebody won the race.  Theirs is the object; ours never entered
         * the table, so no close path will ever run for it -- free the
         * wrapper directly instead of through vfs_close_file(), which would
         * run close_node on an entry shm_live never counted (the shape
         * pipe_new's failure unwind uses for the same reason). */
        struct file *f = e->file;
        __sync_fetch_and_add(&f->refcnt, 1);
        spin_unlock_irqrestore(&shm_lock, iflags);
        kfree(nf->priv);
        kfree(nf);
        kfree(ne);
        current->fds[slot] = f;
        return slot;
    }
    if (shm_live >= LNXRM_SHM_MAX_OBJECTS) {
        spin_unlock_irqrestore(&shm_lock, iflags);
        kfree(nf->priv);
        kfree(nf);
        kfree(ne);
        return LNXRM_ENOSPC;
    }

    /* Publish.  The file's refcnt of 1 is the ENTRY's reference -- the one
     * only shm_unlink() drops -- and the +1 below is this descriptor's. */
    ne->linked = true;
    ne->file = nf;
    __sync_fetch_and_add(&nf->refcnt, 1);
    {
        u32 b = shm_hash(key);
        ne->next = shm_tab[b];
        shm_tab[b] = ne;
    }
    shm_live++;
    spin_unlock_irqrestore(&shm_lock, iflags);

    current->fds[slot] = nf;
    return slot;
}

/* shm_unlink(name): the name goes now, the object goes when its last
 * reference does. */
long sys_shm_unlink(const char *name)
{
    char key[LNXRM_SHM_NAME_MAX];
    long rc = shm_norm(name, key);
    if (rc < 0) return rc;

    u64 iflags;
    spin_lock_irqsave(&shm_lock, &iflags);
    struct shm_ent *e = shm_find(key);
    if (e) {
        shm_remove(e);   /* out of the table BEFORE the reference goes, so a
                          * racing shm_open can never reach a dying entry */
        e->linked = false;
    }
    spin_unlock_irqrestore(&shm_lock, iflags);
    if (!e) return LNXRM_ENOENT;

    /* The entry's OWN reference -- outside the lock, because the last one
     * runs shm_close_node(), which takes it.  `e` is unreachable from the
     * table now and its `file` never changes after creation, so reading it
     * here needs no lock of its own. */
    vfs_close_file(e->file);
    return 0;
}

#ifdef CONFIG_KTEST
u32 shm_live_objects(void)
{
    u64 iflags;
    spin_lock_irqsave(&shm_lock, &iflags);
    u32 n = shm_live;
    spin_unlock_irqrestore(&shm_lock, iflags);
    return n;
}
#endif
