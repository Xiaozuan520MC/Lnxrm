/* pipe(47): data structure + wait queue (PP3).  See include/sys/pipe.h for
 * the shape, the wake discipline and why task->wait_next is safe to share
 * with input_event's list.  Everything here is one lock wide: `p->lock`
 * covers the ring, the two end counters and both queues, so "the condition
 * is false" and "I am on the queue" are one critical section -- which is
 * the whole trick, and the reason a wake can never land in the gap. */
#include <sys/pipe.h>
#include <sys/sched.h>
#include <console.h> /* memset/memcpy */
#include <mm/mm.h>   /* kmalloc / kfree */

/* ---- wait queue ----
 * All list ops are called with the owning pipe's lock held; only the wake
 * is called without it.  These four are the entire queue implementation --
 * deliberately small, because the park/unpark protocol around them is what
 * is subtle, not the linking. */

void pipe_wq_push(struct pipe_wq *q, struct task *t)
{
    t->wait_next = q->head;
    q->head = t;
}

bool pipe_wq_unlink(struct pipe_wq *q, struct task *t)
{
    struct task **p = &q->head;
    while (*p && *p != t) p = &(*p)->wait_next;
    if (!*p) return false; /* already detached by the waker: no-op, not an error */
    *p = t->wait_next;
    t->wait_next = NULL;
    return true;
}

/* Hand the whole chain over so the caller can walk it with the lock OFF.
 * The nodes keep their wait_next links -- that is the chain -- and head
 * becomes NULL, so no later producer can find (and miss) a waiter that is
 * on its way to being woken. */
struct task *pipe_wq_detach(struct pipe_wq *q)
{
    struct task *chain = q->head;
    q->head = NULL;
    return chain;
}

/* Wake a detached chain.  The pipe lock must be OFF: sched_wake_task()
 * takes runq_lock, and nesting it under a queue lock is the ordering this
 * kernel refuses everywhere else (input_event's evq_wake, isr.c's
 * input_push).  A node that a signal already woke answers false from
 * sched_wake_task() and is simply skipped -- the chain is still unlinked
 * correctly either way. */
void pipe_wq_wake(struct task *chain)
{
    while (chain) {
        struct task *next = chain->wait_next;
        chain->wait_next = NULL;
        sched_wake_task(chain);
        chain = next;
    }
}

/* An actionable signal (pending and neither blocked nor ignored) cuts a
 * park short with EINTR -- the same test input_event_wait performs, kept
 * here rather than called from it so neither file depends on the other. */
static bool pipe_interrupted(void)
{
    if (!current) return false;
    u64 ready = current->signal_pending & ~current->sig_blocked;
    for (int s = 1; ready && s < _NSIG; s++)
        if ((ready & (1ULL << s)) && !signal_ignored(current, s)) return true;
    return false;
}

/* ---- the two ends ---- */

static long pipe_read(struct file *f, void *buf, size_t n);
static long pipe_write(struct file *f, const void *buf, size_t n);
static int pipe_rd_close(struct file *f);
static int pipe_wr_close(struct file *f);

/* The read end carries no write door and the write end no read door, so
 * read(fd) on a write end and write(fd) on a read end answer EBADF at the
 * sys_read/sys_write gate instead of reaching a pipe op they have no
 * business reaching.  Spelling all four fields out (rather than letting
 * the rest default to NULL) is what makes that contract visible. */
struct file_ops pipe_rd_fops = {
    .read = pipe_read,
    .write = NULL,
    .getdent = NULL,
    .close = pipe_rd_close,
};

struct file_ops pipe_wr_fops = {
    .read = NULL,
    .write = pipe_write,
    .getdent = NULL,
    .close = pipe_wr_close,
};

struct pipe *pipe_of(const struct file *f) { return f ? f->drv : NULL; }

struct pipe *pipe_new(void)
{
    struct pipe *p = kmalloc(sizeof(*p));
    if (!p) return NULL;
    memset(p, 0, sizeof(*p)); /* ring empty, both queues empty, lock free */
    p->lock = (spinlock_t)SPINLOCK_INIT;
    /* One reader and one writer are counted before either end exists:
     * pipe_end_new() is what wraps them, and a failure in between must not
     * leave a pipe that believes it is already closed. */
    p->nreaders = 1;
    p->nwriters = 1;
    return p;
}

struct file *pipe_end_new(struct pipe *p, bool writer)
{
    struct file *f = kmalloc(sizeof(*f));
    if (!f) return NULL;
    memset(f, 0, sizeof(*f));
    f->ops = writer ? &pipe_wr_fops : &pipe_rd_fops;
    f->flags = writer ? O_WRONLY : O_RDONLY;
    f->refcnt = 1;
    f->drv = p; /* borrowed: the pair of ends owns p between them */
    return f;
}

/* ---- read ---- */

static long pipe_read(struct file *f, void *buf, size_t n)
{
    struct pipe *p = pipe_of(f);
    if (!p) return LNXRM_EFAIL;
    if (!n) return 0; /* a zero-length read never waits, per POSIX */

    for (;;) {
        u64 flags;
        spin_lock_irqsave(&p->lock, &flags);

        if (p->used) {
            /* Bytes are there: hand over what there is and stop.  A short
             * read that finds data is legal and is what keeps a reader
             * from waiting for a writer to make up a larger request. */
            u32 take = p->used < n ? p->used : (u32)n;
            u32 head = p->rd;
            u32 first = PIPE_SZ - head < take ? PIPE_SZ - head : take;
            memcpy(buf, p->buf + head, first);
            if (take > first) memcpy((char *)buf + first, p->buf, take - first);
            p->rd = (p->rd + take) % PIPE_SZ;
            p->used -= take;

            /* Space just came free -- wake every parked writer outside the
             * lock (rule 1), not just one: any of them may now have room
             * for its whole write, and the ones that do not re-park. */
            struct task *wakees = pipe_wq_detach(&p->wwait);
            spin_unlock_irqrestore(&p->lock, flags);
            pipe_wq_wake(wakees);
            return (long)take;
        }

        /* Empty.  EOF outranks everything: no writer can ever add a byte
         * again, so waiting would be waiting for nothing. */
        if (p->nwriters == 0) {
            spin_unlock_irqrestore(&p->lock, flags);
            return 0;
        }
        if (pipe_interrupted()) {
            spin_unlock_irqrestore(&p->lock, flags);
            return LNXRM_EINTR;
        }
        if (!current) { /* no task context: nothing could ever wake us */
            spin_unlock_irqrestore(&p->lock, flags);
            return LNXRM_EFAIL;
        }

        /* Park.  The link goes in under the lock that just re-proved the
         * ring empty, so a writer either runs before this critical section
         * (and the condition above already saw its bytes) or after it (and
         * finds us on the queue).  sleep_until is "never": no tick may
         * wake a pipe reader, only a writer or a close can, and a periodic
         * wake here would be a spin in disguise. */
        pipe_wq_push(&p->rwait, current);
        current->sleep_until = ~0ULL;
        current->state = T_SLEEPING;
        runqueue_remove(current); /* off-queue invariant before sleep */
        spin_unlock_irqrestore(&p->lock, flags);
        schedule();

        /* Woken by a write, a close, a signal -- or a raced wake that put
         * us on a runqueue before we left.  Take the lock only to unlink
         * ourselves (the waker may already have detached the chain, which
         * makes this a no-op); the condition is re-read at the top.  The
         * T_RUNNING restore mirrors console_read: schedule() can hand the
         * CPU back without a switch, and a task that runs with T_RUNNABLE
         * and no runqueue entry is one schedule() away from being lost. */
        spin_lock_irqsave(&p->lock, &flags);
        pipe_wq_unlink(&p->rwait, current);
        current->state = T_RUNNING;
        spin_unlock_irqrestore(&p->lock, flags);
    }
}

/* ---- write ---- */

static long pipe_write(struct file *f, const void *buf, size_t n)
{
    struct pipe *p = pipe_of(f);
    if (!p) return LNXRM_EFAIL;
    if (!n) return 0;
    /* sys_write()'s 4096-byte buffer already clamps to PIPE_SZ; repeating
     * it here keeps "there is room for the WHOLE write" always
     * satisfiable, so the wait below can never ask for space the ring can
     * never offer (n > PIPE_SZ would wait forever, since used only ever
     * falls back to 0). */
    if (n > PIPE_SZ) n = PIPE_SZ;

    for (;;) {
        u64 flags;
        spin_lock_irqsave(&p->lock, &flags);

        /* Nobody is listening.  EPIPE, no signal: this kernel's signal
         * table has no SIGPIPE number to raise (see LNXRM_EPIPE). */
        if (p->nreaders == 0) {
            spin_unlock_irqrestore(&p->lock, flags);
            return LNXRM_EPIPE;
        }

        if (PIPE_SZ - p->used >= n) {
            /* The whole write fits: copy it in one critical section, so no
             * other writer can slip bytes in between ours -- the atomicity
             * POSIX promises for writes up to PIPE_BUF, and with sys_write
             * clamping at PIPE_SZ == 4096 every write qualifies. */
            u32 tail = p->wr;
            u32 first = PIPE_SZ - tail < n ? PIPE_SZ - tail : (u32)n;
            memcpy(p->buf + tail, buf, first);
            if (n > first) memcpy(p->buf, (const char *)buf + first, n - first);
            p->wr = (p->wr + n) % PIPE_SZ;
            p->used += n;

            struct task *wakees = pipe_wq_detach(&p->rwait);
            spin_unlock_irqrestore(&p->lock, flags);
            pipe_wq_wake(wakees);
            return (long)n;
        }

        if (pipe_interrupted()) {
            spin_unlock_irqrestore(&p->lock, flags);
            return LNXRM_EINTR;
        }
        if (!current) {
            spin_unlock_irqrestore(&p->lock, flags);
            return LNXRM_EFAIL;
        }

        /* Full and readers exist: park until a read frees enough.  Same
         * discipline as the read side -- link, SLEEPING, off-queue, all
         * under the lock, then sleep with no deadline. */
        pipe_wq_push(&p->wwait, current);
        current->sleep_until = ~0ULL;
        current->state = T_SLEEPING;
        runqueue_remove(current);
        spin_unlock_irqrestore(&p->lock, flags);
        schedule();

        spin_lock_irqsave(&p->lock, &flags);
        pipe_wq_unlink(&p->wwait, current);
        current->state = T_RUNNING;
        spin_unlock_irqrestore(&p->lock, flags);
    }
}

/* ---- close: where EOF and EPIPE are born ---- */

static int pipe_end_close(struct file *f, bool writer)
{
    struct pipe *p = pipe_of(f);
    if (!p) return 0;
    f->drv = NULL; /* this end can never be closed twice (refcnt gate) */

    u64 flags;
    spin_lock_irqsave(&p->lock, &flags);

    bool dead;
    struct task *wakees = NULL;
    if (writer) {
        if (p->nwriters) p->nwriters--;
        dead = (p->nreaders == 0 && p->nwriters == 0);
        /* The last writer is gone: every parked reader must learn EOF now,
         * not on the next write that will never come. */
        if (p->nwriters == 0) wakees = pipe_wq_detach(&p->rwait);
    } else {
        if (p->nreaders) p->nreaders--;
        dead = (p->nreaders == 0 && p->nwriters == 0);
        /* The last reader is gone: every parked writer's next attempt is
         * an EPIPE, so let them all out of the wait to discover it. */
        if (p->nreaders == 0) wakees = pipe_wq_detach(&p->wwait);
    }

    spin_unlock_irqrestore(&p->lock, flags);
    /* Wake BEFORE the free below, and only then: a woken task runs
     * pipe_read/pipe_write again and dereferences `p`, so the object must
     * still be alive when it does.  It always is -- `dead` means both ends
     * are gone, and no task can be blocked inside a pipe whose end file it
     * no longer holds. */
    pipe_wq_wake(wakees);
    if (dead) kfree(p);
    return 0;
}

static int pipe_rd_close(struct file *f) { return pipe_end_close(f, false); }
static int pipe_wr_close(struct file *f) { return pipe_end_close(f, true); }

/* ---- pipe(2) ---- */

long sys_pipe(int *ufds)
{
    /* Validate the destination before anything is allocated: a pipe whose
     * creation had already succeeded is a pipe somebody must unwind. */
    if (!user_ptr_writable((u64)ufds, 2 * sizeof(int))) return LNXRM_EFAULT;

    /* Two free slots -- a pipe is two descriptors, so one is not enough
     * and EMFILE is the honest answer rather than a half-installed pair. */
    int a = -1, b = -1;
    for (int i = 0; i < NR_FDS; i++) {
        if (current->fds[i]) continue;
        if (a < 0)
            a = i;
        else {
            b = i;
            break;
        }
    }
    if (b < 0) return LNXRM_EMFILE;

    struct pipe *p = pipe_new();
    if (!p) return LNXRM_ENOMEM;
    struct file *rd = pipe_end_new(p, false);
    struct file *wr = rd ? pipe_end_new(p, true) : NULL;
    if (!rd || !wr) {
        /* Never installed in the fd table, so no close path will run for
         * them -- free the two wrappers directly and the pipe with them.
         * Its counters are ignored here: it was never observable. */
        kfree(rd);
        kfree(wr);
        kfree(p);
        return LNXRM_ENOMEM;
    }

    current->fds[a] = rd;
    current->fds[b] = wr;
    int out[2] = {a, b};
    if (copy_to_user(ufds, out, sizeof(out)) < 0) {
        /* Defensive: the range was validated above and nothing unmaps it
         * inside one syscall, but a half-installed pair must never survive
         * a failed copy -- unwind both ends exactly as close() would. */
        current->fds[a] = NULL;
        current->fds[b] = NULL;
        vfs_close_file(rd);
        vfs_close_file(wr);
        return LNXRM_EFAULT;
    }
    return 0;
}
