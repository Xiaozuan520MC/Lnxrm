/* T-004: running out of memory is an error return, never a panic.
 *
 * The branches under test are the ones a user program could reach by
 * looping brk()/fork() until the buddy was dry -- before this change each
 * of them stopped the machine instead of answering the call.  The buddy is
 * not actually emptied to prove it: pmm_inject_oom() makes the next frame
 * allocation behave as if the free lists were empty, so an OOM path can be
 * driven on a box that has plenty of memory.  (Exhausting RAM for real is
 * covered end to end by /bin/systest's brk.oom block, which gives every
 * frame straight back afterwards.)
 *
 * The second half of the file is T-OOM, the mirror image of that contract:
 * a failure that CAN be answered still answers (T-004, untouched below), and
 * a user fault that cannot -- no frame, no way out -- is where the
 * out-of-memory killer (mm/oom.c) spends somebody's life instead.
 *
 * Every case must leave both allocators exactly as it found them. */
#include <sys/ktest.h>
#include <sys/sched.h>
#include <sys/vfs.h>      /* syscall_probe: mmap through the real dispatch */
#include <sys/cpu.h>      /* struct intr_frame: a caller-built #PF */
#include <console.h>      /* memset: the synthetic #PF frame */
#include <mm/mm.h>
#include <mm/pagefault.h> /* page_fault_try_fix: the entry isr_common takes */

/* kmalloc()'s buddy path: anything past a page skips the free lists, so one
 * injected failure lands in exactly the branch that used to panic. */
static void test_kmalloc_buddy_oom_returns_null(void)
{
    u64 used0 = kheap_used_bytes();
    u64 free0 = pmm_free_bytes();

    pmm_inject_oom(1);
    void *p = kmalloc(1 << 20);
    pmm_inject_oom(0);

    K_EXPECT(p == NULL);                    /* refused, not fatal */
    K_EXPECT_EQ(kheap_used_bytes(), used0); /* and not charged for it */
    K_EXPECT_EQ(pmm_free_bytes(), free0);

    p = kmalloc(1 << 20); /* the heap still hands memory out afterwards */
    K_ASSERT(p != NULL);
    kfree(p);
    K_EXPECT_EQ(kheap_used_bytes(), used0);
    K_EXPECT_EQ(pmm_free_bytes(), free0);
}

/* kmalloc()'s refill path: the small free list serves what is on it, then
 * carves a fresh page out of the buddy.  Draining a bin that nothing else
 * uses (bin 8: one 4096-byte chunk per page) until the refill is due makes
 * that branch deterministic, and the injected failure has to come back as
 * NULL instead of a panic. */
static void test_kmalloc_refill_oom_returns_null(void)
{
    enum { CAP = 64, CHUNK = 2048 }; /* CHUNK + header -> bin 8 */
    static void *stash[CAP];
    u64 used0 = kheap_used_bytes();
    u64 free0 = pmm_free_bytes();
    int n = 0;

    pmm_inject_oom(1);
    for (; n < CAP; n++) {
        void *q = kmalloc(CHUNK);
        if (!q) break;
        stash[n] = q;
    }
    pmm_inject_oom(0);

    K_EXPECT(n < CAP); /* the bin dried up and the refill said no */
    for (int i = 0; i < n; i++) kfree(stash[i]);
    K_EXPECT_EQ(kheap_used_bytes(), used0);
    K_EXPECT_EQ(pmm_free_bytes(), free0);
}

/* A request larger than the buddy can ever satisfy needs no injection at
 * all: MAX_ORDER caps it out and kmalloc must answer NULL. */
static void test_kmalloc_oversize_returns_null(void)
{
    u64 used0 = kheap_used_bytes();
    u64 free0 = pmm_free_bytes();

    K_EXPECT(kmalloc(1UL << 40) == NULL);
    K_EXPECT_EQ(kheap_used_bytes(), used0);
    K_EXPECT_EQ(pmm_free_bytes(), free0);
}

/* vmm had two panics of its own on the same paths: the frame for a new
 * aspace root (fork/exec) and the page tables vmm_map_user builds on the
 * way to a page (brk grows one PT every 2 MiB).  Both must report failure
 * and leave nothing half-built behind. */
static void test_vmm_oom_is_an_error_not_a_panic(void)
{
    u64 free0 = pmm_free_bytes();
    u64 root = vmm_new_user_aspace();
    K_ASSERT(root != 0);

    /* (a) the aspace root */
    pmm_inject_oom(1);
    u64 none = vmm_new_user_aspace();
    pmm_inject_oom(0);
    K_EXPECT_EQ(none, 0);

    /* (b) the first page table on the way to USER_BASE */
    u64 pa = pmm_alloc();
    K_ASSERT(pa != 0);
    pmm_inject_oom(1);
    int r = vmm_map_user(root, USER_BASE, pa, true, true, false);
    pmm_inject_oom(0);
    K_EXPECT_EQ((u64)r, (u64)LNXRM_ENOMEM);
    K_EXPECT_EQ(vmm_translate_in(root, USER_BASE), 0); /* nothing half-mapped */

    /* the same call succeeds once frames are available again */
    K_EXPECT_EQ(vmm_map_user(root, USER_BASE, pa, true, true, false), 0);
    K_EXPECT_EQ(vmm_translate_in(root, USER_BASE), pa);
    K_EXPECT_EQ(vmm_unmap_user(root, USER_BASE), pa);
    pmm_free(pa);

    vmm_destroy_user_aspace(root);
    K_EXPECT_EQ(pmm_free_bytes(), free0);
}

/* ================= T-OOM: the out-of-memory killer (mm/oom.c) =================
 *
 * The policy is a pure function of the task table, so the heuristic is
 * pinned without emptying a real machine: fake slots stand in for the
 * processes it would have to choose between, and the trigger itself runs
 * through the same #PF entry isr_common() takes, with one frame allocation
 * made to fail the way an empty buddy fails it.
 *
 * All four run at KTEST_LATE -- after sched_init() has built the table and
 * before /bin/init exists -- the window t_task.c uses for the same reason:
 * claims, wakeups and recycling can be exercised while the only real
 * occupant of the table is the idle task.  Interrupts are off for the whole
 * stage, so a fake briefly queued by a wakeup cannot be scheduled before
 * its own case takes it back off.  Every case frees its slots and leaves the
 * latch clear (one frame out and in -- what a recovering shortage looks
 * like), so no case can depend on the order the cases run in. */

/* A task the killer is allowed to look at: claimed slot, a pid nothing real
 * uses, a state the scheduler never queued it in (send_signal only touches
 * the signal bit of a T_RUNNABLE task), and `pages` user pages to its name.
 * pid 0 is never given out by task_alloc_slot(), which is why fakes are
 * numbered from 6100 up. */
static struct task *fake_task(u32 pid, u64 pages, enum task_state st)
{
    struct task *t = task_alloc_slot();
    if (!t) return NULL;
    t->pid = pid;
    t->state = st;
    t->mem_pages = pages;
    t->mem_peak = pages;
    t->signal_pending = 0;
    return t;
}

/* Put the kill-once latch back to "no shortage in progress": a frame out
 * and a frame in, exactly the event pmm_alloc_order() reports to oom.c. */
static void oom_latch_reset(void)
{
    u64 pa = pmm_alloc();
    if (pa) pmm_free(pa);
}

/* The heuristic: most user pages wins, and everything that must not die is
 * passed over however much it holds. */
static void test_oom_selects_the_biggest_live_task(void)
{
    struct task *small = fake_task(6101, 16, T_RUNNABLE);
    struct task *big = fake_task(6102, 4096, T_RUNNABLE);
    struct task *mid = fake_task(6103, 512, T_RUNNABLE);
    K_ASSERT(small && big && mid);

    K_EXPECT_EQ(oom_select_victim(), big);       /* the hog, not the first */
    K_EXPECT(oom_select_victim() != current);    /* never the boot/idle task */

    /* every exclusion, each holding MORE than the one task allowed to die:
     * a zombie's frames are reaped, an embryo has no process yet, init and
     * idle hold the machine together, a kxld task is the kernel's, a task
     * with no pages would free nothing, and one already carrying SIGKILL is
     * on its way out and cannot be killed twice. */
    struct task *zomb = fake_task(6104, 1 << 20, T_ZOMBIE);
    struct task *embryo = fake_task(6105, 1 << 20, T_EMBRYO);
    struct task *init = fake_task(1, 1 << 20, T_RUNNABLE);
    struct task *kxld = fake_task(6106, 1 << 20, T_RUNNABLE);
    struct task *dry = fake_task(6107, 0, T_RUNNABLE);
    struct task *dying = fake_task(6108, 1 << 20, T_RUNNABLE);
    K_ASSERT(zomb && embryo && init && kxld && dry && dying);
    kxld->cred.kind = CRED_KXLD;
    dying->signal_pending = 1ULL << SIGKILL;

    K_EXPECT_EQ(oom_select_victim(), big); /* none of them displaced it */

    /* with the killable ones gone, there is nobody to choose: the table
     * holds nothing but the idle task, which the rules refuse. */
    task_free_slot(small);
    task_free_slot(big);
    task_free_slot(mid);
    task_free_slot(zomb);
    task_free_slot(embryo);
    task_free_slot(init);
    task_free_slot(kxld);
    task_free_slot(dry);
    task_free_slot(dying);
    K_EXPECT(oom_select_victim() == NULL);
    K_EXPECT(oom_select_victim() != current);
}

/* The policy end to end: the biggest holder dies, and only one of them
 * dies per shortage -- a fault storm must not walk the table to the last
 * task while memory is still dry.  One frame handed out releases the
 * latch, because that is the moment memory is flowing again. */
static void test_oom_kills_once_per_shortage(void)
{
    u64 free0 = pmm_free_bytes();
    struct task *first = fake_task(6121, 4096, T_RUNNABLE);
    struct task *second = fake_task(6122, 2048, T_RUNNABLE);
    K_ASSERT(first && second);
    oom_latch_reset();

    K_EXPECT(oom_kill_largest()); /* the hog pays */
    K_EXPECT(first->signal_pending & (1ULL << SIGKILL));
    K_EXPECT(!(second->signal_pending & (1ULL << SIGKILL)));

    /* still dry: no second victim, or every fault would cost a process */
    K_EXPECT(!oom_kill_largest());
    K_EXPECT(!(second->signal_pending & (1ULL << SIGKILL)));

    /* one frame changes hands -> the shortage is over -> the next one may
     * kill, and the task already dying is passed over for the next hog */
    oom_latch_reset();
    K_EXPECT(oom_kill_largest());
    K_EXPECT(second->signal_pending & (1ULL << SIGKILL));
    K_EXPECT(!oom_kill_largest()); /* and nobody is left to kill */

    first->signal_pending = 0;
    second->signal_pending = 0;
    task_free_slot(first);
    task_free_slot(second);
    oom_latch_reset();
    K_EXPECT_EQ(pmm_free_bytes(), free0);
}

/* A stopped task is a victim like any other: SIGKILL must wake it, because
 * T_STOPPED never returns to user space on its own -- a signal left pending
 * on it would never be delivered and its frames would never come back. */
static void test_oom_wakes_a_stopped_victim(void)
{
    struct task *v = fake_task(6131, 4096, T_STOPPED);
    K_ASSERT(v);
    K_EXPECT_EQ(oom_select_victim(), v);
    oom_latch_reset();

    K_EXPECT(oom_kill_largest());
    K_EXPECT_EQ(v->state, T_RUNNABLE);            /* moved, not merely marked */
    K_EXPECT(v->signal_pending & (1ULL << SIGKILL));
    K_EXPECT(v->rq_cpu >= 0);                     /* and really on a queue */

    v->signal_pending = 0;
    task_free_slot(v); /* dequeues on the way out: no fake left runnable */
    K_EXPECT_EQ(v->state, T_UNUSED);
    K_EXPECT_EQ(v->rq_cpu, -1);
    oom_latch_reset();
}

/* The trigger where it actually matters: a user #PF that cannot get a frame
 * refuses the fault exactly as before, and on the way out spends the life of
 * the process holding the most memory.  One allocation is made to fail the
 * way an empty buddy fails it, so no memory is really short here. */
static void test_oom_fault_kills_the_hog_before_refusing(void)
{
    K_ASSERT(current->vmas == NULL); /* the window t_mmap.c leaves behind */
    u64 count_before = vmm_count_user_pages(current->pml4);
    u64 fb_before = pmm_free_bytes();
    struct task *hog = fake_task(6141, 4096, T_RUNNABLE);
    K_ASSERT(hog);
    K_EXPECT_EQ(oom_select_victim(), hog);
    oom_latch_reset();

    long base = syscall_probe(SYS_mmap, 0, 0x1000, LNXRM_PROT_READ | LNXRM_PROT_WRITE);
    K_ASSERT(base > 0);

    struct intr_frame f;
    memset(&f, 0, sizeof(f));
    f.cs = 3;   /* user mode, the way isr_common() gates the real thing */
    f.err = 0;  /* not present, read: a plain first touch */
    pmm_inject_oom(1);
    bool fixed = page_fault_try_fix(&f, (u64)base);
    pmm_inject_oom(0);

    K_EXPECT(!fixed);                                          /* refused */
    K_EXPECT_EQ(vmm_translate_in(current->pml4, (u64)base), 0); /* nothing mapped */
    K_EXPECT(hog->signal_pending & (1ULL << SIGKILL));          /* the hog pays */

    K_EXPECT_EQ(syscall_probe(SYS_munmap, (u64)base, 0x1000, 0), 0);
    K_EXPECT(current->vmas == NULL);
    hog->signal_pending = 0;
    task_free_slot(hog);
    oom_latch_reset();
    K_EXPECT_EQ(vmm_count_user_pages(current->pml4), count_before);
    K_EXPECT_EQ(current->mem_pages, count_before);
    K_EXPECT_EQ(pmm_free_bytes(), fb_before);
}

KTEST("oom", KTEST_MM, test_kmalloc_buddy_oom_returns_null);
KTEST("oom", KTEST_MM, test_kmalloc_refill_oom_returns_null);
KTEST("oom", KTEST_MM, test_kmalloc_oversize_returns_null);
KTEST("oom", KTEST_MM, test_vmm_oom_is_an_error_not_a_panic);
KTEST("oom", KTEST_LATE, test_oom_selects_the_biggest_live_task);
KTEST("oom", KTEST_LATE, test_oom_kills_once_per_shortage);
KTEST("oom", KTEST_LATE, test_oom_wakes_a_stopped_victim);
KTEST("oom", KTEST_LATE, test_oom_fault_kills_the_hog_before_refusing);
