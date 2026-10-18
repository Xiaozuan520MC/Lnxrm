/* Late self-tests: task-table bookkeeping.
 *
 * They run after sched_init() and before the first real process is spawned,
 * while the only occupied slot is the BSP idle task -- so slot claims and
 * recycling can be checked without racing a scheduler that is already
 * switching processes. */
#include <sys/ktest.h>
#include <sys/sched.h>
#include <sys/percpu.h>
#include <sys/cpu.h>   /* console_waiter_arm/disarm, input_push/pop (T-054) */
#include <console.h>

static void test_current_is_bsp_idle(void)
{
    struct task *cur = current;
    K_ASSERT(cur != NULL);
    K_EXPECT_EQ(cur->pid, 0); /* pid 0 is the idle task, no user process yet */
    K_EXPECT(cur->state == T_RUNNING || cur->state == T_RUNNABLE);
    K_EXPECT_EQ(cur->rq_cpu, (u64)-1); /* idle is never put on a runqueue */
    K_EXPECT(find_task(cur->pid) == cur); /* and it is findable by its pid */
}

static void test_slot_claim_and_recycle(void)
{
    struct task *a = task_alloc_slot();
    K_ASSERT(a != NULL);
    K_EXPECT_EQ(a->state, T_EMBRYO);
    K_EXPECT_EQ(a->rq_cpu, -1);
    /* cpu_id is a u32 that stores -1 for "not running anywhere" */
    K_EXPECT_EQ(a->cpu_id, (u32)-1);

    /* poison the signal state the way a live task would have it */
    a->signal_pending = 0xdeadbeefUL;
    a->sig_blocked = 0xdeadbeefUL;

    /* "claim immediately": a second alloc must never hand out the slot
     * that is still being filled in */
    struct task *b = task_alloc_slot();
    K_ASSERT(b != NULL);
    K_EXPECT(b != a);
    K_EXPECT_EQ(b->state, T_EMBRYO);

    task_free_slot(a);
    task_free_slot(b);
    K_EXPECT_EQ(a->state, T_UNUSED);
    K_EXPECT_EQ(b->state, T_UNUSED);

    /* a recycled slot must not inherit the previous owner's signals */
    struct task *c = task_alloc_slot();
    K_ASSERT(c != NULL);
    K_EXPECT_EQ(c->signal_pending, 0);
    K_EXPECT_EQ(c->sig_blocked, 0);
    K_EXPECT_EQ(c->sig_saved_blocked, 0);
    K_EXPECT_EQ(c->state, T_EMBRYO);
    task_free_slot(c);
}

static void test_find_task_misses_unused_pids(void)
{
    K_EXPECT(find_task(12345) == NULL);
    K_EXPECT(find_task(0x7ffffff0) == NULL);
}

static void test_task_table_wellformed(void)
{
    int seen = 0;
    int i = 0;
    for (struct task *t = task_iter(&i); t; t = task_iter(&i)) {
        /* only states the scheduler knows about */
        K_EXPECT(t->state >= T_UNUSED && t->state <= T_STOPPED);
        /* cpu_id is either "not running anywhere" (the u32 that holds -1)
         * or a valid CPU index */
        K_EXPECT((int)t->cpu_id == -1 || t->cpu_id < (u32)MAX_CPUS);
        seen++;
    }
    K_EXPECT(seen >= 1);       /* at least the BSP idle task */
    K_EXPECT(seen < NR_TASKS); /* and it did not swallow the whole table */
}

/* The scheduler's run queue is a push-front list, NOT round-robin (C62).
 * This test pins the contract documented in sched.c so a future change to
 * real round-robin has to update the comment AND this test together. */
static void test_runqueue_is_push_front(void)
{
    struct cpu_info *cpu = this_cpu_data();
    struct task *a = task_alloc_slot();
    struct task *b = task_alloc_slot();
    K_ASSERT(a && b);
    a->state = T_RUNNABLE;
    b->state = T_RUNNABLE;

    struct task *was_first = cpu->runq_head.rq_next;

    /* push-front: the most recently added task sits at the head */
    runqueue_add_local(a);
    K_EXPECT_EQ(cpu->runq_head.rq_next, a); /* a is now first */
    K_EXPECT_EQ(a->rq_next, was_first);      /* and links to the old first */
    K_EXPECT_EQ(a->rq_cpu, (int)cpu->id);

    runqueue_add_local(b);
    K_EXPECT_EQ(cpu->runq_head.rq_next, b); /* b (last added) is first */
    K_EXPECT_EQ(b->rq_next, a);             /* LIFO: b before a */
    K_EXPECT_EQ(a->rq_next, was_first);

    /* adding an already-queued task must be a no-op (no self-loop) */
    runqueue_add_local(b);
    K_EXPECT_EQ(cpu->runq_head.rq_next, b);
    K_EXPECT_EQ(b->rq_next, a);

    /* removal restores the "not queued" invariant */
    runqueue_remove(a);
    K_EXPECT_EQ(a->rq_cpu, -1);
    K_EXPECT(a->rq_next == NULL);
    runqueue_remove(b);
    K_EXPECT_EQ(b->rq_cpu, -1);
    K_EXPECT_EQ(cpu->runq_head.rq_next, was_first); /* queue restored */

    task_free_slot(a);
    task_free_slot(b);
}

/* T-050: (pid, generation) names exactly one *incarnation*.  pid alone is
 * unique only while pid_counter has not wrapped; the generation is what makes
 * a stale reference miss instead of answering in a stranger's name. */
static void test_generation_names_one_incarnation(void)
{
    struct task *a = task_alloc_slot();
    K_ASSERT(a != NULL);
    a->pid = 4242;
    u32 g = a->generation;
    K_EXPECT(g != 0);

    /* an exact (pid, gen) reference finds the live incarnation */
    K_EXPECT_EQ(find_task_gen(4242, g), a);
    K_EXPECT_EQ(find_task(4242), a);      /* pid-only: whoever holds it now */

    /* dead: neither lookup may answer */
    task_free_slot(a);
    K_EXPECT(find_task(4242) == NULL);
    K_EXPECT(find_task_gen(4242, g) == NULL);

    /* a fresh claim is a NEW incarnation -- generations never repeat (the
     * counter is monotonic), so even a wrap handing out 4242 again leaves the
     * old reference missing while the new one resolves. */
    struct task *b = task_alloc_slot();
    K_ASSERT(b != NULL);
    K_EXPECT(b->generation != g);
    b->pid = 4242;                        /* simulate pid_counter wrapping */
    K_EXPECT(find_task_gen(4242, g) == NULL);         /* stale ref: miss */
    K_EXPECT_EQ(find_task_gen(4242, b->generation), b); /* live ref: hit */
    K_EXPECT_EQ(find_task(4242), b);     /* pid-only now sees the new holder */
    task_free_slot(b);
}

/* T-050/T-051: the parent link names an incarnation, never a slot.  Freeing
 * the parent must leave no child able to resolve it -- least of all to a later
 * occupant of the same slot wearing the same pid. */
static void test_parent_link_dies_with_incarnation(void)
{
    struct task *p = task_alloc_slot();
    struct task *c = task_alloc_slot();
    K_ASSERT(p && c);
    p->pid = 7001;
    c->pid = 7002;
    c->parent_pid = p->pid;               /* wired the way fork wires it */
    c->parent_gen = p->generation;
    K_EXPECT_EQ(task_parent(c), p);

    /* Free the parent.  The free-time sweep (T-051) must leave the child
     * pointing at nothing.  (No pid-1 task exists yet at KTEST_LATE, so
     * "reparent to init" lands on the no-parent case.) */
    u32 dead_gen = p->generation;
    task_free_slot(p);
    K_EXPECT(task_parent(c) == NULL);

    /* Recycle the slot with the SAME pid -- a pid_counter wrap.  The child
     * must not adopt the stranger: only the generation can tell them apart. */
    struct task *p2 = task_alloc_slot();
    K_ASSERT(p2 != NULL);
    p2->pid = 7001;                       /* same pid, new generation */
    K_EXPECT(p2->generation != dead_gen);
    c->parent_pid = 7001;                 /* point at the pid... */
    c->parent_gen = dead_gen;             /* ...of the DEAD incarnation */
    K_EXPECT(task_parent(c) == NULL);     /* the generation is what rejects it */
    c->parent_gen = p2->generation;       /* now: the live incarnation */
    K_EXPECT_EQ(task_parent(c), p2);

    task_free_slot(p2);
    task_free_slot(c);
}

/* T-054: one input byte wakes *every* blocked console reader.  Under the old
 * single slot the second arm() displaced the first, and a displaced waiter
 * could only recover on its 1-jiffy timeout -- so with two readers a keypress
 * reached one of them and left the other polling. */
static void test_multi_console_waiters_all_woken(void)
{
    struct task *a = task_alloc_slot();
    struct task *b = task_alloc_slot();
    K_ASSERT(a && b);
    a->pid = 8001;
    b->pid = 8002;
    a->state = T_SLEEPING;
    b->state = T_SLEEPING;

    console_waiter_arm(a);
    console_waiter_arm(b);   /* must NOT displace a */

    input_push('x');         /* a single byte */

    /* both readers wake; neither is left waiting on its timeout */
    K_EXPECT_EQ(a->state, T_RUNNABLE);
    K_EXPECT_EQ(b->state, T_RUNNABLE);

    /* cleanup: disarm is a no-op (input_push emptied the list), task_free_slot
     * unlinks them from the runqueue input_push just put them on, and the byte
     * is drained so it cannot leak into a later test. */
    console_waiter_disarm(a);
    console_waiter_disarm(b);
    task_free_slot(a);
    task_free_slot(b);
    K_EXPECT_EQ(input_pop(), (int)'x');
}

KTEST("task", KTEST_LATE, test_current_is_bsp_idle);
KTEST("task", KTEST_LATE, test_slot_claim_and_recycle);
KTEST("task", KTEST_LATE, test_find_task_misses_unused_pids);
KTEST("task", KTEST_LATE, test_task_table_wellformed);
KTEST("task", KTEST_LATE, test_runqueue_is_push_front);
KTEST("task", KTEST_LATE, test_generation_names_one_incarnation);
KTEST("task", KTEST_LATE, test_parent_link_dies_with_incarnation);
KTEST("task", KTEST_LATE, test_multi_console_waiters_all_woken);
