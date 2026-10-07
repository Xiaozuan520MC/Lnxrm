/* Process management: pid allocation, fork/execve/exit/waitpid, the user
 * memory access helpers and /bin/init spawning. */
#include <sys/sched.h>
#include <io.h>
#include <sys/vfs.h>
#include <console.h>
#include <framebuffer.h>
#include <mm/mm.h>
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

bool user_ptr_ok(u64 p, u64 n)
{
    if (!n || !vmm_is_user_range(p, p + n)) return false;
    u64 lo = p & ~4095ULL;
    u64 hi = (p + n - 1) & ~4095ULL;
    for (u64 pg = lo; pg <= hi; pg += 4096)
        if (!vmm_translate_in(current->pml4, pg)) return false;
    return true;
}

bool user_ptr_writable(u64 p, u64 n)
{
    if (!n || !vmm_is_user_range(p, p + n)) return false;
    u64 lo = p & ~4095ULL;
    u64 hi = (p + n - 1) & ~4095ULL;
    for (u64 pg = lo; pg <= hi; pg += 4096)
        if (!vmm_writable_in(current->pml4, pg)) return false;
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
    ch->parent = current;
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

    /* clone fd table */
    memcpy(ch->fds, current->fds, sizeof(ch->fds));
    for (int i = 0; i < NR_FDS; i++)
        if (ch->fds[i]) __sync_fetch_and_add(&ch->fds[i]->refcnt, 1);
    ch->brk_base = current->brk_base;
    ch->brk_cur = current->brk_cur;
    /* T-032: dup_user_aspace gave the child a frame of its own for every
     * user page, so its account is what its own space measures -- a fresh
     * count, not a copy of a number, and from here on the two accounts
     * move independently (the quota bites the child that grows, not the
     * parent).  Its peak starts at what it was born holding. */
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
long sys_execve(const char *upath, char *const uargv[], char *const uenvp[])
{
    char path[128];
    char argv[16][64];
    char envp[16][64];
    int argc = 0;
    int envc = 0;

    /* Path, argv and envp strings may straddle unmapped pages: copy them
     * through the per-byte validated helper instead of reading user
     * memory raw. */
    if (copy_user_str(path, (u64)upath, sizeof(path)) < 0) return LNXRM_EFAULT;

    if (uargv && UCP((u64)uargv)) {
        for (int i = 0; i < 16; i++) {
            const char *a;
            if (copy_from_user(&a, &uargv[i], sizeof(a)) < 0 || !a) break;
            if (copy_user_str(argv[i], (u64)a, sizeof(argv[i])) < 0) break;
            argc++;
        }
    }

    /* envp used to be accepted and silently dropped; copy it for real so
     * the new image's stack carries it (crt0 publishes it as `environ`). */
    if (uenvp && UCP((u64)uenvp)) {
        for (int i = 0; i < 16; i++) {
            const char *e;
            if (copy_from_user(&e, &uenvp[i], sizeof(e)) < 0 || !e) break;
            if (copy_user_str(envp[i], (u64)e, sizeof(envp[i])) < 0) break;
            envc++;
        }
    }

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

    u64 old_pml4 = current->pml4;
    u64 new_pml4 = vmm_new_user_aspace();
    if (!new_pml4) {
        kfree(img);
        return LNXRM_ENOMEM;
    }

    /* map a fresh user stack (16 KiB) below USER_STACK_TOP */
    for (u64 va = USER_STACK_TOP - 0x4000; va < USER_STACK_TOP; va += PAGE_SIZE) {
        u64 pa = pmm_alloc();
        if (!pa) {
            kfree(img); /* was leaked */
            vmm_destroy_user_aspace(new_pml4);
            return LNXRM_ENOMEM;
        }
        memset((void *)PHYS_TO_VIRT(pa), 0, PAGE_SIZE);
        if (vmm_map_user(new_pml4, va, pa, true, true, false) < 0) {
            pmm_free(pa); /* nothing is reachable yet, so unwind it all */
            kfree(img);
            vmm_destroy_user_aspace(new_pml4);
            return LNXRM_ENOMEM;
        }
    }
    if (signal_map_restorer(new_pml4) < 0) {
        kfree(img);
        vmm_destroy_user_aspace(new_pml4);
        return LNXRM_ENOMEM;
    }

    u64 entry = 0, brk_end = 0;
    vmm_switch_to(new_pml4);
    entry = elf_load(new_pml4, img, sz, &brk_end);
    kfree(img);
    if (!entry) {
        /* ELF load failed: roll back to the caller's original address
         * space.  Previously this sys_exit(-8)'d after already switching
         * CR3, leaving current->pml4 pointing at a destroyed (or never
         * assigned) pml4 and leaking new_pml4. POSIX says exec failure
         * returns to the caller. */
        vmm_switch_to(old_pml4);
        vmm_destroy_user_aspace(new_pml4);
        return LNXRM_ENOEXEC;
    }

    /* Assign the new address space BEFORE destroying the old one so
     * current->pml4 / any interrupt path never sees a stale CR3. */
    current->pml4 = new_pml4;
    vmm_destroy_user_aspace(old_pml4);
    current->brk_base = (void *)ALIGN_UP(brk_end, PAGE_SIZE);
    current->brk_cur = current->brk_base;

    /* T-032: a new image starts a new account (and a new peak).  Counted
     * from the page tables, so it covers the stack loop, the sigreturn
     * restorer and whatever elf_load mapped, however many pages that
     * turns out to be -- nobody has to remember to add them up. */
    current->mem_pages = vmm_count_user_pages(current->pml4);
    current->mem_peak = current->mem_pages;

    /* Close O_CLOEXEC descriptors (the old body was an empty if). */
    for (int i = 0; i < NR_FDS; i++)
        if (current->fds[i] && (current->fds[i]->flags & O_CLOEXEC)) sys_close(i);

    strncpy(current->name, path, TASK_NAME_LEN - 1);

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

    /* User stack per the SysV AMD64 process-entry layout:
     *   [strings]
     *   argc | argv[0..argc-1] | NULL | envp[0..envc-1] | NULL | auxv NULL
     * rsp is 16-byte aligned at entry, as the ABI requires. */
    u64 sp = USER_STACK_TOP;
    u64 argv_ptrs[17];
    u64 envp_ptrs[17];

    /* Whole argv/envp/auxv build is one SMAP window: every store below
     * goes through a user VA (U/S=1), and this runs after cpu_init() so
     * CR4.SMAP is already on.  No blocking call inside, so the window
     * cannot leak across a schedule(). */
    u64 ac_smap = smap_enter();
    sp -= 64;
    *(u64 *)sp = 0;
    for (int i = envc - 1; i >= 0; i--) {
        size_t len = strlen(envp[i]) + 1;
        sp -= len;
        memcpy((void *)sp, envp[i], len);
        envp_ptrs[i] = sp;
    }
    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(argv[i]) + 1;
        sp -= len;
        memcpy((void *)sp, argv[i], len);
        argv_ptrs[i] = sp;
    }

    {
        /* argc + argv + NULL + envp + NULL + two AT_NULL words */
        u64 slots = 1 + (u64)argc + 1 + (u64)envc + 1 + 2;
        u64 bytes = slots * 8;
        u64 pad = (16 - (bytes & 15)) & 15;
        u64 *p;

        sp &= ~15UL;
        sp -= bytes + pad;
        p = (u64 *)sp;
        *p++ = (u64)argc;
        for (int i = 0; i < argc; i++) *p++ = argv_ptrs[i];
        *p++ = 0;
        for (int i = 0; i < envc; i++) *p++ = envp_ptrs[i];
        *p++ = 0;
        *p++ = 0; /* auxv AT_NULL tag */
        *p++ = 0;
    }
    smap_leave(ac_smap);

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

    for (int i = 0; i < NR_FDS; i++)
        if (current->fds[i]) sys_close(i);

    /* Reparent orphans to init (pid 1).  The old `init_t ? init_t :`
     * was dead code — init_t was always NULL. */
    struct task *init_t = find_task(1);
    int i = 0;
    for (struct task *t = task_iter(&i); t; t = task_iter(&i))
        if (t != current && t->parent == current) t->parent = init_t;

    /* send SIGCHLD to parent */
    if (current->parent) send_signal(current->parent, SIGCHLD);

    current->exit_code = code;
    current->state = T_ZOMBIE;
    runqueue_remove(current);
    if (current->parent && current->parent->state == T_SLEEPING && current->parent->pid != 0) {
        current->parent->state = T_RUNNABLE;
        runqueue_add(current->parent);
    }
    schedule();
    for (;;) __asm__ volatile("hlt");
}

int sys_waitpid(int wpid, int *ustatus, int opts)
{
    for (;;) {
        bool have_kids = false;
        int i = 0;
        for (struct task *t = task_iter(&i); t; t = task_iter(&i)) {
            if (t == current || t->parent != current) continue;
            have_kids = true;
            if (wpid > 0 && t->pid != (u32)wpid) continue;
            if (t->state == T_ZOMBIE) {
                int code = t->exit_code;
                if (ustatus && UCP(ustatus)) copy_to_user(ustatus, &code, sizeof(code));
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

    for (u64 va = USER_STACK_TOP - 0x4000; va < USER_STACK_TOP; va += PAGE_SIZE) {
        u64 pa = pmm_alloc();
        if (!pa) {
            kfree(img);
            vmm_destroy_user_aspace(t->pml4);
            kfree(t->kstack);
            task_free_slot(t);
            return LNXRM_ENOMEM;
        }
        memset((void *)PHYS_TO_VIRT(pa), 0, PAGE_SIZE);
        if (vmm_map_user(t->pml4, va, pa, true, true, false) < 0) {
            pmm_free(pa);
            kfree(img);
            vmm_destroy_user_aspace(t->pml4);
            kfree(t->kstack);
            task_free_slot(t);
            return LNXRM_ENOMEM;
        }
    }
    if (signal_map_restorer(t->pml4) < 0) {
        kfree(img);
        vmm_destroy_user_aspace(t->pml4);
        kfree(t->kstack);
        task_free_slot(t);
        return LNXRM_ENOMEM;
    }

    vmm_switch_to(t->pml4);
    u64 entry = elf_load(t->pml4, img, sz, (u64 *)&t->brk_base);
    kfree(img);
    if (!entry) {
        vmm_switch_to(master_pml4_phys());
        vmm_destroy_user_aspace(t->pml4);
        kfree(t->kstack);
        task_free_slot(t);
        return LNXRM_ENOEXEC;
    }

    /* Initial user stack: path string + argc/argv/NULL words.  Direct
     * user-VA stores, so they need their own SMAP window (this is the
     * kernel_spawn twin of sys_execve's argv/auxv build below). */
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
        e.ppid = t->parent ? (int32_t)t->parent->pid : 0;
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
