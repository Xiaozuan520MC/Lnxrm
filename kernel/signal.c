/* Signal delivery: default dispositions, user-installed handlers and the
 * sigreturn path that gets a process back to where it was interrupted.
 *
 * Disposition per signal (struct task):
 *   sig_handlers[sig] == NULL  -> SIG_DFL  (terminate / stop / ignore)
 *   sig_handlers[sig] == (void*)1 -> SIG_IGN
 *   anything else              -> user handler address
 */
#include <sys/sched.h>
#include <sys/cpu.h>
#include <console.h>
#include <mm/mm.h>

/* Signals whose default disposition terminates the process.  Everything
 * else defaults to ignore, matching the historical behaviour of this
 * kernel (SIGFPE/SIGBUS are raised straight to sys_exit by the CPU
 * exception path in isr.c, so they never reach this table). */
static bool sig_default_terminates(int sig)
{
    return sig == SIGINT || sig == SIGTERM || sig == SIGQUIT || sig == SIGILL ||
           sig == SIGABRT || sig == SIGSEGV;
}

/* True when `sig` is currently a no-op for task `t`: either the caller
 * installed SIG_IGN, or it still has the default ignore disposition.
 * SIGKILL / SIGSTOP can never be ignored. */
bool signal_ignored(struct task *t, int sig)
{
    if (!t || sig < 1 || sig >= _NSIG) return true;
    if (sig == SIGKILL || sig == SIGSTOP) return false;
    void *h = t->sig_handlers[sig];
    if (h == SIG_IGN) return true;
    if (h != SIG_DFL) return false; /* a real handler is never "ignored" */
    return !sig_default_terminates(sig);
}

/* Send signal `sig` to task `t`. */
void send_signal(struct task *t, int sig)
{
    /* Recheck after find_task's unlock window: the slot may have been
     * freed (waitpid on another CPU) or recycled. Writing signal bits
     * into a dead/reused task would signal the wrong process. */
    if (sig < 1 || sig >= _NSIG || !t) return;
    if (t->state == T_UNUSED) return;

    /* SIGKILL and SIGSTOP cannot be ignored/blocked */
    if (sig == SIGKILL) {
        t->signal_pending |= (1ULL << SIGKILL);
        /* wake if sleeping so it can be killed */
        if (t->state == T_SLEEPING) {
            t->state = T_RUNNABLE;
            runqueue_add(t);
        }
        return;
    }

    if (sig == SIGSTOP) {
        t->signal_pending |= (1ULL << SIGSTOP);
        if (t->state == T_SLEEPING) {
            t->state = T_RUNNABLE;
            runqueue_add(t);
        }
        return;
    }

    /* SIGCONT must wake a stopped task, otherwise it stays off every
     * runqueue forever (STOPPED is not SLEEPING, so the generic wake
     * below would not catch it). */
    if (sig == SIGCONT) {
        t->signal_pending |= (1ULL << SIGCONT);
        if (t->state == T_STOPPED || t->state == T_SLEEPING) {
            t->state = T_RUNNABLE;
            runqueue_add(t);
        }
        return;
    }

    /* set pending bit */
    t->signal_pending |= (1ULL << sig);

    /* if task is sleeping, wake it up to deliver the signal */
    if (t->state == T_SLEEPING) {
        t->state = T_RUNNABLE;
        runqueue_add(t);
    }
}

/* Machine code for the restorer the handler `ret`s into:
 *     mov eax, 20      ; SYS_sigreturn
 *     int 0x80
 *     ud2              ; must never fall through
 * Every user page except code is NX, so this cannot be written onto the
 * stack any more: signal_map_restorer() puts it in a page of its own,
 * mapped execute-only-enough at USER_SIGRETURN_VA when the address space
 * is created. */
static const u8 sigreturn_code[] = {0xb8, 0x14, 0x00, 0x00, 0x00, 0xcd, 0x80, 0x0f, 0x0b};

/* Map the sigreturn restorer into `pml4`.  Called from the execve/spawn
 * paths; fork inherits the page through dup_user_aspace().
 * Returns LNXRM_ENOMEM when the frame or the mapping is not available: a
 * user program may well be the reason memory is short (T-004), and this
 * runs on a path that has a perfectly good error return to use. */
int signal_map_restorer(u64 pml4)
{
    if (vmm_translate_in(pml4, USER_SIGRETURN_VA)) return 0; /* already mapped */
    u64 pa = pmm_alloc();
    if (!pa) return LNXRM_ENOMEM;
    memset((void *)PHYS_TO_VIRT(pa), 0, PAGE_SIZE);
    memcpy((void *)PHYS_TO_VIRT(pa), sigreturn_code, sizeof(sigreturn_code));
    /* RX: the handler jumps here, and nothing ever writes it again. */
    if (vmm_map_user(pml4, USER_SIGRETURN_VA, pa, false, true, true) < 0) {
        pmm_free(pa);
        return LNXRM_ENOMEM;
    }
    return 0;
}

/* Redirect `t`'s return-to-user frame at `handler`, keeping the frame it
 * was about to resume in t->saved_tf so SYS_sigreturn can put it back. */
static void deliver_handler(struct task *t, int sig, void *handler)
{
    struct intr_frame *f = t->tf;
    u64 sp;

    if (!f) return; /* no frame to redirect (not on a return-to-user path) */

    t->saved_tf = *f;
    t->sig_saved_blocked = t->sig_blocked;
    t->sig_in_handler = true;
    t->sig_blocked |= (1ULL << sig) | t->sig_masks[sig];

    /* Build the frame a `call` would have produced: a return address
     * pointing at the restorer page, with rsp % 16 == 8 on handler entry
     * as the SysV ABI requires. */
    sp = f->ussp;
    sp -= 64;                 /* keep clear of the live stack */
    sp &= ~15UL;              /* restorer return slot at a 16-byte boundary */
    /* Validate [sp-8, sp+8): the return slot written below sits at sp-8
     * (rsp % 16 == 8 on handler entry), which is NOT inside the aligned
     * 16 bytes at sp -- checking only those used to leave the actual store
     * one page short of coverage.  Unmapped or read-only (CR0.WP) both
     * mean the frame cannot be built: kill like an overflowing stack. */
    if (!user_ptr_writable(sp - 8, 16))
        sys_exit(-SIGSEGV);   /* noreturn: like an overflowing stack */
    sp -= 8;
    { /* SMAP: the return slot lives on a user page */
        u64 ac = smap_enter();
        *(u64 *)sp = USER_SIGRETURN_VA;
        smap_leave(ac);
    }
    f->ussp = sp;
    f->rip = (u64)handler;
    f->rdi = (u64)sig;
    f->rflags |= 0x200;
}

/* Apply every pending signal that is deliverable right now.  Called on
 * the way back to user space (syscall return and IRQ return), i.e. with
 * t == current and t->tf pointing at the frame that is about to be
 * restored by iretq. */
void do_signal_check(struct task *t)
{
    if (!t || t->pid == 0) return;

    if (!t->signal_pending) return;

    /* SIGSTOP: uncatchable and unblockable, park at a scheduling point */
    if (t->signal_pending & (1ULL << SIGSTOP)) {
        t->signal_pending &= ~(1ULL << SIGSTOP);
        t->state = T_STOPPED;
        /* Park immediately: schedule() will not requeue a STOPPED task.
         * send_signal(SIGCONT) puts it back on a runqueue later. */
        schedule();
        return;
    }

    /* SIGCONT: continue a stopped process */
    if (t->signal_pending & (1ULL << SIGCONT)) {
        void *h = t->sig_handlers[SIGCONT];
        if (h == SIG_DFL || h == SIG_IGN)
            t->signal_pending &= ~(1ULL << SIGCONT);
        if (t->state == T_STOPPED) {
            t->state = T_RUNNABLE;
            if (t != current) runqueue_add(t);
        }
        /* a user handler for SIGCONT falls through to the loop below */
    }

    for (int sig = 1; sig < _NSIG; sig++) {
        if (!(t->signal_pending & (1ULL << sig))) continue;
        /* Blocked signals keep their pending bit: they are delivered
         * after sigprocmask unblocks them, not dropped. */
        if (t->sig_blocked & (1ULL << sig)) continue;

        void *h = t->sig_handlers[sig];

        if (h == SIG_IGN || (h == SIG_DFL && !sig_default_terminates(sig))) {
            t->signal_pending &= ~(1ULL << sig);
            continue;
        }

        if (h != SIG_DFL) {
            /* user-installed handler */
            if (t != current || !t->tf) continue;
            /* One frame at a time: a second signal raised while a
             * handler is running stays pending until sigreturn. */
            if (t->sig_in_handler) continue;
            t->signal_pending &= ~(1ULL << sig);
            deliver_handler(t, sig, h);
            return;
        }

        /* default disposition: terminate */
        t->signal_pending &= ~(1ULL << sig);
        if (sig == SIGKILL) sys_exit(-SIGKILL);
        if (sig_default_terminates(sig)) sys_exit(-sig);
    }
}

/* ================= sigaction ================= */
long sys_sigaction(int sig, const struct lnxrm_sigaction *uact, struct lnxrm_sigaction *uold)
{
    struct lnxrm_sigaction kact;

    if (sig < 1 || sig >= _NSIG) return LNXRM_EINVAL;
    /* SIGKILL / SIGSTOP may not be caught or ignored (POSIX). */
    if (sig == SIGKILL || sig == SIGSTOP) return LNXRM_EINVAL;

    if (uold) {
        memset(&kact, 0, sizeof(kact));
        kact.sa_handler = current->sig_handlers[sig];
        kact.sa_mask = current->sig_masks[sig];
        if (copy_to_user(uold, &kact, sizeof(kact)) < 0) return LNXRM_EFAULT;
    }

    if (!uact) return 0;

    if (copy_from_user(&kact, uact, sizeof(kact)) < 0) return LNXRM_EFAULT;
    /* Reject a handler address that is neither a sentinel nor mapped
     * user code -- otherwise the next signal would iretq into nowhere. */
    if ((u64)kact.sa_handler > (u64)LNXRM_SIG_IGN &&
        !user_ptr_ok((u64)kact.sa_handler, 1))
        return LNXRM_EFAULT;

    current->sig_masks[sig] = kact.sa_mask & ~((1ULL << SIGKILL) | (1ULL << SIGSTOP));
    current->sig_handlers[sig] = kact.sa_handler;
    return 0;
}

/* ================= sigprocmask ================= */
long sys_sigprocmask(int how, const u64 *uset, u64 *uold)
{
    u64 old = current->sig_blocked;
    u64 set;

    if (uold && copy_to_user(uold, &old, sizeof(old)) < 0) return LNXRM_EFAULT;
    if (!uset) return 0;

    if (copy_from_user(&set, uset, sizeof(set)) < 0) return LNXRM_EFAULT;
    /* SIGKILL / SIGSTOP are always deliverable. */
    set &= ~((1ULL << SIGKILL) | (1ULL << SIGSTOP));

    switch (how) {
    case LNXRM_SIG_BLOCK: current->sig_blocked |= set; break;
    case LNXRM_SIG_UNBLOCK: current->sig_blocked &= ~set; break;
    case LNXRM_SIG_SETMASK: current->sig_blocked = set; break;
    default: return LNXRM_EINVAL;
    }
    return 0;
}

/* ================= sigreturn ================= */
long sys_sigreturn(void)
{
    struct task *t = current;
    struct intr_frame *f = t->tf;
    long rax;

    if (!t->sig_in_handler || !f) return LNXRM_EINVAL;

    /* The interrupted context lives in saved_tf.  Its rax is returned as
     * the syscall result because syscall_entry() writes `ret` back into
     * f->rax right after we return; every other field is copied over
     * directly. */
    rax = (long)t->saved_tf.rax;
    *f = t->saved_tf;
    t->sig_in_handler = false;
    t->sig_blocked = t->sig_saved_blocked;
    return rax;
}
