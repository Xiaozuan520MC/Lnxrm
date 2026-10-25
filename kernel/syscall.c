/* System call dispatch (int 0x80, nr in rax, args in rdi rsi r10). */
#include <sys/vfs.h>
#include <sys/sched.h>
#include <sys/cpu.h>
#include <sys/protect.h>
#include <console.h>
#include <mm/mm.h>
#include <framebuffer.h>

long sys_getdent(int fd, void *ubuf, size_t len);
long sys_lseek(int fd, long off, int whence);
int blk_list_all(void *ubuf, int max);

static long sys_brk(u64 newbrk)
{
    if (!newbrk) return (long)current->brk_cur;
    if ((u64)newbrk < (u64)current->brk_base || newbrk > USER_STACK_TOP - (1 << 20))
        return LNXRM_ENOMEM;
    u64 old = ALIGN_UP((u64)current->brk_cur, PAGE_SIZE);
    u64 tgt = ALIGN_UP(newbrk, PAGE_SIZE);
    /* T-032: what the process costs today, counted rather than assumed --
     * the same walk ps uses, so the quota and the report cannot be two
     * different numbers.  Taken once, before the space changes, it also
     * gives the exact post-brk total (delta known, rest unchanged). */
    u64 have = vmm_count_user_pages(current->pml4);
    if (tgt > old) {
        /* T-032: refuse BEFORE taking a frame.  A plain user may hold
         * CRED_USER_MEM_QUOTA_PAGES of address space in total -- image,
         * stack, restorer and heap -- and that ceiling is the only thing
         * standing between one process and a machine with no reclaim and
         * no OOM killer.  Shrinking is never refused (below). */
        u64 quota = cred_mem_quota(&current->cred);
        if (quota && have + (tgt - old) / PAGE_SIZE > quota) return LNXRM_ENOMEM;

        /* T-004: growth can run out of frames *or* out of the page tables
         * that reach `va`.  Either way brk_cur has not moved yet, so the
         * pages mapped so far would be invisible to a later shrink (which
         * only walks [brk_cur, old)) -- unmap them again before reporting
         * ENOMEM, or every failed attempt leaks the whole partial growth. */
        u64 mapped = old;
        for (u64 va = old; va < tgt; va += PAGE_SIZE) {
            u64 pa = pmm_alloc();
            if (!pa) break;
            memset((void *)PHYS_TO_VIRT(pa), 0, PAGE_SIZE);
            if (vmm_map_user(current->pml4, va, pa, true, true, false) < 0) {
                pmm_free(pa);
                break;
            }
            mapped = va + PAGE_SIZE;
        }
        if (mapped != tgt) {
            for (u64 va = old; va < mapped; va += PAGE_SIZE) {
                u64 pa = vmm_unmap_user(current->pml4, va);
                if (pa) pmm_free(pa);
            }
            return LNXRM_ENOMEM;
        }
        current->mem_pages = have + (tgt - old) / PAGE_SIZE;
    } else {
        for (u64 va = tgt; va < old; va += PAGE_SIZE) {
            u64 pa = vmm_unmap_user(current->pml4, va);
            if (pa) pmm_free(pa);
        }
        current->mem_pages = have - (old - tgt) / PAGE_SIZE;
    }
    current->brk_cur = (void *)newbrk;
    if (current->mem_pages > current->mem_peak) current->mem_peak = current->mem_pages;
    return (long)current->brk_cur;
}

/* ---- kill(pid, sig) ---- */
/* T-031: may `current` signal `t`?  Two gates, in order:
 *  1. kxld protection: pid 1 and kxld tasks are untouchable -- EACCES
 *     for everyone, root included;
 *  2. the regular gate: same uid, or CAP_KILL.
 * A broadcast (pid == -1) never reaches this function: sys_kill refuses
 * it up front unless the caller holds CAP_KILL. */
static long kill_allowed(const struct task *t)
{
    if (kxld_protect_kill(t)) {
        /* a denial receipt, like the CR0.WP / SMEP / SMAP ones: without
         * it "kill 1 did nothing" is indistinguishable from "kill is
         * broken". */
        kprintf("[kxld] denied kill: pid %u -> pid %u\n",
                (unsigned)current->pid, (unsigned)t->pid);
        return LNXRM_EACCES;
    }
    if (t->cred.uid == current->cred.uid) return 0;
    if (cred_capable(&current->cred, CAP_KILL)) return 0;
    kprintf("[kill] denied: pid %u (uid %u) -> pid %u (uid %u)\n",
            (unsigned)current->pid, (unsigned)current->cred.uid,
            (unsigned)t->pid, (unsigned)t->cred.uid);
    return LNXRM_EACCES;
}

static long sys_kill(int pid, int sig)
{
    if (sig < 0 || sig >= _NSIG) return LNXRM_EINVAL;

    /* T-031: `kill(-1, ...)` is a broadcast, so it is a CAP_KILL
     * operation in its own right.  Without this a plain user's broadcast
     * is either a silent no-op (every target refused) or a way to
     * enumerate who else it may signal; the sig==0 probe asks the same
     * permission question and gets the same answer. */
    long broadcast = 0;
    if (pid == -1 && !cred_capable(&current->cred, CAP_KILL)) {
        kprintf("[cap] denied kill(-1): pid %u (uid %u) lacks CAP_KILL\n",
                (unsigned)current->pid, (unsigned)current->cred.uid);
        broadcast = LNXRM_EACCES;
    }

    /* kill(pid, 0) is the POSIX existence/permission probe: it never
     * delivers anything, so it must not fall through to send_signal()
     * (which silently drops sig == 0 and would report a fake success). */
    if (sig == 0) {
        if (pid > 0) {
            struct task *t = find_task(pid);
            if (!t) return LNXRM_ESRCH;
            return kill_allowed(t); /* T-031: probe checks permission too */
        }
        if (pid == -1) return broadcast; /* T-031: broadcast probe gated too */
        return LNXRM_EINVAL;
    }

    if (pid > 0) {
        /* send to specific process */
        struct task *t = find_task(pid);
        if (!t) return LNXRM_ESRCH; /* ESRCH */
        long err = kill_allowed(t);
        if (err) return err;
        send_signal(t, sig);
        return 0;
    } else if (pid == -1) {
        if (broadcast) return broadcast; /* T-031: no CAP_KILL, no broadcast */
        /* send to all processes we may signal: idles and half-built
         * forks drop out on pid==0, kxld/init targets on kill_allowed */
        int i = 0;
        for (struct task *t = task_iter(&i); t; t = task_iter(&i)) {
            if (t->pid == 0) continue;
            if (kill_allowed(t)) continue;
            send_signal(t, sig);
        }
        return 0;
    }
    return LNXRM_EINVAL;
}

/* ---- capability gates (T-031) ----
 * One receipt per refusal, in the shape sys_kill already prints: which
 * call said no, who asked, which capability was missing.  Silence here
 * would make "ps printed nothing" indistinguishable from "ps is broken". */
static long cap_gate(u64 cap, const char *capname, const char *what)
{
    if (cred_capable(&current->cred, cap)) return 0;
    kprintf("[cap] denied %s: pid %u (uid %u) lacks %s\n", what,
            (unsigned)current->pid, (unsigned)current->cred.uid, capname);
    return LNXRM_EACCES;
}

/* ---- the owner gate (T-033) ----
 * cap_gate answers "may this process draw at all".  Two processes can
 * both answer yes and still not both be talking to the same screen;
 * this gate answers that second question -- the first drawing call
 * claims the surface for its pid, every later drawer is turned away
 * until the holder exits (kernel/task.c -> fb_disown).
 *
 * Order is fixed by the caller and has to stay capability-first: a
 * plain user must keep seeing -13 for "you have no CAP_FB", which is
 * the whole explanation for its refusal, instead of being told it is
 * not the owner of something it was never allowed to touch in the
 * first place.  sys_fb_info is deliberately gated by neither: it reads
 * geometry and draws nothing, and a process that cannot learn the
 * screen size has no way to decide where it may draw once it can.
 *
 * A refusal never takes the surface.  Otherwise the process that was
 * merely turned away would end up holding it, and the one actually
 * drawing would be the one refused on its very next call. */
static long fb_owner_gate(const char *what)
{
    if (fb_claim(current->pid)) return 0;
    kprintf("[fb] denied %s: pid %u holds the surface, pid %u draws\n", what,
            (unsigned)fb_owner_get(), (unsigned)current->pid);
    return LNXRM_EACCES;
}

/* ---- setuid(uid): the drop that makes the gates above testable ----
 * Every task starts life root (kernel_spawn, then fork inheritance), so
 * without a way down there would be no process on the system any of
 * these gates could refuse.  cred_setuid() decides what the call means;
 * the kernel side only reports a refusal. */
static long sys_setuid(u32 uid)
{
    long err = cred_setuid(&current->cred, uid);
    if (err) {
        kprintf("[cap] denied setuid: pid %u (uid %u) -> uid %u\n",
                (unsigned)current->pid, (unsigned)current->cred.uid, (unsigned)uid);
        return err;
    }
    return 0;
}

/* ---- uname() ---- */
long sys_uname(struct lnxrm_utsname *u)
{
    struct lnxrm_utsname k;

    if (!u) return LNXRM_EFAULT;
    memset(&k, 0, sizeof(k));
    strncpy(k.sysname, "lnxrm", sizeof(k.sysname) - 1);
    strncpy(k.nodename, "lnxrm", sizeof(k.nodename) - 1);
    strncpy(k.release, "v0.09", sizeof(k.release) - 1);
    strncpy(k.version, "Lnxrm v0.09 " __DATE__ " " __TIME__, sizeof(k.version) - 1);
    strncpy(k.machine, "x86-64", sizeof(k.machine) - 1);
    if (copy_to_user(u, &k, sizeof(k)) < 0) return LNXRM_EFAULT;
    return 0;
}

/* ---- framebuffer syscalls (23-27, plus fb_flip 32) ----
 * Colour fields are true-colour 0x00RRGGBB (see lnxrm_abi.h), never
 * palette indices.  Every primitive clips to the visible area, so a bad
 * rectangle from user space can at worst draw nothing.  The drawing calls
 * land in the offscreen back surface; fb_flip (32) publishes it.
 *
 * T-031: the five surface calls need CAP_FB -- without it any process
 * could repaint the console the shell is writing to.  fb_info stays
 * open: it reads geometry (width/height/pitch) and draws nothing, and a
 * program that cannot even learn the screen size has no way to decide
 * where it may draw once it does hold CAP_FB. */
static long sys_fb_info(struct lnxrm_fb_info *uinfo)
{
    struct lnxrm_fb_info info;
    info.width = fb_width;
    info.height = fb_height;
    info.bpp = fb_bpp;
    info.pitch = fb_pitch;
    if (copy_to_user(uinfo, &info, sizeof(info)) < 0) return LNXRM_EFAULT;
    return 0;
}

static long sys_fb_clear(u32 color)
{
    fb_clear(color);
    return 0;
}

static long sys_fb_fill(struct lnxrm_fb_rect *urect)
{
    struct lnxrm_fb_rect r;
    if (!user_ptr_ok((u64)urect, sizeof(r))) return LNXRM_EFAULT;
    copy_from_user(&r, urect, sizeof(r));
    fb_fill_rect(r.x, r.y, r.w, r.h, r.color);
    return 0;
}

static long sys_fb_char(struct lnxrm_fb_char *uch)
{
    struct lnxrm_fb_char c;
    if (!user_ptr_ok((u64)uch, sizeof(c))) return LNXRM_EFAULT;
    copy_from_user(&c, uch, sizeof(c));
    fb_draw_char(c.x, c.y, (char)c.ch, c.fg, c.bg);
    return 0;
}

static long sys_fb_puts(struct lnxrm_fb_str *ustr)
{
    struct lnxrm_fb_str s;
    if (!user_ptr_ok((u64)ustr, sizeof(s))) return LNXRM_EFAULT;
    copy_from_user(&s, ustr, sizeof(s));
    char kbuf[256];
    if (copy_user_str(kbuf, (u64)s.str, sizeof(kbuf)) < 0) return LNXRM_EFAULT;
    fb_puts(s.x, s.y, kbuf, s.fg, s.bg);
    return 0;
}

/* fb_flip: publish everything drawn since the last flip, as one frame.
 * The drawing calls above put pixels into the offscreen back surface; this
 * is the call that puts them on the screen, so it carries the same two
 * gates as they do -- it touches the surface just as directly, it only
 * does it in the other direction.  A program draws a whole frame through
 * 24-27 and then flips once, instead of watching each call land. */
static long sys_fb_flip(void)
{
    fb_flip();
    return 0;
}

/* ---- keyboard syscalls (34-36) ----
 * kbd_get pops one buffered key and kbd_inject types on the console's
 * behalf; both race the shell for the shared input stream, so both are
 * CAP_SYS_ADMIN (ps/diskinfo's gate).  kbd_info is the open reader: it
 * answers counters and touches nothing, exactly fb_info's position. */
static long sys_kbd_inject(const char *ubuf, u32 len)
{
    if (!len) return 0; /* zero keys: a legal no-op, pointer unchecked */
    if (!user_ptr_ok((u64)ubuf, len)) return LNXRM_EFAULT;
    if (len > 256) return LNXRM_EINVAL;
    char kbuf[256];
    if (copy_from_user(kbuf, ubuf, len) < 0) return LNXRM_EFAULT;
    return kbd_inject_bytes(kbuf, len);
}

static long sys_kbd_info(struct lnxrm_kbd_info *uinfo)
{
    struct lnxrm_kbd_info info;
    if (!uinfo) return LNXRM_EFAULT;
    kbd_stats(&info);
    if (copy_to_user(uinfo, &info, sizeof(info)) < 0) return LNXRM_EFAULT;
    return 0;
}

/* Copy a NUL-terminated path out of user space and run a path syscall on it. */
static long path_syscall(u64 upath, long (*fn)(const char *))

{
    char path[128];
    if (copy_user_str(path, upath, sizeof(path)) != 0) return LNXRM_EFAULT;
    return fn(path);
}

/* Same, for the two-path syscalls (rename / move). */
static long path2_syscall(u64 upath, u64 upath2, long (*fn)(const char *, const char *))
{
    char path[128], path2[128];
    if (copy_user_str(path, upath, sizeof(path)) != 0) return LNXRM_EFAULT;
    if (copy_user_str(path2, upath2, sizeof(path2)) != 0) return LNXRM_EFAULT;
    return fn(path, path2);
}

/* Read the syscall arguments out of the trap frame and run fn.
 * This is the whole dispatch table: register ABI plumbing lives in
 * syscall_entry(), everything else is a plain C call. */
static long syscall_do(struct intr_frame *f, u64 nr, u64 a3)
{
    long ret = LNXRM_ENOSYS;

    switch (nr) {
    case SYS_read:
        ret = sys_read((int)f->rdi, (void *)f->rsi, a3);
        break;
    case SYS_write:
        ret = sys_write((int)f->rdi, (const void *)f->rsi, a3);
        break;
    case SYS_open: {
        char path[128];
        ret = copy_user_str(path, f->rdi, sizeof(path)) == 0 ? sys_open(path, (int)f->rsi)
                                                             : LNXRM_EFAULT;
        break;
    }
    case SYS_close:
        ret = sys_close((int)f->rdi);
        break;
    case SYS_lseek:
        ret = sys_lseek((int)f->rdi, (long)f->rsi, (int)a3);
        break;
    case SYS_brk:
        ret = sys_brk(f->rdi);
        break;
    case SYS_getdent:
        ret = sys_getdent((int)f->rdi, (void *)f->rsi, a3);
        break;
    case SYS_dup2:
        ret = sys_dup2((int)f->rdi, (int)f->rsi);
        break;
    case SYS_nanosleep:
        ret = sys_nanosleep(f->rdi, (struct lnxrm_timespec *)a3);
        break;
    case SYS_getpid:
        ret = current->pid;
        break;
    case SYS_fork:
        ret = sys_fork();
        break;
    case SYS_execve:
        ret = sys_execve((const char *)f->rdi, (char *const *)f->rsi, (char *const *)a3);
        break;
    case SYS_exit:
        sys_exit((int)f->rdi);   /* noreturn: no break needed */
    case SYS_wait4:
        ret = sys_waitpid((int)f->rdi, (int *)f->rsi, (int)a3);
        break;
    case SYS_kill:
        ret = sys_kill((int)f->rdi, (int)f->rsi);
        break;
    case SYS_uname:
        ret = sys_uname((struct lnxrm_utsname *)f->rdi);
        break;
    case SYS_sigaction:
        ret = sys_sigaction((int)f->rdi, (const struct lnxrm_sigaction *)f->rsi,
                            (struct lnxrm_sigaction *)a3);
        break;
    case SYS_sigprocmask:
        ret = sys_sigprocmask((int)f->rdi, (const u64 *)f->rsi, (u64 *)a3);
        break;
    case SYS_getppid:
        /* T-050: the stored pid half of the link; 0 when there is no parent
         * (init, or an orphan whose parent was reaped before reparenting). */
        ret = (int32_t)current->parent_pid;
        break;
    case SYS_ps:
        /* T-031: the process table is an enumeration of everyone else's
         * state -- CAP_SYS_ADMIN (fdisk's twin, diskinfo, is gated the
         * same way below). */
        if ((ret = cap_gate(CAP_SYS_ADMIN, "CAP_SYS_ADMIN", "ps")) != 0) break;
        ret = sys_ps((struct lnxrm_ps_entry *)f->rdi, (int)f->rsi);
        break;
    case SYS_sigreturn:
        ret = sys_sigreturn();
        break;
    case SYS_diskinfo:
        /* T-031: fdisk died with user/fdisk.c, and listing the block
         * devices was all it ever did -- so its gate is this one. */
        if ((ret = cap_gate(CAP_SYS_ADMIN, "CAP_SYS_ADMIN", "diskinfo")) != 0) break;
        ret = blk_list_all((void *)f->rdi, (int)f->rsi);
        break;
    case SYS_mkdir:
        ret = path_syscall(f->rdi, sys_mkdir);
        break;
    case SYS_fb_info:
        ret = sys_fb_info((struct lnxrm_fb_info *)f->rdi);
        break;
    /* T-033: both gates, in this order, on all five surface calls --
     * capability (may you draw at all) then ownership (are you the one
     * on the screen).  fb_flip is the fifth: it publishes rather than
     * draws, but it touches the surface just as directly.  fb_info above
     * has neither: it draws nothing. */
    case SYS_fb_clear:
        if ((ret = cap_gate(CAP_FB, "CAP_FB", "fb_clear")) != 0) break;
        if ((ret = fb_owner_gate("fb_clear")) != 0) break;
        ret = sys_fb_clear((u32)f->rdi);
        break;
    case SYS_fb_fill:
        if ((ret = cap_gate(CAP_FB, "CAP_FB", "fb_fill")) != 0) break;
        if ((ret = fb_owner_gate("fb_fill")) != 0) break;
        ret = sys_fb_fill((struct lnxrm_fb_rect *)f->rdi);
        break;
    case SYS_fb_char:
        if ((ret = cap_gate(CAP_FB, "CAP_FB", "fb_char")) != 0) break;
        if ((ret = fb_owner_gate("fb_char")) != 0) break;
        ret = sys_fb_char((struct lnxrm_fb_char *)f->rdi);
        break;
    case SYS_fb_puts:
        if ((ret = cap_gate(CAP_FB, "CAP_FB", "fb_puts")) != 0) break;
        if ((ret = fb_owner_gate("fb_puts")) != 0) break;
        ret = sys_fb_puts((struct lnxrm_fb_str *)f->rdi);
        break;
    case SYS_fb_flip:
        if ((ret = cap_gate(CAP_FB, "CAP_FB", "fb_flip")) != 0) break;
        if ((ret = fb_owner_gate("fb_flip")) != 0) break;
        ret = sys_fb_flip();
        break;
    case SYS_unlink:
        ret = path_syscall(f->rdi, sys_unlink);
        break;
    case SYS_rmdir:
        ret = path_syscall(f->rdi, sys_rmdir);
        break;
    case SYS_rename:
        ret = path2_syscall(f->rdi, f->rsi, sys_rename);
        break;
    case SYS_setuid:
        ret = sys_setuid((u32)f->rdi);
        break;
    case SYS_sync:
        ret = sys_sync(); /* C27: flush staged bytes to every device */
        break;
    case SYS_kbd_get:
        if ((ret = cap_gate(CAP_SYS_ADMIN, "CAP_SYS_ADMIN", "kbd_get")) != 0) break;
        ret = kbd_get_byte();
        break;
    case SYS_kbd_inject:
        if ((ret = cap_gate(CAP_SYS_ADMIN, "CAP_SYS_ADMIN", "kbd_inject")) != 0) break;
        ret = sys_kbd_inject((const char *)f->rdi, (u32)f->rsi);
        break;
    case SYS_kbd_info:
        ret = sys_kbd_info((struct lnxrm_kbd_info *)f->rdi);
        break;
    default:
        /* Anything the table does not name: numbers above LNXRM_SYSCALL_LAST. */
        kprintf("[sys] unknown syscall %lu from pid %u\n", (unsigned long)nr,
                current ? current->pid : 0);
        ret = LNXRM_ENOSYS;
    }
    return ret;
}

void syscall_entry(struct intr_frame *f)
{
    /* Same ring-0 entry contract as isr_common: a user-set EFLAGS.AC must
     * not survive into the kernel (it would disable SMAP for everything
     * below), while the iretq frame restores the user's flags later. */
    if (g_smap) __asm__ volatile("clac" ::: "memory");

    /* T-030: kxld exists to protect the kernel and never acts as a
     * user-side client; reaching the syscall path as kxld means a
     * privilege invariant broke. */
    BUG_ON(current && current->cred.kind == CRED_KXLD);

    if (current) current->tf = f;

    long ret = syscall_do(f, f->rax, f->r10);

    f->rax = (u64)(long)ret;
    f->rflags |= 0x200;
    /* Handle pending SIGSTOP/SIGCONT/SIGKILL and default-terminate signals
     * before iretq back to user mode. */
    if (current && (f->cs & 3) == 3) do_signal_check(current);
}

#ifdef CONFIG_KTEST
/* ktest hook: run the dispatch table against caller-built arguments.
 *
 * syscall_entry()'s half of the contract -- CLAC, the kxld BUG_ON, the tf
 * save, the signal check -- is ring-0 *user-entry* plumbing and starts
 * with int 0x80, which a kernel self-test cannot raise on itself.  The
 * other half -- the capability/owner gates, the pointer validation, the
 * per-syscall error codes -- is what the self-test wants, and this calls
 * exactly that half: syscall_do() with a zeroed frame.  Not reachable
 * from user space (it is not a syscall of its own) and built only when
 * CONFIG_KTEST is defined. (2026-10-20) */
long syscall_probe(u64 nr, u64 a1, u64 a2, u64 a3)
{
    struct intr_frame f;

    memset(&f, 0, sizeof(f));
    f.rax = nr;
    f.rdi = a1;
    f.rsi = a2;
    f.r10 = a3;
    return syscall_do(&f, nr, a3);
}
#endif
