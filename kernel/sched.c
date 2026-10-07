/* Round-robin preemptive scheduler with per-CPU run queues.
 *
 * Queue topology:
 *   - Each cpu_info has its own circular run queue (sentinel at
 *     cpu->runq_head). A RUNNABLE task sits on exactly one queue,
 *     identified by task->rq_cpu (-1 == not queued).
 *   - A single global runq_lock (taken with interrupts off) serializes
 *     every enqueue/dequeue/pick so cross-CPU handoff cannot corrupt
 *     links. Critical sections are only a few pointer stores, so one
 *     lock is fine for MAX_CPUS=8.
 *   - The idle task of each CPU is NEVER on any runqueue; it is only
 *     entered as a fallback when pick_next() finds nothing runnable.
 */
#include <sys/sched.h>
#include <sys/percpu.h>
#include <console.h>
#include <mm/mm.h>
#include <sys/cpu.h>
#include <sys/spinlock.h>
#include <sys/apic.h>
#include <sys/smp.h>

static struct task task_table[NR_TASKS];

/* Global lock covering all per-CPU run queues. Always taken irqsave. */
static spinlock_t runq_lock = SPINLOCK_INIT;
/* Guards task_table allocation state (T_UNUSED <-> in-use). */
static spinlock_t task_table_lock = SPINLOCK_INIT;

/* In SMP mode, `current` is a function reading from per-CPU data. */
#ifndef CONFIG_SMP
struct task *current;
#else
struct task *get_current(void)
{ return this_cpu_data()->_current; }
#endif

static void switch_to(struct task *next);

/* Per-CPU pick_next: find runnable task from this CPU's queue. */
static struct task *pick_next(void)
{
    struct cpu_info *cpu = this_cpu_data();
    struct task *head = cpu->runq_head.rq_next;
    if (head == &cpu->runq_head) return NULL;
    return head;
}

/* Queue depth of one CPU's run queue (call with runq_lock held). */
static u32 runq_len(struct cpu_info *c)
{
    u32 n = 0;
    for (struct task *t = c->runq_head.rq_next; t != &c->runq_head; t = t->rq_next)
        n++;
    return n;
}

/* Choose the CPU that should receive `t`:
 *   1. This CPU if it is idle — the caller is awake here already, so the
 *      task runs with zero IPI/HLT-wakeup cost.
 *   2. Else any idle AP — spreads work off the BSP and keeps the BSP's
 *      queue empty for device IRQs (IOAPIC routes every IRQ to it).
 *   3. Else all CPUs are busy: the shortest queue, so a CPU that keeps
 *      forking cannot pile everything on itself while another drains.
 * Reads of c->idle/_current are advisory (taken under runq_lock but
 * written without it): a stale "idle" just costs one pointless IPI, and
 * a stale "busy" falls through to the next candidate. */
static struct cpu_info *preferred_cpu(void)
{
    struct cpu_info *self = this_cpu_data();

    if (self->started && self->idle && self->_current == self->idle) return self;

    for (u32 i = 1; i < MAX_CPUS; i++) {
        struct cpu_info *c = &cpu_table[i];
        if (c != self && c->started && c->idle && c->_current == c->idle) return c;
    }

    struct cpu_info *best = self;
    u32 best_len = runq_len(self);
    for (u32 i = 0; i < MAX_CPUS; i++) {
        struct cpu_info *c = &cpu_table[i];
        if (c == self || !c->started) continue;
        u32 n = runq_len(c);
        if (n < best_len) {
            best = c;
            best_len = n;
        }
    }
    return best;
}

/* Enqueue `t` at the HEAD of `cpu`'s queue. Caller holds runq_lock. */
static void enqueue_on(struct cpu_info *cpu, struct task *t)
{
    t->rq_cpu = (int)cpu->id;
    t->rq_next = cpu->runq_head.rq_next;
    cpu->runq_head.rq_next = t;
}

/* Add task to a run queue using the placement policy (self / idle CPU /
 * shortest queue). ONLY for tasks that are not executing anywhere
 * (sleepers being woken, fresh work): a running task's context is only
 * saved by its own swtch(), so parking it on a remote queue lets that
 * CPU schedule() it before this CPU has switched away — double run.
 * The outgoing current is requeued with runqueue_add_local() instead. */
void runqueue_add(struct task *t)
{
    u64 flags;
    spin_lock_irqsave(&runq_lock, &flags);
    if (t->rq_cpu != -1) {
        /* already queued somewhere */
        spin_unlock_irqrestore(&runq_lock, flags);
        return;
    }
    struct cpu_info *cpu = preferred_cpu();
    enqueue_on(cpu, t);
    spin_unlock_irqrestore(&runq_lock, flags);

    if (cpu == this_cpu_data()) {
        /* Local enqueue: flag it so a returning-to-user (or idle) context
         * reschedules immediately instead of waiting up to a full tick. */
        this_cpu_data()->need_resched = true;
    } else if (cpu->started) {
        /* Wake a remote idle out of HLT; reschedule_ipi_handler sets
         * need_resched so the interrupted context preempts on return. */
        apic_send_ipi(cpu->apic_id, IPI_VECTOR_RESCHEDULE, 0, 1);
    }
}

/* Requeue the outgoing current on THIS CPU's queue. Staying local is
 * both safe (we finish switch_to before anyone else can pick it up)
 * and cheap (no IPI); cross-CPU balancing happens on wakeups instead. */
void runqueue_add_local(struct task *t)
{
    u64 flags;
    spin_lock_irqsave(&runq_lock, &flags);
    if (t->rq_cpu != -1) {
        spin_unlock_irqrestore(&runq_lock, flags);
        return;
    }
    enqueue_on(this_cpu_data(), t);
    spin_unlock_irqrestore(&runq_lock, flags);
}

/* Remove task from whichever run queue owns it (task->rq_cpu).
 * Safe no-op if the task is not currently queued (running tasks live
 * off-queue; rq_cpu == -1). */
void runqueue_remove(struct task *t)
{
    u64 flags;
    spin_lock_irqsave(&runq_lock, &flags);
    int owner = t->rq_cpu;
    if (owner >= 0 && owner < (int)MAX_CPUS) {
        struct cpu_info *cpu = &cpu_table[owner];
        struct task *p = &cpu->runq_head;
        while (p->rq_next && p->rq_next != &cpu->runq_head && p->rq_next != t) p = p->rq_next;
        if (p->rq_next == t) p->rq_next = t->rq_next;
    }
    t->rq_next = NULL;
    t->rq_cpu = -1;
    spin_unlock_irqrestore(&runq_lock, flags);
}

extern void swtch(u64 *old_slot, u64 *new_slot);
void tss_set_rsp0(u64 rsp);

void sched_init(void)
{
    u64 flags;
    spin_lock_irqsave(&task_table_lock, &flags);
    memset(task_table, 0, sizeof(task_table));
    for (int i = 0; i < NR_TASKS; i++) {
        task_table[i].rq_cpu = -1; /* memset would leave 0 == "CPU 0"! */
        task_table[i].cpu_id = -1;
    }
    spin_unlock_irqrestore(&task_table_lock, flags);

    /* init BSP per-cpu data */
    struct cpu_info *bsp = this_cpu_data();
    bsp->id = 0;
    bsp->bsp = true;
    bsp->started = true;
    bsp->need_resched = false;

    /* set up idle task (pid 0) in task_table */
    struct task *idle = &task_table[0];
    idle->pid = 0;
    strcpy(idle->name, "idle");
    idle->state = T_RUNNING;
    idle->cpu_id = 0;
    idle->rq_cpu = -1;
    /* Idle must map the kernel PML4: switch_to() does
     * vmm_switch_to(next->pml4) whenever prev->pml4 differs, and a
     * zero CR3 would triple-fault the moment we park on idle. */
    idle->pml4 = master_pml4_phys();
    /* T-030: idle guards kernel integrity -- highest tier, and it never
     * makes a syscall (syscall_entry BUG_ONs on kxld). */
    idle->cred.kind = CRED_KXLD;
    idle->cred.uid = CRED_UID_PRIV;

    /* BSP current = idle; expose it to preferred_cpu()/AP bring-up. */
    bsp->idle = idle;
    bsp->_current = idle;

    /* set GS base for BSP */
    cpu_set_gs_base((u64)bsp);

    kprintf("[sched] task table %d slots, BSP idle ready\n", NR_TASKS);
}

/* Create a fresh idle task for the given CPU. Never enqueued. */
struct task *sched_create_idle(int cpu_id)
{
    u64 flags;
    struct task *t = NULL;
    spin_lock_irqsave(&task_table_lock, &flags);
    /* index 0 is the BSP idle; AP idles start at 1 */
    for (int i = (cpu_id == 0 ? 0 : 1); i < NR_TASKS; i++) {
        if (task_table[i].state == T_UNUSED) {
            task_table[i].state = T_EMBRYO;
            t = &task_table[i];
            break;
        }
    }
    spin_unlock_irqrestore(&task_table_lock, flags);
    if (!t) return NULL;

    t->pid = 0;
    if (cpu_id == 0)
        strcpy(t->name, "idle");
    else {
        /* "apN" — short enough for TASK_NAME_LEN */
        t->name[0] = 'a';
        t->name[1] = 'p';
        t->name[2] = (char)('0' + cpu_id);
        t->name[3] = 0;
    }
    t->state = T_RUNNING;
    t->cpu_id = cpu_id;
    t->rq_cpu = -1;
    t->rq_next = NULL;
    t->parent = NULL;
    t->kstack = NULL;
    t->kstack_top = NULL;
    /* Same justification as BSP idle: switch_to may load this as CR3. */
    t->pml4 = master_pml4_phys();
    t->ctx.sp = 0; /* filled by first swtch() away from this idle */
    /* T-030: every per-CPU idle is kxld, the kernel-protecting tier. */
    t->cred.kind = CRED_KXLD;
    t->cred.uid = CRED_UID_PRIV;
    return t;
}

void sched_maybe_preempt(struct intr_frame *f)
{
    if (!this_cpu_data()->need_resched || !current) return;
    if (current->state != T_RUNNING) return;
    /* User tasks only preempt on ring-3 return; the idle task (pid 0)
     * may also be preempted from its sti;hlt loop (kernel mode) so a
     * cross-CPU runqueue_add can land work without waiting for the
     * next explicit schedule() at the bottom of the idle loop. */
    if ((f->cs & 3) == 3 || current->pid == 0) {
        this_cpu_data()->need_resched = false;
        schedule();
    }
}

void sched_tick(void)
{
    /* Wake sleepers FIRST — must run even when idle is current, or a
     * nanosleep that switched to idle never gets picked up again
     * (PIT only fires on the BSP; idle's early return used to skip
     * this loop entirely). */
    for (int i = 1; i < NR_TASKS; i++) {
        struct task *t = &task_table[i];
        if (t->state == T_SLEEPING && jiffies >= t->sleep_until) {
            t->state = T_RUNNABLE;
            runqueue_add(t);
            this_cpu_data()->need_resched = true;
        }
    }

    if (!current || current->pid == 0) return; /* no quantum to burn on idle */

    if (current->quantum > 0) {
        current->quantum--;
        if (current->quantum == 0 && current->state == T_RUNNING)
            this_cpu_data()->need_resched = true;
    }
}

void schedule(void)
{
    if (!current) panic("schedule without current");
    bool cli_state = false;
    u64 fl;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(fl));
    cli_state = !!(fl & 0x200);

    struct task *next = pick_next();

    if (!next) {
        /* This CPU's queue is empty.
         *  - If current can still run (RUNNABLE/RUNNING user), just
         *    refresh its quantum and return (we are the only choice).
         *  - If current cannot run (SLEEPING/STOPPED/ZOMBIE — the
         *    caller already set state and expects us to leave), fall
         *    through to idle instead of the old busy-loop that spun
         *    forever with IF=0 (the nanosleep hang / exit deadlock). */
        if (current->state == T_RUNNING || current->state == T_RUNNABLE) {
            if (current->pid != 0) current->quantum = 6;
            goto out;
        }
        /* current cannot run: switch to this CPU's idle task. */
        next = this_cpu_data()->idle;
        if (!next || next == current) {
            /* No idle available (should not happen once sched_init /
             * ap_main are correct). Last resort: requeue a runnable
             * current, otherwise just leave. */
            if (current->state == T_RUNNABLE && current->pid != 0) { current->state = T_RUNNING; }
            goto out;
        }
        /* Do NOT requeue a non-running current here — handle below. */
    } else if (next == current) {
        if (current->state == T_RUNNING && current->pid != 0) current->quantum = 6;
        /* A task that is current should never sit on a runqueue; drop
         * a stale entry if one exists (e.g. raced wake while sleeping). */
        if (current->rq_cpu != -1) runqueue_remove(current);
        goto out;
    }

    /* Requeue current only if it is a non-idle task that can still run.
     * Idle is never enqueued; SLEEPING/STOPPED/ZOMBIE stay off-queue so
     * a later wake (send_signal / sched_tick) can runqueue_add them.
     * Local requeue is mandatory here: current's context only becomes
     * valid for another CPU after this function's switch_to — parking it
     * on a remote queue would let that CPU pick it too early (double run). */
    if (current->state == T_RUNNING && current->pid != 0) {
        current->state = T_RUNNABLE;
        runqueue_add_local(current);
    }
    runqueue_remove(next); /* idle is rq_cpu==-1 → no-op */
    switch_to(next);

out:
    if (cli_state) __asm__ volatile("sti");
}

/* Entered from start_kernel on the BSP (and, after the first switch
 * away, whenever the BSP idle is scheduled back in). Never returns. */
void idle_loop(void)
{
    for (;;) {
        schedule();
        __asm__ volatile("sti; hlt");
    }
}

static void switch_to(struct task *next)
{
    struct task *prev = current;
    next->state = T_RUNNING;
    next->quantum = 6;
    next->cpu_id = this_cpu_data()->id;
    if (prev) prev->cpu_id = -1;

    tss_set_rsp0((u64)next->kstack_top);
    if (prev != next && prev && prev->pml4 != next->pml4) vmm_switch_to(next->pml4);

    this_cpu_data()->_current = next;
    swtch((u64 *)&prev->ctx, &next->ctx.sp);
}

long sys_nanosleep(u64 ns, struct lnxrm_timespec *urem)
{
    u64 ms = ns / 1000000;
    if (!ms) ms = 1; /* the tick is the finest granularity we have */
    u64 start = jiffies;
    u64 until = start + ms * HZ / 1000 + 1;
    bool interrupted = false;

    while ((i64)(jiffies - until) < 0) {
        /* Abort only for a signal that would actually do something: a
         * pending-but-ignored SIGCHLD must not cut the sleep short. */
        u64 ready = current->signal_pending & ~current->sig_blocked;
        interrupted = false;
        for (int s = 1; ready && s < _NSIG; s++) {
            if ((ready & (1ULL << s)) && !signal_ignored(current, s)) {
                interrupted = true;
                break;
            }
        }
        if (interrupted) break;

        current->sleep_until = until;
        current->state = T_SLEEPING;
        runqueue_remove(current);
        schedule();
    }

    if (urem) {
        /* Report what is left of the caller's own request, not of the
         * tick-rounded deadline, so the value is in the same units the
         * caller passed in. */
        i64 elapsed_ns = (i64)(jiffies - start) * (1000000000LL / HZ);
        i64 left_ns = (i64)ns - elapsed_ns;
        struct lnxrm_timespec rem;

        if (left_ns < 0) left_ns = 0;
        rem.tv_sec = left_ns / 1000000000LL;
        rem.tv_nsec = left_ns % 1000000000LL;
        if (copy_to_user(urem, &rem, sizeof(rem)) < 0) return LNXRM_EFAULT;
    }

    if (interrupted) return LNXRM_EINTR;
    return 0;
}

/* ---- task table helpers used by task.c ---- */
struct task *task_alloc_slot(void)
{
    u64 flags;
    struct task *t = NULL;
    spin_lock_irqsave(&task_table_lock, &flags);
    for (int i = 1; i < NR_TASKS; i++) {
        if (task_table[i].state == T_UNUSED) {
            /* Claim immediately so a concurrent alloc cannot hand out
             * the same slot. Caller may overwrite state to T_EMBRYO. */
            task_table[i].state = T_EMBRYO;
            task_table[i].pid = 0; /* drop the old pid NOW: until the
              * caller assigns a fresh one, find_task(old_pid) must not
              * match this half-built task. Every caller assigns its own
              * pid right after alloc (fork/spawn/idle). */
            task_table[i].rq_cpu = -1;
            task_table[i].rq_next = NULL;
            task_table[i].cpu_id = -1;
            /* Slots are reused without being cleared, so a recycled task
             * must not inherit the previous occupant's signal state. */
            task_table[i].signal_pending = 0;
            task_table[i].sig_blocked = 0;
            task_table[i].sig_saved_blocked = 0;
            task_table[i].sig_in_handler = false;
            memset(task_table[i].sig_handlers, 0, sizeof(task_table[i].sig_handlers));
            memset(task_table[i].sig_masks, 0, sizeof(task_table[i].sig_masks));
            memset(&task_table[i].saved_tf, 0, sizeof(task_table[i].saved_tf));
            task_table[i].sig_fault_rip = 0;
            /* T-030: recycled slots start unprivileged.  Callers that
             * know better overwrite this at once (fork inherits, spawn
             * is root, idle is kxld) -- but a slot must never hand a
             * fresh task the previous occupant's tier. */
            task_table[i].cred.kind = CRED_USER;
            task_table[i].cred.uid = CRED_UID_USER;
            /* T-032: same rule for the memory account -- a slot must
             * never report the 64 MiB the previous occupant died holding. */
            task_table[i].mem_pages = 0;
            task_table[i].mem_peak = 0;
            t = &task_table[i];
            break;
        }
    }
    spin_unlock_irqrestore(&task_table_lock, flags);
    return t;
}

void task_free_slot(struct task *t)
{
    if (!t) return;
    u64 flags;
    spin_lock_irqsave(&task_table_lock, &flags);
    /* Belt-and-suspenders: never free a task still linked on a queue. */
    if (t->rq_cpu != -1) {
        spin_unlock_irqrestore(&task_table_lock, flags);
        runqueue_remove(t);
        spin_lock_irqsave(&task_table_lock, &flags);
    }
    t->state = T_UNUSED;
    t->cpu_id = -1;
    t->rq_cpu = -1;
    t->rq_next = NULL;
    /* T-032: hand the account back with the frames.  The caller (reap)
     * destroyed the address space a line ago, so anything left in these
     * two numbers is a claim on memory nobody owns. */
    t->mem_pages = 0;
    t->mem_peak = 0;
    spin_unlock_irqrestore(&task_table_lock, flags);
}

struct task *find_task(u32 pid)
{
    u64 flags;
    struct task *t = NULL;
    /* Scan under the table lock: without it a concurrent
     * task_free_slot/task_alloc_slot can swap the slot mid-read (we
     * would observe pid from one occupant and state from the next).
     * The returned pointer is still racy afterwards — callers go
     * through send_signal's state recheck (kernel/signal.c). */
    spin_lock_irqsave(&task_table_lock, &flags);
    for (int i = 0; i < NR_TASKS; i++) {
        if (task_table[i].state != T_UNUSED && task_table[i].pid == pid) {
            t = &task_table[i];
            break;
        }
    }
    spin_unlock_irqrestore(&task_table_lock, flags);
    return t;
}

struct task *task_iter(int *i)
{
    while (*i < NR_TASKS) {
        struct task *t = &task_table[(*i)++];
        if (t->state != T_UNUSED) return t;
    }
    return NULL;
}
