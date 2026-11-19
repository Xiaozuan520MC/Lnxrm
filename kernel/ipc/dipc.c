/* dipc_open(53) / dipc_recv(54) / dipc_send(55): driver inter-process
 * communication channels (S1a).  The contract is spelled out in
 * include/sys/dipc.h; this file is the mechanism.
 *
 * The three pieces, and why each one is where it is:
 *
 *  1. A NAME TABLE that owns one struct file per channel, built exactly
 *     like kernel/fs/shm.c's -- same "every opener shares one file" trick
 *     (so mm/filepage.c's pointer-keyed cache cannot split the ring across
 *     two frames), same synthesised V_REG vnode behind file->priv (so
 *     sys_mmap's gate opens and C42's "priv set, drv NULL" stays true),
 *     same two lifetimes and one door (name until destroy, bytes until the
 *     last reference, both ending in dipc_close_node behind vfs_close_file's
 *     refcount gate).  The one deliberate difference: NOTHING here is
 *     reachable through shm_open, so the raw stream is not a user-named
 *     object anybody can open.
 *
 *  2. A BYTE RING in the channel's own file page, indexed by a header at
 *     the front of that page.  The kernel reads and writes it under
 *     dipc_lock; a ring3 consumer that mmaps the same descriptor reads the
 *     very same bytes through its own mapping of the frame.
 *
 *  3. A DOORBELL: one wait list per channel on task->wait_next, pushed and
 *     popped under dipc_lock and woken strictly outside it.
 *
 * Locking, in full: there is ONE lock, dipc_lock, and it guards the table,
 * the ring indices and every wait list.  It is irqsave because dipc_push
 * runs from IRQ1/IRQ12.  Nothing allocates, reads a disk, kprintfs or
 * parks while it is held -- copy_to_user/copy_from_user are pure memcpy
 * behind a SMAP window and take no lock of their own, which is what lets
 * the byte move straight from frame to user buffer with no bounce buffer
 * on the 32 KiB kernel stack. */
#include <sys/dipc.h>
#include <sys/vfs.h>
#include <sys/sched.h>
#include <mm/mm.h>       /* kmalloc / kfree / pmm_free */
#include <mm/filepage.h> /* filepage_get / filepage_drop_file */
#include <console.h>     /* memset / memcpy / strlen / strcmp / kprintf */

LNXRM_STATIC_ASSERT(LNXRM_DIPC_RING_SIZE == PAGE_SIZE,
                    "a dipc ring must fill exactly one frame");
LNXRM_STATIC_ASSERT(sizeof(struct dipc_hdr) == LNXRM_DIPC_HDR_SIZE,
                    "dipc header and its layout constant disagree");
LNXRM_STATIC_ASSERT(LNXRM_DIPC_PAY_SIZE > 1,
                    "the ring needs room for a payload and a full marker");

/* ---- the table ---- */

static struct dipc *dipc_head;
static u32 dipc_live; /* channels created and not yet freed by close_node */
spinlock_t dipc_lock = SPINLOCK_INIT;

struct dipc *dipc_find(const char *name)
{
    if (!name) return NULL;
    for (struct dipc *d = dipc_head; d; d = d->next)
        if (strcmp(d->name, name) == 0) return d;
    return NULL;
}

/* Caller holds dipc_lock. */
static void dipc_remove(struct dipc *d)
{
    struct dipc **p = &dipc_head;
    while (*p && *p != d) p = &(*p)->next;
    if (*p) *p = d->next;
}

/* Canonical form, shm_norm's rule for the same reason: drop leading
 * slashes, refuse what is left if it is empty or still contains one, and
 * bound it.  "/ps2" and "ps2" name ONE channel; "ps2/x" is EINVAL rather
 * than a name that looks like a path it is not. */
static int dipc_norm(const char *in, char out[LNXRM_DIPC_NAME_MAX])
{
    if (!in || !in[0]) return LNXRM_EINVAL;
    const char *p = in;
    while (*p == '/') p++;
    if (!*p) return LNXRM_EINVAL;
    size_t n = strlen(p);
    if (n >= LNXRM_DIPC_NAME_MAX) return LNXRM_ENAMETOOLONG;
    for (size_t i = 0; i < n; i++)
        if (p[i] == '/') return LNXRM_EINVAL;
    memcpy(out, p, n + 1);
    return 0;
}

/* ---- the ring ---- */

/* One past `i`, wrapping at the payload's end.  The payload is 4088 bytes
 * (a page minus the header), not a power of two, so this is a branch
 * rather than a mask -- and a branch is cheaper than the divide. */
static u32 ring_next(u32 i)
{
    return (i + 1 == LNXRM_DIPC_PAY_SIZE) ? 0 : i + 1;
}

/* `i` advanced by `k` places (k <= PAY_SIZE, so one wrap at most). */
static u32 ring_adv(u32 i, u32 k)
{
    i += k;
    return i >= LNXRM_DIPC_PAY_SIZE ? i - LNXRM_DIPC_PAY_SIZE : i;
}

static u32 ring_count(const struct dipc_hdr *h)
{
    u32 rd = h->rd, wr = h->wr;
    return wr >= rd ? wr - rd : LNXRM_DIPC_PAY_SIZE - rd + wr;
}

/* A ring3 consumer maps this same page, so nothing stops it scribbling on
 * the header.  A plausible index reads as data; an implausible one would
 * send the kernel outside the frame, which is not a thing a user program
 * gets to do.  Out of range means "rewritten by somebody" -- reset. */
static void ring_check(struct dipc_hdr *h)
{
    if (h->rd >= LNXRM_DIPC_PAY_SIZE || h->wr >= LNXRM_DIPC_PAY_SIZE) {
        h->rd = 0;
        h->wr = 0;
    }
}

/* Append up to `len` bytes.  Returns how many landed; the rest went into
 * d->dropped by the caller.  Caller holds dipc_lock. */
static u32 ring_push(struct dipc *d, const u8 *buf, u32 len)
{
    struct dipc_hdr *h = (struct dipc_hdr *)d->ring;
    u8 *pay = d->ring + LNXRM_DIPC_HDR_SIZE;
    u32 rd = h->rd, wr = h->wr, n = 0;

    while (n < len && ring_next(wr) != rd) {
        pay[wr] = buf[n++];
        wr = ring_next(wr);
    }
    h->wr = wr; /* bytes first, index second: the index publishes them */
    return n;
}

/* Move `n` bytes out of the ring into the caller's buffer.  A wrapped read
 * is at most two copies, and `rd` advances only when BOTH land, so an
 * EFAULT leaves the stream exactly as it was (the user keeps the partial
 * bytes it was handed and reads them again next time -- an honest outcome
 * for a call that refused).  Caller holds dipc_lock. */
static long ring_take(struct dipc *d, struct dipc_hdr *h, void *ubuf, u32 n)
{
    u8 *pay = d->ring + LNXRM_DIPC_HDR_SIZE;
    u32 rd = h->rd;
    u32 first = LNXRM_DIPC_PAY_SIZE - rd;
    if (first > n) first = n;

    if (copy_to_user(ubuf, pay + rd, first) < 0) return LNXRM_EFAULT;
    if (first < n && copy_to_user((u8 *)ubuf + first, pay, n - first) < 0)
        return LNXRM_EFAULT;

    h->rd = ring_adv(rd, n);
    return 0;
}

/* ---- the descriptor's two sets of doors ---- */

/* Declared before the tables that name them, in the order the VFS reaches
 * them: read once (filepage_get's seed), write never, size always. */
static int dipc_node_read(void *mnt, void *node, u64 off, void *buf, size_t n);
static int dipc_node_write(void *mnt, void *node, u64 off, const void *buf, size_t n);
static u64 dipc_size(void *mnt, void *node);
static void dipc_close_node(void *mnt, void *node);

/* fs_ops: only the four a synthesised node needs.  All twelve fields are
 * spelled out -- clang's -Wmissing-field-initializers (under -Wextra) flags
 * a partial one, so a NULL door is written as a NULL door. */
static struct fs_ops dipc_fs_ops = {
    .lookup = NULL,
    .getdent = NULL,
    .read = dipc_node_read,
    .write = dipc_node_write,
    .size = dipc_size,
    .close_node = dipc_close_node,
    .create = NULL,
    .mkdir = NULL,
    .unlink = NULL,
    .rmdir = NULL,
    .rename = NULL,
    .freespace = NULL,
};

static struct file_ops dipc_fops = {
    .read = NULL,
    .write = NULL,
    .getdent = NULL,
    .close = reg_close,
};

/* filepage_get's one read, and the only one: it runs on the channel's
 * FIRST frame request, before anything is published, on a frame this file
 * just zeroed.  Afterwards the frame sits in the cache and nobody asks the
 * file again -- which matters, because reading it a second time would hand
 * back zeroes and lose the bytes already in the ring. */
static int dipc_node_read(void *mnt, void *node, u64 off, void *buf, size_t n)
{
    memset(buf, 0, n);
    if (off >= LNXRM_DIPC_RING_SIZE) return 0;
    size_t left = (size_t)(LNXRM_DIPC_RING_SIZE - off);
    if (left > n) left = n;
    return (int)left;
}

/* Store nothing, answer success: the mapped frame IS the ring, so a
 * write-back has nowhere to put bytes and no reason to try.  Answering `n`
 * is what lets vma_writeback clear PG_D instead of retrying forever --
 * the same reason shm's door does it. */
static int dipc_node_write(void *mnt, void *node, u64 off, const void *buf, size_t n)
{
    if (n > 0x7fffffffU) return 0; /* `int` return: nothing this big is mapped */
    return (int)n;
}

static u64 dipc_size(void *mnt, void *node)
{
    return LNXRM_DIPC_RING_SIZE; /* one frame, forever */
}

/* THE door.  Runs exactly once, behind vfs_close_file's refcount gate, when
 * the channel's last reference goes -- so every fd and every mapping of it
 * is already gone.  Drops the channel's OWN reference on the ring frame
 * first: vfs_close_file calls this before filepage_drop_file(), and the
 * cache still holds its own reference at that point, so the frame dies on
 * the SECOND drop -- whichever of the two runs last, never before both. */
static void dipc_close_node(void *mnt, void *node)
{
    struct dipc *d = node;
    if (!d) return;

    u64 iflags;
    bool named;
    spin_lock_irqsave(&dipc_lock, &iflags);
    named = d->linked;
    if (named) {
        dipc_remove(d);
        d->linked = false;
    }
    spin_unlock_irqrestore(&dipc_lock, iflags);
    /* Outside the lock: kprintf must never run under a spinlock.  Reaching
     * here while still named means dipc_destroy never ran, which is a bug
     * worth saying out loud rather than a normal path. */
    if (named) kprintf("[dipc] %s released while still named\n", d->name);

    pmm_free(d->ring_pa); /* the channel's OWN reference (filepage_get's) */
    dipc_live--;
    kfree(d);
}

/* Build the ONE file a channel owns.  refcnt starts at 1: that is the
 * entry's own reference, and dipc_open adds one more per descriptor. */
static struct file *dipc_file_new(struct dipc *d)
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
    vn->size = LNXRM_DIPC_RING_SIZE;
    vn->fs_data = d;       /* what close_node receives, back to front */
    vn->ops = &dipc_fs_ops;
    vn->mnt_data = NULL;   /* no volume: a channel is RAM, not a file on one */
    vn->ino = 0;
    vn->dev = 0;
    vn->ro = false;

    memset(f, 0, sizeof(*f));
    f->ops = &dipc_fops;
    /* ONE access mode shared by every opener -- which is what lets
     * mmap(MAP_SHARED | PROT_WRITE) pass sys_mmap's "open for writing?"
     * check whoever opened the channel.  dipc_open therefore takes no mode
     * argument at all: there is no per-opener mode to record. */
    f->flags = O_RDWR;
    f->refcnt = 1; /* the entry's reference */
    f->priv = vn;  /* C42: priv set, drv left NULL by the memset above */
    return f;
}

/* ---- lifecycle ---- */

int dipc_create(const char *name, long (*sink)(const u8 *, u32))
{
    char key[LNXRM_DIPC_NAME_MAX];
    int rc = dipc_norm(name, key);
    if (rc < 0) return rc;

    /* Allocated with nothing held: kmalloc takes a lock of its own and the
     * failure paths below have to free a frame, neither of which belongs
     * under dipc_lock. */
    struct dipc *d = kmalloc(sizeof(*d));
    if (!d) return LNXRM_ENOMEM;
    memset(d, 0, sizeof(*d));
    memcpy(d->name, key, strlen(key) + 1);
    d->sink = sink;

    struct file *f = dipc_file_new(d);
    if (!f) {
        kfree(d);
        return LNXRM_ENOMEM;
    }

    /* The ring frame, taken BEFORE publication: filepage_get answers one
     * reference per caller, and this one is the channel's -- dropped by
     * close_node.  It also seeds the cache entry, so every later mmap of
     * this file lands on this same frame. */
    u64 pa = filepage_get(f, 0);
    if (!pa) {
        kfree(f->priv);
        kfree(f);
        kfree(d);
        return LNXRM_ENOMEM;
    }
    d->ring_pa = pa;
    d->ring = (u8 *)PHYS_TO_VIRT(pa);
    ((struct dipc_hdr *)d->ring)->rd = 0;
    ((struct dipc_hdr *)d->ring)->wr = 0;

    u64 iflags;
    int err = 0;
    spin_lock_irqsave(&dipc_lock, &iflags);
    if (dipc_find(key)) {
        err = LNXRM_EEXIST; /* somebody published the same name first */
    } else if (dipc_live >= LNXRM_DIPC_MAX) {
        err = LNXRM_ENOSPC;
    } else {
        d->linked = true;
        d->file = f;
        d->next = dipc_head;
        dipc_head = d;
        dipc_live++;
    }
    spin_unlock_irqrestore(&dipc_lock, iflags);
    if (!err) return 0;

    /* Nothing was published, so no close path will ever run for this file
     * -- unwind it by hand instead of through vfs_close_file(), which would
     * call close_node on an entry dipc_live never counted (the shape
     * shm_open's and pipe_new's failure unwinds use for the same reason).
     * Order matters: drop OUR frame reference, then the cache's, then the
     * two allocations -- never pmm_free a frame twice. */
    pmm_free(pa);
    filepage_drop_file(f);
    kfree(f->priv);
    kfree(f);
    kfree(d);
    return err;
}

int dipc_destroy(const char *name)
{
    char key[LNXRM_DIPC_NAME_MAX];
    int rc = dipc_norm(name, key);
    if (rc < 0) return rc;

    u64 iflags;
    struct file *f = NULL;
    spin_lock_irqsave(&dipc_lock, &iflags);
    struct dipc *d = dipc_find(key);
    if (d) {
        dipc_remove(d);   /* out of the table BEFORE the reference goes, so a
                           * racing dipc_create can never find a dying entry */
        d->linked = false;
        f = d->file;      /* read under the lock: the pointer is why we hold it */
    }
    spin_unlock_irqrestore(&dipc_lock, iflags);
    if (!d) return LNXRM_ENOENT;

    /* The entry's OWN reference, dropped outside the lock because the last
     * one runs close_node, which takes it. */
    vfs_close_file(f);
    return 0;
}

/* ---- the producer's door ---- */

/* Shared body of the two producer doors.  With need_reader set, the byte
 * is stored only while a descriptor other than the entry's own holds the
 * channel open, and *owned answers whether that was the
 * case -- the IRQ stubs use it to decide who decodes.  The wake chain is
 * detached under the lock and woken outside it either way: dipc_push runs
 * from IRQ1, so no lock may be held across sched_wake_task. */
static u32 dipc_push_common(const char *name, const u8 *buf, u32 len,
                            bool need_reader, bool *owned)
{
    if (owned) *owned = false;
    if (!name || !buf || !len) return 0;

    u64 iflags;
    struct task *wakees = NULL;
    u32 stored = 0;

    spin_lock_irqsave(&dipc_lock, &iflags);
    struct dipc *d = dipc_find(name);
    if (d) {
        /* refcnt: the entry's reference (1) plus one per open descriptor,
         * bumped atomically by sys_dipc_open -- so >1 is "somebody other
         * than the entry is holding this channel". */
        bool live = __atomic_load_n(&d->file->refcnt, __ATOMIC_RELAXED) > 1;
        if (owned) *owned = live;
        if (!need_reader || live) {
            stored = ring_push(d, buf, len);
            if (stored < len) d->dropped += len - stored;
            /* Detach the WHOLE list here and wake it outside: sched_wake_task
             * may take the runqueue lock, and an IRQ handler holding two locks
             * nobody has ordered is exactly the accident pipe.h warns about. */
            wakees = d->waiters;
            d->waiters = NULL;
        }
    }
    spin_unlock_irqrestore(&dipc_lock, iflags);

    while (wakees) {
        /* Capture the successor BEFORE waking: the task we wake may run on
         * another CPU at once, park again and clear its own wait_next, and
         * reading it after that would lose the rest of the chain. */
        struct task *next = wakees->wait_next;
        wakees->wait_next = NULL;
        sched_wake_task(wakees);
        wakees = next;
    }
    return stored;
}

u32 dipc_push(const char *name, const u8 *buf, u32 len)
{
    return dipc_push_common(name, buf, len, false, NULL);
}

bool dipc_push_if_listening(const char *name, const u8 *buf, u32 len)
{
    bool live;
    dipc_push_common(name, buf, len, true, &live);
    /* "Listening" is the channel's OPEN-NESS, not whether this call stored
     * something: a full ring still means the ring3 side owns the stream
     * (the byte is dropped under dipc_push's flood policy), and splitting
     * single bytes between the two decoders would only reorder them. */
    return live;
}

/* ---- fd -> channel ---- */

struct dipc *dipc_from_fd(int fd)
{
    if (!current || fd < 0 || fd >= NR_FDS) return NULL;
    struct file *f = current->fds[fd];
    if (!f || f->ops != &dipc_fops || !f->priv) return NULL;
    return (struct dipc *)((struct vnode *)f->priv)->fs_data;
}

/* ---- the three calls ---- */

/* dipc_open(name, flags) -> descriptor.  Flags must be 0: a channel has no
 * creation or access mode to choose (dipc_file_new), so there is nothing
 * POSIX's O_CREAT/O_RDONLY pair could mean here -- and a flag this call
 * does not understand is EINVAL rather than something silently ignored. */
long sys_dipc_open(const char *name, u64 flags)
{
    char key[LNXRM_DIPC_NAME_MAX];
    int rc = dipc_norm(name, key);
    if (rc < 0) return rc;
    if (flags != 0) return LNXRM_EINVAL;

    /* A descriptor slot BEFORE anything else: a channel nobody can be
     * handed is worse than a refusal.  Nothing below allocates or blocks,
     * so the slot found here is still free when it is filled. */
    int slot = -1;
    for (int i = 0; i < NR_FDS; i++)
        if (!current->fds[i]) {
            slot = i;
            break;
        }
    if (slot < 0) return LNXRM_EMFILE;

    u64 iflags;
    spin_lock_irqsave(&dipc_lock, &iflags);
    struct dipc *d = dipc_find(key);
    if (!d) {
        spin_unlock_irqrestore(&dipc_lock, iflags);
        return LNXRM_ENOENT;
    }
    /* THE descriptor reference.  Every opener of a channel holds the same
     * struct file, which is the whole reason two mappings share a frame. */
    struct file *f = d->file;
    __sync_fetch_and_add(&f->refcnt, 1);
    spin_unlock_irqrestore(&dipc_lock, iflags);

    current->fds[slot] = f;
    return slot;
}

/* dipc_recv(fd, buf, len, flags): take what is waiting, or park until
 * something does.  Returns the byte count (>= 1), LNXRM_EAGAIN when empty
 * and LNXRM_DIPC_NONBLOCK was set, LNXRM_EINTR when an actionable signal
 * cut the wait short.
 *
 * NOTHING HERE MAY PARK except the schedule() in the loop below -- which is
 * the point of the call.  The two checks (signal, ring) that gate the park
 * are redone under dipc_lock before committing to it, the same recheck
 * input_event_wait and pipe's ends perform: a producer that arrived in the
 * window must not be slept through. */
long sys_dipc_recv(int fd, void *ubuf, u64 len, u64 flags)
{
    if (len == 0 || len > LNXRM_DIPC_RECV_MAX) return LNXRM_EINVAL;
    if (flags & ~(u64)LNXRM_DIPC_NONBLOCK) return LNXRM_EINVAL;
    if (!ubuf) return LNXRM_EFAULT;

    struct dipc *d = dipc_from_fd(fd);
    if (!d) return LNXRM_EBADF;
    const bool nonblock = (flags & LNXRM_DIPC_NONBLOCK) != 0;

    for (;;) {
        u64 iflags;

        /* ---- fast path: something is already there ---- */
        spin_lock_irqsave(&dipc_lock, &iflags);
        struct dipc_hdr *h = (struct dipc_hdr *)d->ring;
        ring_check(h);
        u32 have = ring_count(h);
        if (have) {
            u32 n = have > (u32)len ? (u32)len : have;
            long rc = ring_take(d, h, ubuf, n);
            spin_unlock_irqrestore(&dipc_lock, iflags);
            if (rc < 0) return rc;
            return (long)n;
        }
        if (nonblock) {
            spin_unlock_irqrestore(&dipc_lock, iflags);
            return LNXRM_EAGAIN;
        }
        spin_unlock_irqrestore(&dipc_lock, iflags);

        /* ---- an actionable signal (not pending-but-ignored) cuts it ---- */
        {
            u64 ready = current->signal_pending & ~current->sig_blocked;
            for (int s = 1; ready && s < _NSIG; s++) {
                if ((ready & (1ULL << s)) && !signal_ignored(current, s))
                    return LNXRM_EINTR;
            }
        }

        /* ---- park ---- */
        spin_lock_irqsave(&dipc_lock, &iflags);
        /* Recheck under the lock: a producer may have arrived between the
         * unlocked checks above and this one. */
        h = (struct dipc_hdr *)d->ring;
        ring_check(h);
        have = ring_count(h);
        if (have) {
            u32 n = have > (u32)len ? (u32)len : have;
            long rc = ring_take(d, h, ubuf, n);
            spin_unlock_irqrestore(&dipc_lock, iflags);
            if (rc < 0) return rc;
            return (long)n;
        }
        current->wait_next = d->waiters;
        d->waiters = current;
        current->sleep_until = ~0ULL; /* no deadline: a producer or a signal */
        current->state = T_SLEEPING;
        runqueue_remove(current); /* safe no-op: current is not on a runq */
        spin_unlock_irqrestore(&dipc_lock, iflags);
        schedule();

        /* Woken (producer or signal).  Unlink ourselves so a later producer
         * never evicts a no-longer-sleeping task and starve a real waiter
         * behind it -- idempotent: dipc_push already detached the list, in
         * which case this walk finds nothing and only clears wait_next.
         * The T_RUNNING restore mirrors pipe: schedule() can hand the CPU
         * back without a switch, and a task running as T_RUNNABLE with no
         * runqueue entry is one schedule() away from being lost. */
        spin_lock_irqsave(&dipc_lock, &iflags);
        {
            struct task **p = &d->waiters;
            while (*p && *p != current) p = &(*p)->wait_next;
            if (*p == current) *p = current->wait_next;
            current->wait_next = NULL;
        }
        current->state = T_RUNNING;
        spin_unlock_irqrestore(&dipc_lock, iflags);
    }
}

/* dipc_send(fd, buf, len, flags): the reverse path.  Flags must be 0 (a
 * sink has no ordering or truncation mode), len is bounded because the
 * bytes are staged on the 32 KiB kernel stack, and a channel created
 * without a sink refuses outright rather than accepting and dropping. */
long sys_dipc_send(int fd, const void *ubuf, u64 len, u64 flags)
{
    if (flags != 0) return LNXRM_EINVAL;
    if (len == 0 || len > LNXRM_DIPC_SEND_MAX) return LNXRM_EINVAL;

    struct dipc *d = dipc_from_fd(fd);
    if (!d) return LNXRM_EBADF;
    if (!d->sink) return LNXRM_EINVAL;

    u8 buf[LNXRM_DIPC_SEND_MAX];
    if (copy_from_user(buf, ubuf, (size_t)len) < 0) return LNXRM_EFAULT;

    /* Outside dipc_lock, and the ONLY call in this file that reaches the
     * input queue: the sink takes evq_lock, and a sink is kernel code this
     * channel's creator chose -- it may do whatever it likes, which is not
     * something to promise while holding a spinlock. */
    return d->sink(buf, (u32)len);
}

#ifdef CONFIG_KTEST
u32 dipc_live_objects(void)
{
    u64 iflags;
    spin_lock_irqsave(&dipc_lock, &iflags);
    u32 n = dipc_live;
    spin_unlock_irqrestore(&dipc_lock, iflags);
    return n;
}

u32 dipc_pending(const char *name)
{
    u64 iflags;
    spin_lock_irqsave(&dipc_lock, &iflags);
    struct dipc *d = dipc_find(name);
    u32 n = d ? ring_count((struct dipc_hdr *)d->ring) : 0;
    spin_unlock_irqrestore(&dipc_lock, iflags);
    return n;
}

u64 dipc_dropped(const char *name)
{
    u64 iflags;
    spin_lock_irqsave(&dipc_lock, &iflags);
    struct dipc *d = dipc_find(name);
    u64 n = d ? d->dropped : 0;
    spin_unlock_irqrestore(&dipc_lock, iflags);
    return n;
}
#endif
