/* Process management: pid allocation, fork/execve/exit/waitpid, the user
 * memory access helpers and /bin/init spawning. */
#include <sys/sched.h>
#include <io.h>
#include <sys/vfs.h>
#include <console.h>
#include <framebuffer.h>
#include <mm/mm.h>
#include <mm/pagefault.h>
#include <elf.h>
#include <sys/cpu.h>
#include <sys/spinlock.h>

/* The user-visible state numbering in <abi/lnxrm_abi.h> is what `ps`
 * hands out; it must stay identical to the kernel's enum. */
LNXRM_STATIC_ASSERT((int)LNXRM_TS_RUNNABLE == (int)T_RUNNABLE &&
                        (int)LNXRM_TS_RUNNING == (int)T_RUNNING &&
                        (int)LNXRM_TS_SLEEPING == (int)T_SLEEPING &&
                        (int)LNXRM_TS_ZOMBIE == (int)T_ZOMBIE &&
                        (int)LNXRM_TS_STOPPED == (int)T_STOPPED,
                    "lnxrm_task_state must match enum task_state");

extern void glue_first(void);
struct task *task_alloc_slot(void);
void task_free_slot(struct task *t);
void runqueue_add(struct task *);
void runqueue_remove(struct task *);

static u32 pid_counter;
static spinlock_t pid_lock = SPINLOCK_INIT;

u32 next_pid(void)
{
    u64 flags;
    spin_lock_irqsave(&pid_lock, &flags);
    u32 pid = ++pid_counter;
    spin_unlock_irqrestore(&pid_lock, flags);
    return pid;
}

/* Materialize a not-present user page exactly the way a real #PF would.
 *
 * The two gates below run BEFORE the copy, so a lazy page -- an mmap
 * region's first touch, a stack page below the watermark -- that the copy
 * itself would have faulted in must be faulted in here.  Without this,
 * read() into an untouched mmap buffer answers -EFAULT where Linux's
 * copy_to_user simply takes the #PF and carries on, which would leave
 * anonymous mmap usable only after the program has poked every page
 * itself.
 *
 * Nothing else changes: the fixers keep every rule (VMA prot, quota, the
 * stack guard, the RSP window), so a pointer they still refuse fails the
 * gate exactly as it did before -- a kernel address, a non-canonical one,
 * a hole in the window and a read-only page under a write all still say
 * no.  err is the real not-present code (bit 1 set for a write) so
 * demand_page asks the VMA for the right permission; cs marks it CPL3 and
 * ussp stays 0, which is outside user space, so stack_grow's RSP window
 * is skipped -- this pointer was NAMED by the caller, not arrived at by a
 * running stack, and it is still confined to the stack window, the guard
 * and the quota. */
static bool user_page_in(u64 page, bool write)
{
    struct intr_frame f;
    memset(&f, 0, sizeof(f));
    f.cs = 3;
    f.err = write ? 2 : 0;
    return page_fault_try_fix(&f, page);
}

bool user_ptr_ok(u64 p, u64 n)
{
    if (!n || !vmm_is_user_range(p, p + n)) return false;
    u64 lo = p & ~4095ULL;
    u64 hi = (p + n - 1) & ~4095ULL;
    for (u64 pg = lo; pg <= hi; pg += 4096)
        if (!vmm_translate_in(current->pml4, pg) && !user_page_in(pg, false))
            return false;
    return true;
}

bool user_ptr_writable(u64 p, u64 n)
{
    if (!n || !vmm_is_user_range(p, p + n)) return false;
    u64 lo = p & ~4095ULL;
    u64 hi = (p + n - 1) & ~4095ULL;
    for (u64 pg = lo; pg <= hi; pg += 4096) {
        /* a not-present page is materialized first (see user_page_in) --
         * then, and only then, is it worth asking whether it is writable */
        if (!vmm_translate_in(current->pml4, pg) && !user_page_in(pg, true))
            return false;
        /* T-COW: after fork every writable page is read-only ON BOTH SIDES
         * (PG_W clear, PG_COW set), so the writability check below would
         * reject a perfectly legitimate store into this task's own memory.
         * Resolve the copy first -- page_cow_break is a no-op for a page
         * that is not COW, so text/.rodata keep refusing exactly as they
         * always did.  One rule, at the one gate every kernel store path
         * already funnels through (copy_to_user, the signal frame push in
         * signal.c, the vfs writes): no caller has to remember COW. */
        if (!page_cow_break(current, pg)) return false;
        if (!vmm_writable_in(current->pml4, pg)) return false;
    }
    return true;
}

#define UCP(p) ((p) ? user_ptr_ok((u64)(p), 8) : true)

/* ================= fork ================= */
int sys_fork(void)
{
    struct task *ch = task_alloc_slot();
    if (!ch) return LNXRM_EFAIL;

    ch->kstack = kmalloc(KSTACK_SIZE);
    if (!ch->kstack) {
        task_free_slot(ch);
        return LNXRM_EFAIL;
    }
    ch->kstack_top = ch->kstack + KSTACK_SIZE;
    ch->pid = next_pid();
    strcpy(ch->name, current->name);
    /* T-050: record which *incarnation* of the parent this child was born to,
     * so the link resolves to that task and never to a later squatter that
     * happens to reuse the slot. */
    ch->parent_pid = current->pid;
    ch->parent_gen = current->generation;
    ch->cred = current->cred; /* T-030: fork inherits the parent's tier */
    ch->state = T_EMBRYO;
    ch->pml4 = vmm_new_user_aspace();
    if (!ch->pml4) { /* T-004: no frame for the root -> fork fails, not panics */
        kfree(ch->kstack);
        task_free_slot(ch);
        return LNXRM_EFAIL;
    }

    extern int dup_user_aspace(u64 src, u64 dst);
    if (dup_user_aspace(current->pml4, ch->pml4) < 0) {
        vmm_destroy_user_aspace(ch->pml4);
        kfree(ch->kstack);
        task_free_slot(ch);
        return LNXRM_EFAIL;
    }
    /* T-MMAP: the child claims the same mmap ranges.  Deep copy (one node
     * per range), NOT shared: the list is per-task bookkeeping, and the
     * PAGES behind it are already handled by dup_user_aspace -- shared and
     * write-protected for COW, demand-paged independently for ranges the
     * parent never touched.  A failed copy unwinds exactly like a failed
     * page-table dup: the frames are reference-counted, so destroying the
     * half-built child's tables drops its references and the parent keeps
     * every page it walked in with. */
    if (vma_dup(current, ch) < 0) {
        vmm_destroy_user_aspace(ch->pml4);
        kfree(ch->kstack);
        task_free_slot(ch);
        return LNXRM_EFAIL;
    }
    /* T-FILE: undo the copy-on-write conversion for MAP_SHARED ranges.
     * dup_user_aspace() write-protected every writable page it shared --
     * which is right for anonymous and private memory and wrong for a
     * shared file page, where the whole point is that both sides keep
     * writing ONE frame (and through it, the file).  Runs after both the
     * page-table dup and the VMA dup, because it needs the child's tables
     * to exist and the parent's list to say which ranges are shared.  It
     * cannot fail: it only rewrites flags on frames the dup already
     * reference-counted. */
    vma_fork_shared(current, current->pml4, ch->pml4);

    /* clone fd table */
    memcpy(ch->fds, current->fds, sizeof(ch->fds));
    for (int i = 0; i < NR_FDS; i++)
        if (ch->fds[i]) __sync_fetch_and_add(&ch->fds[i]->refcnt, 1);
    ch->brk_base = current->brk_base;
    ch->brk_cur = current->brk_cur;
    /* T-094: dup_user_aspace copied every stack page, so the child's stack
     * occupies the same window -- inherit the low-watermark too. */
    ch->ustack_lo = current->ustack_lo;
    /* T-032: dup_user_aspace mapped the child's own page TABLES over the
     * parent's frames (shared, COW -- see the copy in vmm.c), so its
     * account is what its own space measures -- a fresh count, not a copy
     * of a number, and from here on the two accounts move independently
     * (the quota bites the child that grows, not the parent).  Its peak
     * starts at what it was born holding. */
    ch->mem_pages = vmm_count_user_pages(ch->pml4);
    ch->mem_peak = ch->mem_pages;
    ch->signal_pending = 0;
    /* Dispositions and the blocked mask are inherited; a freshly forked
     * child has no pending signals of its own.  The saved frame is copied
     * too so that a child forked *from inside a handler* can still
     * sigreturn its way back to the interrupted context. */
    memcpy(ch->sig_handlers, current->sig_handlers, sizeof(ch->sig_handlers));
    memcpy(ch->sig_masks, current->sig_masks, sizeof(ch->sig_masks));
    ch->sig_blocked = current->sig_blocked;
    ch->sig_saved_blocked = current->sig_saved_blocked;
    ch->sig_in_handler = current->sig_in_handler;
    ch->saved_tf = current->saved_tf;
    ch->sig_fault_rip = current->sig_fault_rip;

    /* fabricate the child's interrupt frame */
    struct intr_frame *tf = (struct intr_frame *)(ch->kstack_top - sizeof(struct intr_frame));
    memcpy(tf, current->tf, sizeof(*tf));
    tf->rax = 0;
    tf->rflags |= 0x200;
    ch->tf = tf;

    /* register save area */
    u64 *area = (u64 *)((char *)tf - 7 * 8);
    memset(area, 0, 6 * 8);
    area[6] = (u64)glue_first;
    ch->ctx.sp = (u64)area;

    ch->cpu_id = -1;
    ch->rq_cpu = -1;
    ch->rq_next = NULL;
    ch->state = T_RUNNABLE;
    runqueue_add(ch);
    return ch->pid;
}

/* ================= execve ================= */

/* Map the fresh user stack -- 16 KiB below USER_STACK_TOP -- plus the
 * sigreturn restorer into `pml4`.  Returns 0, or LNXRM_ENOMEM; on failure
 * every frame it did map stays in `pml4`, which is harmless because both
 * callers answer by destroying that address space.  What they free after
 * that differs (image; image + kernel stack + slot), so the unwind stays
 * at each call site. */
static int user_stack_map(u64 pml4)
{
    for (u64 va = USER_STACK_TOP - 0x4000; va < USER_STACK_TOP; va += PAGE_SIZE) {
        u64 pa = pmm_alloc();
        if (!pa) return LNXRM_ENOMEM;
        memset((void *)PHYS_TO_VIRT(pa), 0, PAGE_SIZE);
        if (vmm_map_user(pml4, va, pa, true, true, false) < 0) {
            pmm_free(pa); /* never reached a PTE: hand it straight back */
            return LNXRM_ENOMEM;
        }
    }
    return signal_map_restorer(pml4);
}

/* ---- execve, in pieces ----
 *
 * execve is a pipeline: copy what the caller handed over, read the image,
 * build a new address space, load into it, commit, lay out the stack.  The
 * whole thing used to be one 187-line function whose first half was a
 * ladder of "free the image and close the file" exits -- each stage
 * remembering which of the four resources it had already taken.  Splitting
 * it at the stage boundaries leaves sys_execve() as the ordering, and the
 * one cleanup rule it still needs is "free img, destroy new_pml4", which
 * both live in the caller.
 *
 * POSIX says exec failure RETURNS to the caller: nothing below may exit on
 * a failed stage (this sys_exit(-8)'d once, after CR3 had already been
 * switched, leaving current->pml4 pointing at a destroyed pml4). */
#define EXEC_MAX_ARGS 16
#define EXEC_ARG_LEN  64

struct exec_args {
    char path[128];
    char argv[EXEC_MAX_ARGS][EXEC_ARG_LEN];
    char envp[EXEC_MAX_ARGS][EXEC_ARG_LEN];
    int argc;
    int envc;
};

/* Path, argv and envp strings may straddle unmapped pages: copy them
 * through the per-byte validated helper instead of reading user memory
 * raw.
 *
 * A NULL argv/envp stays legal (userland/sh.c execs with envp NULL); a
 * NON-NULL but unreachable array used to be accepted and dropped -- the
 * new image then ran with argc 0 instead of the caller being told the
 * pointer was bad.  Refuse it, and refuse a bad entry pointer or entry
 * string instead of truncating the array there. (2026-10-20)
 *
 * envp used to be accepted and silently dropped; it is copied for real so
 * the new image's stack carries it (crt0 publishes it as `environ`). */
static long exec_copy_args(struct exec_args *a, const char *upath,
                           char *const uargv[], char *const uenvp[])
{
    if (copy_user_str(a->path, (u64)upath, sizeof(a->path)) < 0)
        return LNXRM_EFAULT;

    if (uargv) {
        if (!UCP((u64)uargv)) return LNXRM_EFAULT;
        for (int i = 0; i < EXEC_MAX_ARGS; i++) {
            const char *s;
            if (copy_from_user(&s, &uargv[i], sizeof(s)) < 0)
                return LNXRM_EFAULT;
            if (!s) break;
            if (copy_user_str(a->argv[i], (u64)s, sizeof(a->argv[i])) < 0)
                return LNXRM_EFAULT;
            a->argc++;
        }
    }

    if (uenvp) {
        if (!UCP((u64)uenvp)) return LNXRM_EFAULT;
        for (int i = 0; i < EXEC_MAX_ARGS; i++) {
            const char *s;
            if (copy_from_user(&s, &uenvp[i], sizeof(s)) < 0)
                return LNXRM_EFAULT;
            if (!s) break;
            if (copy_user_str(a->envp[i], (u64)s, sizeof(a->envp[i])) < 0)
                return LNXRM_EFAULT;
            a->envc++;
        }
    }
    return 0;
}

/* The image, whole, in a buffer the caller frees.  Every failure path
 * here closes what it opened and hands nothing back half-built. */
static long exec_read_image(const char *path, void **img_out, size_t *sz_out)
{
    struct file *f = NULL;
    long err = vfs_open_file(path, O_RDONLY, &f);
    if (err < 0) return err;

    size_t sz = vfs_file_size(f);
    void *img = kmalloc(ALIGN_UP(sz + 1, PAGE_SIZE));
    if (!img) {
        vfs_close_file(f);
        return LNXRM_ENOMEM;
    }
    if (vfs_read_file(f, img, sz) != (long)sz) {
        kfree(img);
        vfs_close_file(f);
        return LNXRM_EIO;
    }
    vfs_close_file(f);
    *img_out = img;
    *sz_out = sz;
    return 0;
}

/* The point of no return, in one place: the new address space is adopted,
 * the old one goes away, and everything that describes "this process" --
 * heap, stack watermark, accounting, open files, name, signal state --
 * starts describing the new image instead.  It runs only after elf_load
 * has succeeded, which is what makes a FAILED execve roll back cleanly
 * above and leave the caller's address space, vma list and signal
 * dispositions untouched. */
static void exec_commit(const struct exec_args *a, u64 old_pml4, u64 new_pml4,
                        u64 brk_end)
{
    /* MAP_SHARED pages the old image dirtied belong to their files, so they
     * are written back while the old root -- and therefore the frames --
     * still exist.  This is deliberately the FIRST thing here: everything
     * below it either destroys that root or drops the VMA list that names
     * the files, and both would take the write-back's evidence with them.
     * It runs only on the success path, so a failed execve leaves the
     * caller's shared pages exactly where they were (still dirty, still
     * the caller's to write back at its own exit). */
    vma_writeback(current, old_pml4, 0, MMAP_TOP);

    /* The point of no return, in two adjacent steps: load the new root,
     * then publish it.  Nothing between them can schedule (this is ring 0
     * and the task is not idle), and switch_to() re-checks the live CR3, so
     * the two can never disagree across a context switch.  CR3 goes first
     * because vmm_destroy_user_aspace(old_pml4) below must never run while
     * the CPU is still paging through the table it is about to free. */
    vmm_switch_to(new_pml4);
    current->pml4 = new_pml4;
    vmm_destroy_user_aspace(old_pml4);

    /* T-MMAP: the new image starts with NO mmap ranges -- the old list
     * described the address space just destroyed, and keeping it would make
     * the demand pager map the new image's address space at the old
     * image's addresses (with the old protection).  Runs after the point of
     * no return for the same reason ustack_lo is set there: a FAILED
     * execve rolls back above and must leave the caller's list intact. */
    vma_free_all(current);
    current->brk_base = (void *)ALIGN_UP(brk_end, PAGE_SIZE);
    current->brk_cur = current->brk_base;

    /* T-094: the new image's stack was mapped 16 KiB below USER_STACK_TOP
     * above; record its low-watermark so a deep recursion can grow it.  Set
     * after the point of no return, so a failed execve (which rolls back to
     * the old aspace) never leaves a stale watermark. */
    current->ustack_lo = USER_STACK_TOP - 0x4000;

    /* T-032: a new image starts a new account (and a new peak).  Counted
     * from the page tables, so it covers the stack loop, the sigreturn
     * restorer and whatever elf_load mapped, however many pages that turns
     * out to be -- nobody has to remember to add them up. */
    current->mem_pages = vmm_count_user_pages(current->pml4);
    current->mem_peak = current->mem_pages;

    /* Close O_CLOEXEC descriptors (the old body was an empty if). */
    for (int i = 0; i < NR_FDS; i++)
        if (current->fds[i] && (current->fds[i]->flags & O_CLOEXEC))
            sys_close(i);

    strncpy(current->name, a->path, TASK_NAME_LEN - 1);

    /* execve replaces the process image, so it also replaces the signal
     * disposition: handlers, masks and any live handler frame go back to
     * the defaults (POSIX). */
    current->signal_pending = 0;
    current->sig_blocked = 0;
    current->sig_saved_blocked = 0;
    current->sig_in_handler = false;
    current->sig_fault_rip = 0;
    memset(current->sig_handlers, 0, sizeof(current->sig_handlers));
    memset(current->sig_masks, 0, sizeof(current->sig_masks));
}

/* User stack per the SysV AMD64 process-entry layout:
 *   [strings]
 *   argc | argv[0..argc-1] | NULL | envp[0..envc-1] | NULL | auxv NULL
 * rsp is 16-byte aligned at entry, as the ABI requires.  Returns the rsp
 * the new image starts from. */
static u64 exec_build_stack(const struct exec_args *a)
{
    u64 sp = USER_STACK_TOP;
    u64 argv_ptrs[EXEC_MAX_ARGS + 1];
    u64 envp_ptrs[EXEC_MAX_ARGS + 1];

    /* Whole argv/envp/auxv build is one SMAP window: every store below goes
     * through a user VA (U/S=1), and this runs after cpu_init() so CR4.SMAP
     * is already on.  No blocking call inside, so the window cannot leak
     * across a schedule(). */
    u64 ac_smap = smap_enter();

    sp -= 64;
    *(u64 *)sp = 0;
    for (int i = a->envc - 1; i >= 0; i--) {
        size_t len = strlen(a->envp[i]) + 1;
        sp -= len;
        memcpy((void *)sp, a->envp[i], len);
        envp_ptrs[i] = sp;
    }
    for (int i = a->argc - 1; i >= 0; i--) {
        size_t len = strlen(a->argv[i]) + 1;
        sp -= len;
        memcpy((void *)sp, a->argv[i], len);
        argv_ptrs[i] = sp;
    }

    /* argc + argv + NULL + envp + NULL + two AT_NULL words */
    u64 slots = 1 + (u64)a->argc + 1 + (u64)a->envc + 1 + 2;
    u64 bytes = slots * 8;
    u64 pad = (16 - (bytes & 15)) & 15;

    sp &= ~15UL;
    sp -= bytes + pad;
    u64 *p = (u64 *)sp;
    *p++ = (u64)a->argc;
    for (int i = 0; i < a->argc; i++) *p++ = argv_ptrs[i];
    *p++ = 0;
    for (int i = 0; i < a->envc; i++) *p++ = envp_ptrs[i];
    *p++ = 0;
    *p++ = 0; /* auxv AT_NULL tag */
    *p++ = 0;

    smap_leave(ac_smap);
    return sp;
}

long sys_execve(const char *upath, char *const uargv[], char *const uenvp[])
{
    struct exec_args a;
    memset(&a, 0, sizeof a);
    long err = exec_copy_args(&a, upath, uargv, uenvp);
    if (err < 0) return err;

    void *img = NULL;
    size_t sz = 0;
    if ((err = exec_read_image(a.path, &img, &sz)) < 0) return err;

    u64 old_pml4 = current->pml4;
    u64 new_pml4 = vmm_new_user_aspace();
    if (!new_pml4) {
        kfree(img);
        return LNXRM_ENOMEM;
    }

    /* map a fresh user stack (16 KiB) below USER_STACK_TOP */
    if (user_stack_map(new_pml4) < 0) {
        kfree(img); /* was leaked */
        vmm_destroy_user_aspace(new_pml4);
        return LNXRM_ENOMEM;
    }

    u64 entry = 0, brk_end = 0;
    /* Deliberately NO vmm_switch_to() here: elf_load() maps and fills every
     * frame through the high-half alias and an explicit `pml4` root, so it
     * works under ANY address space (see elf.c).  Switching CR3 this early
     * opened a ~15-line window in which CR3 was the new root while
     * current->pml4 still named the old one -- the exact state the #PF
     * report calls (*MISMATCH*).  The switch now happens at the point of
     * no return, two instructions before current->pml4 is published. */
    entry = elf_load(new_pml4, img, sz, &brk_end);
    kfree(img);
    if (!entry) {
        /* ELF load failed: the caller's address space was never left, so
         * there is nothing to switch back to -- POSIX says exec failure
         * returns to the caller. */
        vmm_destroy_user_aspace(new_pml4);
        return LNXRM_ENOEXEC;
    }

    exec_commit(&a, old_pml4, new_pml4, brk_end);
    u64 sp = exec_build_stack(&a);

    struct intr_frame *f2 = current->tf;
    f2->rip = entry;
    f2->ussp = sp;
    f2->cs = 0x18 | 3;
    f2->usss = 0x20 | 3;
    f2->rflags = 0x202;
    return 0;
}

/* ================= exit / wait ================= */
void sys_exit(int code)
{
    /* Drop the single console-read waiter slot if it still points at us.
     * Without this a killed `cat` leaves con_waiter aimed at a freed
     * task slot, and a later reuse of that slot lets input_push()
     * runqueue_add() a task that was never sleeping. */
    console_waiter_disarm(current);

    /* T-033: hand the surface back along with everything else.  A
     * holder that outlived itself would leave the screen owned by a pid
     * that no longer exists: nobody could ever draw again, and the next
     * drawing program would be refused with nothing to appeal to.  This
     * is the single release point -- the syscall itself, SIGKILL, a
     * terminating signal and a fault with no handler all end in
     * sys_exit, and pids are handed out by a counter that never reuses
     * them. */
    fb_disown(current->pid);

    /* MAP_SHARED pages this process dirtied are the FILE's bytes: put them
     * back before anything they need can go away.  The order below is what
     * makes that possible -- the file descriptors are closed next (a last
     * close drops the cached shared frames), and the address space itself
     * is only destroyed when this zombie is reaped, so writing back HERE
     * is the only point where both the frames and the open file are still
     * reachable.  Every other way a process dies -- a signal, a fault with
     * no handler, SIGKILL -- ends in this function, so this one line covers
     * them all. */
    vma_writeback(current, current->pml4, 0, MMAP_TOP);

    for (int i = 0; i < NR_FDS; i++)
        if (current->fds[i]) sys_close(i);

    /* Reparent children to init (pid 1).  A dying init cannot adopt
     * itself and a zombie cannot raise children either: in both cases the
     * link becomes the no-adopter marker (0,0), which task_alloc_slot
     * reclaims on demand (P0-2). */
    struct task *init_t = find_task(1);
    if (!init_t || init_t == current || init_t->state == T_ZOMBIE) init_t = NULL;
    int i = 0;
    for (struct task *t = task_iter(&i); t; t = task_iter(&i))
        if (t != current && t->parent_pid == current->pid &&
            t->parent_gen == current->generation) {
            t->parent_pid = init_t ? init_t->pid : 0;
            t->parent_gen = init_t ? init_t->generation : 0;
        }

    /* send SIGCHLD to parent.  Resolve it once so the signal and the wake
     * below act on the same task even if the slot moves in between. */
    struct task *pp = task_parent(current);
    if (pp) send_signal(pp, SIGCHLD);

    current->exit_code = code;
    current->state = T_ZOMBIE;
    runqueue_remove(current);
    if (pp && pp->state == T_SLEEPING && pp->pid != 0) {
        pp->state = T_RUNNABLE;
        runqueue_add(pp);
    }
    schedule();
    for (;;) __asm__ volatile("hlt");
}

int sys_waitpid(int wpid, int *ustatus, int opts)
{
    /* Refuse a bad status pointer BEFORE anything is reaped.  The old
     * code guarded the copy with UCP() and skipped it silently on
     * failure: the zombie was consumed, the pid came back as success,
     * and the caller's untouched status word read as a garbage exit
     * code.  One threaded task owns its address space, so once this
     * check passes nothing can unmap before the copy below. (2026-10-20) */
    if (ustatus && !user_ptr_ok((u64)ustatus, sizeof(int))) return LNXRM_EFAULT;
    for (;;) {
        bool have_kids = false;
        int i = 0;
        for (struct task *t = task_iter(&i); t; t = task_iter(&i)) {
            /* T-050: "is t my child" is an incarnation match, not a pointer
             * compare -- a recycled slot must not look like my offspring. */
            if (t == current || t->parent_pid != current->pid ||
                t->parent_gen != current->generation) continue;
            have_kids = true;
            if (wpid > 0 && t->pid != (u32)wpid) continue;
            if (t->state == T_ZOMBIE) {
                int code = t->exit_code;
                if (ustatus) copy_to_user(ustatus, &code, sizeof(code));
                u32 pid = t->pid;
                vmm_destroy_user_aspace(t->pml4);
                kfree(t->kstack);
                t->kstack = NULL;
                task_free_slot(t);
                return pid;
            }
        }
        if (!have_kids) return LNXRM_ECHILD;
        if (opts & 1) return 0;
        current->state = T_SLEEPING;
        current->sleep_until = ~0ULL;
        runqueue_remove(current);
        schedule();
    }
}

/* ================= user<->kernel copies ================= */
int copy_user_str(char *kdst, u64 usrc, size_t max)
{
    if (!max) return LNXRM_EFAIL;
    /* One SMAP window around the whole scan (the inner copy_from_user
     * windows nest: each saves AC=1 and restores it).  Saves a
     * pushfq/stac/popfq per byte on the common path. */
    u64 ac = smap_enter();
    int rc = 0;
    bool done = false;
    for (size_t i = 0; i < max; i++) {
        char c;
        if (copy_from_user(&c, (const void *)(usrc + i), 1) < 0) {
            kdst[0] = 0;
            rc = LNXRM_EFAIL;
            break;
        }
        kdst[i] = c;
        if (!c) {
            done = true;
            break;
        }
    }
    if (rc == 0 && !done) kdst[max - 1] = 0;
    smap_leave(ac);
    return rc;
}

int copy_from_user(void *kdst, const void *usrc, size_t n)
{
    if (!user_ptr_ok((u64)usrc, n)) return LNXRM_EFAIL;
    /* SMAP: a ring-0 load from a U/S=1 page needs AC=1 (STAC).  Without
     * the window every copy would #PF the moment cpu_protect_init ran. */
    u64 ac = smap_enter();
    memcpy(kdst, usrc, n);
    smap_leave(ac);
    return 0;
}

int copy_to_user(void *udst, const void *ksrc, size_t n)
{
    /* Every destination page must be mapped WRITABLE: with CR0.WP on a
     * ring-0 store to a read-only user page faults, and there is no #PF
     * fixup table to recover from it.  Refusing here matches POSIX (a
     * read() into a text/.rodata page answers -EFAULT) and, unlike the
     * pre-WP behaviour, no longer overwrites the process's read-only
     * segments behind its back.  SMAP then needs the AC window for the
     * store itself (a *load* likewise for copy_from_user above). */
    if (!user_ptr_writable((u64)udst, n)) return LNXRM_EFAIL;
    u64 ac = smap_enter();
    memcpy(udst, ksrc, n);
    smap_leave(ac);
    return 0;
}

/* ================= first user process ================= */
int kernel_spawn(const char *path)
{
    struct task *t = task_alloc_slot();
    if (!t) return LNXRM_EFAIL;

    t->kstack = kmalloc(KSTACK_SIZE);
    if (!t->kstack) {
        task_free_slot(t);
        return LNXRM_EFAIL;
    }
    t->kstack_top = t->kstack + KSTACK_SIZE;
    t->pid = next_pid();
    strncpy(t->name, path, TASK_NAME_LEN - 1);
    /* T-030: what kernel_spawn starts (init) runs as root. */
    t->cred.kind = CRED_ROOT;
    t->cred.uid = CRED_UID_PRIV;
    t->state = T_EMBRYO;
    t->pml4 = vmm_new_user_aspace();
    if (!t->pml4) {
        kfree(t->kstack);
        task_free_slot(t);
        return LNXRM_ENOMEM;
    }

    struct file *f = NULL;
    if (vfs_open_file(path, O_RDONLY, &f) < 0) {
        kprintf("[init] cannot open %s\n", path);
        vmm_destroy_user_aspace(t->pml4);
        kfree(t->kstack);
        task_free_slot(t);
        return LNXRM_ENOENT;
    }
    size_t sz = vfs_file_size(f);
    void *img = kmalloc(ALIGN_UP(sz + 1, PAGE_SIZE));
    if (!img) {
        vfs_close_file(f);
        vmm_destroy_user_aspace(t->pml4);
        kfree(t->kstack);
        task_free_slot(t);
        return LNXRM_ENOMEM;
    }
    if (vfs_read_file(f, img, sz) != (long)sz) {
        kfree(img);
        vfs_close_file(f);
        vmm_destroy_user_aspace(t->pml4);
        kfree(t->kstack);
        task_free_slot(t);
        return LNXRM_EIO;
    }
    vfs_close_file(f);

    if (user_stack_map(t->pml4) < 0) {
        kfree(img);
        vmm_destroy_user_aspace(t->pml4);
        kfree(t->kstack);
        task_free_slot(t);
        return LNXRM_ENOMEM;
    }

    /* elf_load() fills frames through the high-half alias and an explicit
     * `t->pml4` root, so CR3 need not point there yet -- and must not: this
     * runs as the idle task, whose own root is master_pml4_phys(), so
     * switching here would hold CR3 != current->pml4 across the whole load.
     * The switch moves down to the first direct user-VA store. */
    u64 entry = elf_load(t->pml4, img, sz, (u64 *)&t->brk_base);
    kfree(img);
    if (!entry) {
        vmm_destroy_user_aspace(t->pml4);
        kfree(t->kstack);
        task_free_slot(t);
        return LNXRM_ENOEXEC;
    }

    /* Initial user stack: path string + argc/argv/NULL words.  Direct
     * user-VA stores, so they need their own SMAP window (this is the
     * kernel_spawn twin of sys_execve's argv/auxv build below) -- and CR3
     * has to name the new address space while they run. */
    vmm_switch_to(t->pml4);
    u64 sp = USER_STACK_TOP - 256;
    u64 ac_sp = smap_enter();
    strcpy((char *)sp, path);
    u64 argp[2] = {sp, 0};
    u64 ap = sp - 32;
    ((u64 *)ap)[0] = 1;
    ((u64 *)ap)[1] = argp[0];
    ((u64 *)ap)[2] = 0;
    ((u64 *)ap)[3] = 0;
    smap_leave(ac_sp);

    vmm_switch_to(master_pml4_phys());
    t->brk_base = (void *)ALIGN_UP((uptr)t->brk_base, PAGE_SIZE);
    t->brk_cur = t->brk_base;
    /* T-094: init's stack is the same 16 KiB window; record its low-watermark. */
    t->ustack_lo = USER_STACK_TOP - 0x4000;
    /* T-032: the spawn twin of sys_execve's refresh -- stack, restorer and
     * image, counted from the tables rather than added up by hand. */
    t->mem_pages = vmm_count_user_pages(t->pml4);
    t->mem_peak = t->mem_pages;

    struct intr_frame *tf = (struct intr_frame *)(t->kstack_top - sizeof(struct intr_frame));
    memset(tf, 0, sizeof(*tf));
    tf->rip = entry;
    tf->ussp = ap;
    tf->cs = 0x18 | 3;
    tf->usss = 0x20 | 3;
    tf->rflags = 0x202;
    t->tf = tf;

    u64 *area = (u64 *)((char *)tf - 7 * 8);
    memset(area, 0, 6 * 8);
    area[6] = (u64)glue_first;
    t->ctx.sp = (u64)area;

    t->cpu_id = -1;
    t->rq_cpu = -1;
    t->rq_next = NULL;
    t->state = T_RUNNABLE;
    runqueue_add(t);
    kprintf("[task] spawned pid=%u (%s) entry=%#lx sp=%#lx\n", t->pid, path, entry, ap);
    return t->pid;
}

/* Snapshot the task table into a caller-owned buffer.  `max` caps how many
 * records are written; the return value is the number written (0 is a
 * legitimate answer for an empty table), never a fake success. */
long sys_ps(struct lnxrm_ps_entry *ubuf, int max)
{
    int i = 0;
    int n = 0;

    if (max < 0) return LNXRM_EINVAL;
    if (max > NR_TASKS) max = NR_TASKS;
    if (max && !ubuf) return LNXRM_EFAULT;

    for (struct task *t = task_iter(&i); t && n < max; t = task_iter(&i)) {
        struct lnxrm_ps_entry e;

        if (t->state == T_UNUSED || t->state == T_EMBRYO) continue;
        memset(&e, 0, sizeof(e));
        e.pid = (int32_t)t->pid;
        e.ppid = (int32_t)t->parent_pid; /* T-050: pid half of the link */
        e.cpu = (int32_t)t->cpu_id;
        e.state = (int32_t)t->state;
        e.kind = (int32_t)t->cred.kind; /* T-030 */
        e.uid = (int32_t)t->cred.uid;
        /* T-032: what this task's user space costs.  Read without the task
         * lock -- the counters are plain u64s, so a snapshot taken while
         * the owner is mid-brk is at worst one page out of date. */
        e.mem_kib = (int32_t)(t->mem_pages * (PAGE_SIZE / 1024));
        e.peak_kib = (int32_t)(t->mem_peak * (PAGE_SIZE / 1024));
        strncpy(e.name, t->name, sizeof(e.name) - 1);
        if (copy_to_user(ubuf + n, &e, sizeof(e)) < 0) return LNXRM_EFAULT;
        n++;
    }
    return n;
}
