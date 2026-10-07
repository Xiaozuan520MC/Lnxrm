/* Late self-tests: task-table bookkeeping.
 *
 * They run after sched_init() and before the first real process is spawned,
 * while the only occupied slot is the BSP idle task -- so slot claims and
 * recycling can be checked without racing a scheduler that is already
 * switching processes. */
#include <sys/ktest.h>
#include <sys/sched.h>
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

KTEST("task", KTEST_LATE, test_current_is_bsp_idle);
KTEST("task", KTEST_LATE, test_slot_claim_and_recycle);
KTEST("task", KTEST_LATE, test_find_task_misses_unused_pids);
KTEST("task", KTEST_LATE, test_task_table_wellformed);
