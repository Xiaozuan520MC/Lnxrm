// SPDX-License-Identifier: Apache-2.0

/* pipe(47): a unidirectional byte stream between two descriptors (PP3).
 *
 * SHAPE
 *   One `struct pipe` owns the bytes and both ends point at it through
 *   `file->drv`: pipe(2) allocates it, hands back a READ end (pipe_rd_fops,
 *   O_RDONLY) and a WRITE end (pipe_wr_fops, O_WRONLY).  The two ends are
 *   separate struct files on purpose -- dup2/fork share one file and only
 *   bump its refcnt, so the pipe's own `nreaders`/`nwriters` count FILE
 *   DESCRIPTIONS, and the last one to go is what produces EOF / EPIPE.
 *
 * THE WAIT QUEUE (this is the half that makes a pipe a pipe)
 *   A reader finding the ring empty and a writer finding it full park on
 *   one of two queues, `rwait` and `wwait`.  Each is a push-front singly
 *   linked list threaded through task->wait_next -- the same field
 *   input_event's evq list uses, which is safe because a task parks in
 *   exactly ONE place at a time (it is blocked inside one syscall), so the
 *   two lists can never claim the same node.  Both queues are guarded by
 *   the pipe's own lock, never by a lock of their own: the waiter must
 *   link itself in the very same critical section that proved the
 *   condition false, or the wake that lands in between is lost.
 *
 * WAKE DISCIPLINE (two rules, both load-bearing)
 *   1. Detach under the pipe lock, wake OUTSIDE it.  sched_wake_task()
 *      takes runq_lock, and this codebase never nests that lock inside a
 *      queue lock (input_event's evq_wake and isr.c's input_push both
 *      detach-then-wake for the same reason).
 *   2. Wake EVERY waiter of the affected direction.  Waking one reader
 *      and letting the others sleep while data sits in the ring is a hang
 *      waiting for a write that may never come -- the same reasoning
 *      T-054 wrote down for the console ring.
 *
 * BLOCKING CAN NEVER HANG THE BOOT: a ktest that would block would hang
 * before the timer runs (the LAPIC timer does not start until after the
 * late stage), so every path the self-test walks is one that does not
 * park -- room for the whole write, bytes to read, EOF, or EPIPE. */
#pragma once
#include <types.h>
#include <sys/spinlock.h>
#include <sys/vfs.h> /* struct file / file_ops, O_RDONLY/O_WRONLY */

struct task;

/* Ring capacity.  sys_write() formats through a 4096-byte stack buffer and
 * clamps to it, so a single write never asks for more than PIPE_SZ: the
 * "room for the WHOLE write" test below is always satisfiable, which is
 * what makes the wait atomic (POSIX writes of <= PIPE_BUF land together
 * or not at all) and keeps the park loop from waiting for space that can
 * never appear. */
#define PIPE_SZ 4096

/* One park queue.  `head` is the most recently parked task; the chain runs
 * through task->wait_next.  Guarded by the owning pipe's lock. */
struct pipe_wq {
    struct task *head;
};

struct pipe {
    spinlock_t lock;      /* ring, counters and BOTH wait queues */
    char buf[PIPE_SZ];
    u32 rd;               /* next byte to read  */
    u32 wr;               /* next byte to write */
    u32 used;             /* bytes currently held: wr-rd, never > PIPE_SZ */
    u32 nreaders;         /* open READ-end file descriptions, not fds */
    u32 nwriters;         /* open WRITE-end file descriptions, not fds */
    struct pipe_wq rwait; /* readers parked: ring empty and writers left */
    struct pipe_wq wwait; /* writers parked: ring full and readers left */
};

/* Allocation.  pipe_new() starts with one reader and one writer counted --
 * the two ends pipe_end_new() is about to wrap around it. */
struct pipe *pipe_new(void);
/* Wrap `p` in a descriptor file; NULL is LNXRM_ENOMEM for the caller.  The
 * returned file BORROWS `p`: the pair of ends owns the pipe between them,
 * and pipe_end_close() frees it once both have gone. */
struct file *pipe_end_new(struct pipe *p, bool writer);

/* The pipe an end belongs to (f->drv), or NULL for any other flavour.
 * Public so a self-test can reach the same object the read/write paths do
 * without a test-only accessor of its own. */
struct pipe *pipe_of(const struct file *f);

/* pipe(2) itself: two free descriptor slots, then copy the pair out. */
long sys_pipe(int *ufds);

/* ---- wait-queue operations ----
 * The first three take the owning pipe's lock HELD by the caller: they are
 * the list, and the caller owns the critical section that proves the
 * condition false.  pipe_wq_wake() takes NO lock -- it must be called with
 * the pipe lock already dropped (rule 1 above), and it clears each node's
 * wait_next on the way past so a woken task cannot leave a stale link. */
void pipe_wq_push(struct pipe_wq *q, struct task *t);
bool pipe_wq_unlink(struct pipe_wq *q, struct task *t);
struct task *pipe_wq_detach(struct pipe_wq *q); /* hand over the whole chain */
void pipe_wq_wake(struct task *chain);
