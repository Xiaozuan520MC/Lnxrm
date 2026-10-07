/* Scheduler and task definitions (includes POSIX-like signal support). */
#pragma once
#include <types.h>
#include <sys/spinlock.h>
#include <sys/cred.h>

/* Forward declarations for percpu.h (breaks circular dependency) */
struct cpu_info;

#ifdef __cplusplus
extern "C" {
#endif

/* ---- signals (kernel/signal.c) ----
 * Signal numbers and the user-facing `struct lnxrm_sigaction` live in
 * <abi/lnxrm_abi.h> so that kernel and user space cannot drift apart.
 * types.h (included below) pulls that header in. */
#define _NSIG LNXRM_NR_SIGNALS

LNXRM_STATIC_ASSERT(SIGKILL == 9 && SIGSTOP == 19 && SIGCONT == 18,
                    "signal numbering must match the ABI header");

/* ---- interrupt frame & task context ---- */
/* Interrupt frame pushed by the stubs in entry64.S (ascending addresses). */
struct intr_frame {
    u64 r15, r14, r13, r12;
    u64 r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rax;
    u64 rbx; /* callee-saved: must survive the C handler */
    u64 intno;
    u64 err;
    u64 rip, cs, rflags, ussp, usss;
};

/* Callee-saved context for swtch(): holds the saved-kernel-stack pointer.
 * The register save area itself lives on the task's kernel stack. */
struct cpu_ctx {
    u64 sp;
};

enum task_state {
    T_UNUSED,
    T_EMBRYO,
    T_RUNNABLE,
    T_RUNNING,
    T_SLEEPING,
    T_ZOMBIE,
    T_STOPPED,
};

#define TASK_NAME_LEN 16

struct task {
    u32 pid;
    u32 cpu_id; /* CPU running this task (-1 if none) */
    enum task_state state;
    char name[TASK_NAME_LEN];
    struct cred cred; /* T-030: privilege tier + uid (fork copies it) */

    struct intr_frame *tf; /* points into kstack while in kernel */
    struct cpu_ctx ctx;
    char *kstack; /* kmalloc'd KSTACK_SIZE */
    char *kstack_top;

    u64 pml4;       /* CR3 physical */
    void *brk_base; /* user heap start (= end of segments) */
    void *brk_cur;

    /* T-032: what this task's user space costs, in 4 KiB pages.
     * mem_pages is refreshed from the page tables themselves (at exec,
     * fork and every brk) instead of being bumped wherever a page happens
     * to be mapped, so it cannot drift out of step with the truth; it dies
     * with the address space.  mem_peak is the high-water mark of it --
     * ps shows both, which is how "who owns the RAM" and "who ballooned"
     * are answered.  The ceiling that limits growth is not stored here:
     * cred_mem_quota(&cred) derives it from the tier, so setuid() and the
     * quota can never disagree (one fact, one answer -- C24). */
    u64 mem_pages;
    u64 mem_peak;

    struct file *fds[NR_FDS];

    int exit_code;
    u64 sleep_until;
    int quantum;

    struct task *parent;
    struct task *rq_next; /* runqueue link */
    int rq_cpu;           /* CPU whose runqueue holds this task; -1 = not queued */

    /* signal support */
    u64 signal_pending; /* bitmask of pending signals */
    u64 sig_blocked;    /* bitmask of blocked signals */
    u64 sig_saved_blocked; /* sig_blocked to restore on sigreturn */
    void *sig_handlers[_NSIG]; /* NULL = default, (void*)1 = ignore */
    u64 sig_masks[_NSIG];      /* sa_mask active while that handler runs */
    bool sig_in_handler;       /* a user handler frame is currently live */
    struct intr_frame saved_tf; /* interrupted frame to restore via sigreturn */
    u64 sig_fault_rip;         /* RIP already given a fault handler once */
};

/* current task macro: SMP mode reads from per-CPU data via GS. */
#ifdef CONFIG_SMP
#include <sys/percpu.h>
struct task *get_current(void);
#define current (get_current())
#else
extern struct task *current;
#endif

#define KSTACK_SIZE 32768

int sys_fork(void);
long sys_execve(const char *path, char *const argv[], char *const envp[]);
void sys_exit(int code) __attribute__((noreturn));
int sys_waitpid(int pid, int *status, int opts);
long sys_nanosleep(u64 ns, struct lnxrm_timespec *urem);
long sys_ps(struct lnxrm_ps_entry *ubuf, int max);
long sys_sigaction(int sig, const struct lnxrm_sigaction *uact,
                   struct lnxrm_sigaction *uold);
long sys_sigprocmask(int how, const u64 *uset, u64 *uold);
long sys_sigreturn(void);
long sys_uname(struct lnxrm_utsname *u);
u32 next_pid(void);
struct task *find_task(u32 pid);

int copy_from_user(void *kdst, const void *usrc, size_t n);
int copy_to_user(void *udst, const void *ksrc, size_t n);
/* True iff [p, p+n) lies in user space and every page is mapped. */
bool user_ptr_ok(u64 p, u64 n);
/* Same, plus: every page is mapped writable.  This is what a kernel STORE
 * through a user address needs -- with CR0.WP on, writing a read-only user
 * page (text/.rodata) from ring 0 would raise #PF, and this kernel has no
 * #PF fixup table, so callers check first and report LNXRM_EFAULT. */
bool user_ptr_writable(u64 p, u64 n);
/* Copy a NUL-terminated string from user space (byte-wise validated).
 * Always NUL-terminates kdst. Returns 0 on success, -1 if any byte of the
 * source is outside the user range or not mapped. */
int copy_user_str(char *kdst, u64 usrc, size_t max);

/* scheduler (kernel/sched.c) */
void sched_init(void);
void schedule(void);
void sched_tick(void); /* PIT hook: quantum expiry */
void sched_maybe_preempt(struct intr_frame *f);
struct task *task_iter(int *i);

/* signal helpers (kernel/signal.c) */
void send_signal(struct task *t, int sig);
void do_signal_check(struct task *t);
/* Map the execute-only sigreturn restorer page into a fresh address space
 * (every other user page is NX, so the handler cannot ret into its stack). */
int signal_map_restorer(u64 pml4); /* 0, or LNXRM_ENOMEM */
/* True when `sig` would currently do nothing to `t` (SIG_IGN, or the
 * default ignore disposition).  SIGKILL / SIGSTOP are never ignored. */
bool signal_ignored(struct task *t, int sig);
void runqueue_add(struct task *t);void runqueue_remove(struct task *t);
/* Requeue the outgoing current on THIS CPU's queue (see sched.c). */
void runqueue_add_local(struct task *t);
void idle_loop(void) __attribute__((noreturn));
struct task *task_alloc_slot(void);
void task_free_slot(struct task *t);
struct task *sched_create_idle(int cpu_id);

#ifdef __cplusplus
}
#endif
