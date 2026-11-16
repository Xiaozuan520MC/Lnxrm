/* PP3 (GAP section 5.7): pipe(47) -- the ring, both doors, EOF/EPIPE and
 * the wait queue.
 *
 * NOTHING HERE MAY PARK.  The LAPIC timer does not run yet at KTEST_LATE,
 * so a reader blocked on an empty pipe (or a writer on a full one) would
 * never be woken and the boot would hang right here -- the same reason
 * t_syscall.c refuses to probe nanosleep.  Every path walked below is one
 * that cannot block: room for the whole write, bytes to read, EOF, or
 * EPIPE.  The blocking loops in pipe.c are therefore only ever entered by
 * a second live task, never by these checks.
 *
 * The queue case does not block either.  It drives pipe_wq directly with
 * two FAKE sleepers -- the pattern t_input.c uses for evq's list -- asserts
 * the push/unlink/detach shapes with the pipe lock held and the CHECKS
 * outside it (a failing check prints through kprintf, which must not run
 * under a spinlock), then lets the REAL close path wake them and checks
 * they came back T_RUNNABLE with their links cleared.
 *
 * User pages: three single pages, chosen because 0x9000 must stay
 * unmapped (t_syscall's EFAULT probe) and 0x2000/0x4000/0x5000/0x7000/
 * 0xA000/0xB000/0xC000/0xD000/0xE000/0xF000 all belong to other ktest
 * files.  The pages need not be contiguous -- every gate validates the
 * range page by page. */
#include <sys/ktest.h>
#include <sys/pipe.h>
#include <sys/sched.h>
#include <sys/vfs.h>
#include <sys/cpu.h>  /* smap_enter/leave: kernel access to a user page */
#include <console.h>  /* memcpy / memcmp */
#include <mm/mm.h>    /* pmm_alloc, vmm_map_user */

#define PFDS_VA (USER_BASE + 0x3000) /* pipe(2)'s int[2] out-pair */
#define SRC_VA  (USER_BASE + 0x6000) /* one whole page of write source */
#define DST_VA  (USER_BASE + 0x8000) /* one whole page of read sink */
#define UNMAPPED (USER_BASE + 0x9000) /* deliberately never mapped here */

static bool pages_ready;

/* Map the three pages once, for every case in this file.  A failed check
 * is recorded here; the caller then returns instead of running against
 * addresses that would fault mid-case. */
static bool pipe_pages(void)
{
    if (pages_ready) return true;
    u64 pa0 = pmm_alloc(), pa1 = pmm_alloc(), pa2 = pmm_alloc();
    K_EXPECT(pa0 != 0 && pa1 != 0 && pa2 != 0);
    if (!pa0 || !pa1 || !pa2) return false;
    K_EXPECT_EQ(vmm_map_user(current->pml4, PFDS_VA, pa0, true, true, false), 0);
    K_EXPECT_EQ(vmm_map_user(current->pml4, SRC_VA, pa1, true, true, false), 0);
    K_EXPECT_EQ(vmm_map_user(current->pml4, DST_VA, pa2, true, true, false), 0);
    pages_ready = true;
    return true;
}

static void uput(u64 va, const void *src, size_t n)
{
    u64 ac = smap_enter();
    memcpy((void *)va, src, n);
    smap_leave(ac);
}

static void uget(void *dst, u64 va, size_t n)
{
    u64 ac = smap_enter();
    memcpy(dst, (const void *)va, n);
    smap_leave(ac);
}

/* Compare a user range against kernel bytes, through SMAP like uput. */
static int ucmp(u64 va, const void *k, size_t n)
{
    u64 ac = smap_enter();
    int r = memcmp((const void *)va, k, n);
    smap_leave(ac);
    return r;
}

/* Both ends of a fresh pipe, through the real dispatch door.  The pair
 * lands in PFDS_VA; returns false if the door refused. */
static bool fresh_pipe(int f[2])
{
    if (syscall_probe(SYS_pipe, PFDS_VA, 0, 0) != 0) return false;
    uget(f, PFDS_VA, 2 * sizeof(int));
    return true;
}

static u8 pat[4096]; /* write source pattern; ktest runs one case at a time */

/* ---- the ring itself: round trip, wrap-around, and the wrong-door gate ----
 *
 * The second write is the point of the case: only 596 bytes of tail are
 * free (3596 after the partial read, 3500 written), so it runs past the
 * end of the ring and wraps -- the shape a linear buffer cannot represent.
 * The final read then straddles the wrap on the way back out. */
static void test_pipe_ring_wraps(void)
{
    if (!pipe_pages()) return;
    int f[2];

    /* the door's own gate: a non-user address is refused BEFORE any pipe
     * exists, then a good one installs both ends */
    K_EXPECT_EQ(syscall_probe(SYS_pipe, UNMAPPED, 0, 0), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_pipe, 0, 0, 0), LNXRM_EFAULT);
    K_EXPECT(fresh_pipe(f));

    K_EXPECT(f[0] >= 0 && f[0] < NR_FDS);
    K_EXPECT(f[1] >= 0 && f[1] < NR_FDS);
    K_EXPECT(f[0] != f[1]);
    K_EXPECT(current->fds[f[0]] != NULL);
    K_EXPECT(current->fds[f[1]] != NULL);
    struct pipe *p = pipe_of(current->fds[f[0]]);
    K_ASSERT(p != NULL); /* every other assertion below needs the object */
    K_EXPECT_EQ(pipe_of(current->fds[f[1]]), p); /* both ends, one pipe */
    K_EXPECT_EQ(p->nreaders, 1);
    K_EXPECT_EQ(p->nwriters, 1);
    K_EXPECT(p->rwait.head == NULL);
    K_EXPECT(p->wwait.head == NULL);

    /* Each end carries only its own door: read() on the write end and
     * write() on the read end are EBADF at the sys_read/sys_write gate --
     * the fd is perfectly good, the flavour simply has no such op. */
    K_EXPECT_EQ(sys_read(f[1], (void *)DST_VA, 4), LNXRM_EBADF);
    K_EXPECT_EQ(sys_write(f[0], (const void *)SRC_VA, 4), LNXRM_EBADF);

    /* write 3000, read 2500 back: the ring holds 500 and the read offset
     * has moved off the physical start of the buffer */
    for (u32 i = 0; i < 4096; i++) pat[i] = (u8)(0xA0 + (i % 251));
    uput(SRC_VA, pat, 4096);
    K_EXPECT_EQ(sys_write(f[1], (const void *)SRC_VA, 3000), 3000);
    K_EXPECT_EQ(sys_read(f[0], (void *)DST_VA, 2500), 2500);
    K_EXPECT_EQ(ucmp(DST_VA, pat, 2500), 0);

    /* A different pattern, then 3500 bytes into a ring with 3596 free:
     * the write lands whole (atomic -- it fits) and wraps past 4096. */
    for (u32 i = 0; i < 4096; i++) pat[i] = (u8)(0x10 + (i % 251));
    uput(SRC_VA, pat, 3500);
    K_EXPECT_EQ(sys_write(f[1], (const void *)SRC_VA, 3500), 3500);

    /* Read all 4000 back: 1596 bytes from the old tail (the 500 the first
     * read left, then the first 1096 of the second write) followed by the
     * remaining 2404 from the wrapped head. */
    K_EXPECT_EQ(sys_read(f[0], (void *)DST_VA, 4000), 4000);
    K_EXPECT_EQ(ucmp(DST_VA + 500, pat, 3500), 0); /* the second write, whole */
    /* The first 500 bytes are the tail of the FIRST write, left behind by
     * the partial read -- `pat` now holds the second pattern, so regenerate
     * the first one to check them. */
    for (u32 i = 0; i < 4096; i++) pat[i] = (u8)(0xA0 + (i % 251));
    K_EXPECT_EQ(ucmp(DST_VA, pat + 2500, 500), 0);

    /* Close in the order that never asks a parked question: the writer
     * first (p survives, a reader is still counted), then the reader,
     * which finds both counts zero and frees the pipe -- so nothing below
     * may name `p` again. */
    K_EXPECT_EQ(sys_close(f[1]), 0);
    K_EXPECT_EQ(p->nwriters, 0);
    K_EXPECT_EQ(sys_close(f[0]), 0);
    K_EXPECT(current->fds[f[0]] == NULL);
    K_EXPECT(current->fds[f[1]] == NULL);
}

/* ---- EOF and EPIPE: what a close on each side promises ---- */
static void test_pipe_eof_and_epipe(void)
{
    if (!pipe_pages()) return;
    int f[2];
    K_EXPECT(fresh_pipe(f));

    /* bytes, then the LAST writer closes: the reader sees the data first
     * and only then a fresh read answers 0 -- EOF is never early */
    uput(SRC_VA, "abcd", 4);
    K_EXPECT_EQ(sys_write(f[1], (const void *)SRC_VA, 4), 4);
    K_EXPECT_EQ(sys_close(f[1]), 0);
    K_EXPECT_EQ(sys_read(f[0], (void *)DST_VA, 16), 4);
    K_EXPECT_EQ(ucmp(DST_VA, "abcd", 4), 0);
    K_EXPECT_EQ(sys_read(f[0], (void *)DST_VA, 16), 0); /* EOF */
    K_EXPECT_EQ(sys_close(f[0]), 0);

    /* The other side: readers gone first, so the write answers EPIPE.
     * (Linux pairs this with SIGPIPE -- this kernel's signal table has no
     * number for it, see LNXRM_EPIPE, so only the errno comes back.) */
    K_EXPECT(fresh_pipe(f));
    K_EXPECT_EQ(sys_close(f[0]), 0);
    K_EXPECT_EQ(sys_write(f[1], (const void *)SRC_VA, 4), LNXRM_EPIPE);
    K_EXPECT_EQ(sys_close(f[1]), 0);
}

/* ---- dup2 shares one struct file, so the END is the refcount, not the fd ---- */
static void test_pipe_dup_shares_end(void)
{
    if (!pipe_pages()) return;
    int f[2];
    K_EXPECT(fresh_pipe(f));

    int dupfd = -1;
    for (int i = 0; i < NR_FDS; i++)
        if (i != f[0] && i != f[1] && !current->fds[i]) {
            dupfd = i;
            break;
        }
    K_ASSERT(dupfd >= 0);
    K_EXPECT_EQ(sys_dup2(f[1], dupfd), dupfd);
    K_EXPECT(current->fds[dupfd] == current->fds[f[1]]); /* one object */
    K_EXPECT_EQ(current->fds[f[1]]->refcnt, 2);

    /* Closing the ORIGINAL descriptor only drops the count: the pipe still
     * counts one write end, so nothing is closed from its point of view */
    K_EXPECT_EQ(sys_close(f[1]), 0);
    K_EXPECT_EQ(current->fds[dupfd]->refcnt, 1);
    uput(SRC_VA, "xy", 2);
    K_EXPECT_EQ(sys_write(dupfd, (const void *)SRC_VA, 2), 2);

    /* Last reader gone: the surviving writer's next attempt is EPIPE, and
     * the close after it finds both counts zero and frees the pipe. */
    K_EXPECT_EQ(sys_close(f[0]), 0);
    K_EXPECT_EQ(sys_write(dupfd, (const void *)SRC_VA, 1), LNXRM_EPIPE);
    K_EXPECT_EQ(sys_close(dupfd), 0);
}

/* ---- the wait queue: link shapes, then the REAL wake paths ---- */
static void test_pipe_wait_queue(void)
{
    if (!pipe_pages()) return;
    int f[2];
    K_EXPECT(fresh_pipe(f));
    struct pipe *p = pipe_of(current->fds[f[0]]);
    K_ASSERT(p != NULL);
    K_EXPECT(p->rwait.head == NULL);
    K_EXPECT(p->wwait.head == NULL);

    /* Two fake sleepers, never on any runqueue (t_input.c's pattern for
     * evq).  sleep_until is "never" so no tick -- were one running -- could
     * mistake them for timed sleepers and wake them behind our back. */
    struct task *t1 = task_alloc_slot();
    struct task *t2 = task_alloc_slot();
    K_ASSERT(t1 != NULL && t2 != NULL);
    t1->state = T_SLEEPING;
    t1->wait_next = NULL;
    t1->sleep_until = ~0ULL;
    t2->state = T_SLEEPING;
    t2->wait_next = NULL;
    t2->sleep_until = ~0ULL;

    /* push-front, asserted OUTSIDE the lock: the checks print, and a
     * kprintf under p->lock is a lock-order accident waiting to happen. */
    u64 flags;
    struct task *head, *chain;
    bool un1, un2;

    spin_lock_irqsave(&p->lock, &flags);
    pipe_wq_push(&p->rwait, t1);
    pipe_wq_push(&p->rwait, t2);
    head = p->rwait.head;
    chain = t2->wait_next;
    spin_unlock_irqrestore(&p->lock, flags);
    K_EXPECT_EQ(head, t2);  /* last one in is the head */
    K_EXPECT_EQ(chain, t1); /* ... linked to the one before it */

    /* unlink is idempotent: the second call finds nothing and answers
     * false rather than corrupting the chain it already removed */
    spin_lock_irqsave(&p->lock, &flags);
    un1 = pipe_wq_unlink(&p->rwait, t1);
    un2 = pipe_wq_unlink(&p->rwait, t1);
    head = p->rwait.head;
    chain = t2->wait_next;
    spin_unlock_irqrestore(&p->lock, flags);
    K_EXPECT(un1);
    K_EXPECT(!un2);
    K_EXPECT_EQ(head, t2);
    K_EXPECT_EQ(chain, NULL); /* t2 no longer links to a removed node */

    /* detach hands over the WHOLE chain and empties the queue in the same
     * critical section, so a later push cannot find (and skip) a waiter
     * that is already on its way out */
    spin_lock_irqsave(&p->lock, &flags);
    pipe_wq_push(&p->rwait, t1); /* push-FRONT: the chain runs t1 -> t2 */
    chain = pipe_wq_detach(&p->rwait);
    head = p->rwait.head;
    spin_unlock_irqrestore(&p->lock, flags);
    K_EXPECT_EQ(chain, t1); /* the front is whoever was pushed last */
    K_EXPECT_EQ(head, NULL);

    /* Wake OUTSIDE the lock -- sched_wake_task takes runq_lock, which must
     * never nest under a queue lock -- and both come back runnable with
     * their link cleared and a runqueue entry behind them. */
    pipe_wq_wake(chain);
    K_EXPECT_EQ(t1->state, T_RUNNABLE);
    K_EXPECT_EQ(t2->state, T_RUNNABLE);
    K_EXPECT(t1->wait_next == NULL);
    K_EXPECT(t2->wait_next == NULL);
    K_EXPECT(t1->rq_cpu != -1);
    K_EXPECT(t2->rq_cpu != -1);
    task_free_slot(t1); /* pulls them off that runqueue again */
    task_free_slot(t2);

    /* The REAL wake paths now.  Last writer closes -> every parked reader
     * learns EOF; the pipe survives (a reader is still counted). */
    t1 = task_alloc_slot();
    K_ASSERT(t1 != NULL);
    t1->state = T_SLEEPING;
    t1->wait_next = NULL;
    t1->sleep_until = ~0ULL;
    spin_lock_irqsave(&p->lock, &flags);
    pipe_wq_push(&p->rwait, t1);
    spin_unlock_irqrestore(&p->lock, flags);
    K_EXPECT_EQ(sys_close(f[1]), 0);
    K_EXPECT_EQ(t1->state, T_RUNNABLE);
    K_EXPECT(t1->wait_next == NULL); /* unlinked by the wake itself */
    K_EXPECT_EQ(p->nwriters, 0);
    K_EXPECT(p->rwait.head == NULL);
    task_free_slot(t1);

    /* Last reader closes -> every parked writer is let out to find EPIPE;
     * this close also finds both counts zero, so it frees the pipe.  No
     * assertion may name `p` after this line. */
    t2 = task_alloc_slot();
    K_ASSERT(t2 != NULL);
    t2->state = T_SLEEPING;
    t2->wait_next = NULL;
    t2->sleep_until = ~0ULL;
    spin_lock_irqsave(&p->lock, &flags);
    pipe_wq_push(&p->wwait, t2);
    spin_unlock_irqrestore(&p->lock, flags);
    K_EXPECT_EQ(sys_close(f[0]), 0);
    K_EXPECT_EQ(t2->state, T_RUNNABLE);
    K_EXPECT(t2->wait_next == NULL);
    task_free_slot(t2);
}

KTEST("pipe", KTEST_LATE, test_pipe_ring_wraps);
KTEST("pipe", KTEST_LATE, test_pipe_eof_and_epipe);
KTEST("pipe", KTEST_LATE, test_pipe_dup_shares_end);
KTEST("pipe", KTEST_LATE, test_pipe_wait_queue);
