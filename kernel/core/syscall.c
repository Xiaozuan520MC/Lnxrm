/* System call dispatch (int 0x80, nr in rax, args in rdi rsi r10). */
#include <sys/vfs.h>
#include <sys/sched.h>
#include <sys/cpu.h>
#include <sys/input.h>
#include <sys/protect.h>
#include <sys/rtc.h>
#include <console.h>
#include <mm/mm.h>
#include <mm/vma.h>
#include <framebuffer.h>

long sys_getdent(int fd, void *ubuf, size_t len);
long sys_lseek(int fd, long off, int whence);
int blk_list_all(void *ubuf, int max);

static long sys_brk(u64 newbrk)
{
    if (!newbrk) return (long)current->brk_cur;
    /* Ceiling = MMAP_LO, the bottom of the anonymous mmap window (types.h).
     * The old cap, one MiB below USER_STACK_TOP, sat INSIDE that window --
     * a maxed heap and an mmap region could have claimed the same pages.
     * The stack floor (MMAP_TOP) is above MMAP_LO with the mmap window
     * between them, so heap, mmap and stack can now never meet. */
    if ((u64)newbrk < (u64)current->brk_base || newbrk > MMAP_LO)
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

/* ---- mmap / munmap (T-MMAP, then file mappings) ----
 *
 * mmap is the six-argument Linux shape: mmap(addr, len, prot, flags, fd,
 * offset).  The int 0x80 ABI hands the dispatcher three of them (rdi, rsi,
 * r10, exactly as every other call has them) and the frame carries the rest
 * in rdx/rcx/r8 -- h_mmap reads them straight out, so no new register
 * plumbing was needed.  `addr` must still be 0: the kernel picks the
 * address top-down in [MMAP_LO, MMAP_TOP) and answering EINVAL for
 * anything else is honest where honouring a hint it would then ignore is
 * not.  prot is the LNXRM_PROT_* mask; unknown bits are masked off so a
 * later flag stays forward-compatible instead of breaking old callers.
 *
 * fd < 0 means "no file": an anonymous mapping, whose flags word is
 * normalised to MAP_PRIVATE (both names mean the same thing for a range
 * with no file behind it).  fd >= 0 must name a regular file opened for
 * reading, and a MAP_SHARED range that will be written must additionally
 * have been opened for writing -- otherwise the write-back at unmap would
 * be refused by the very descriptor it is writing through.
 *
 * Nothing is mapped here.  sys_mmap records one VMA (mm/vma.c) and the
 * first touch of each page goes through the #PF dispatch
 * (page_fault.c demand_page), which meters the frame against
 * cred_mem_quota() -- a file-backed page exactly like an anonymous one, so
 * mapping a 100 MiB file costs nothing until it is read and cannot be used
 * to sidestep the 64 MiB cap.  An untouched range therefore costs one
 * kmalloc node -- which is what lets the window be 64 GiB without
 * pretending to own 16 million frames it will never get. */
static long sys_mmap(u64 addr, u64 len, u64 prot, u64 flags, long fd, u64 offset)
{
    if (addr != 0) return LNXRM_EINVAL; /* kernel-chosen address only */
    if (!len) return LNXRM_EINVAL;
    if (len > MMAP_TOP - MMAP_LO) return LNXRM_ENOMEM;

    if (flags & ~(u64)LNXRM_MAP_FLAGS) return LNXRM_EINVAL;
    if ((flags & LNXRM_MAP_SHARED) && (flags & LNXRM_MAP_PRIVATE))
        return LNXRM_EINVAL; /* pick one: they mean opposite things */
    if (!flags) flags = LNXRM_MAP_PRIVATE;  /* no opinion == private */
    if (offset & (PAGE_SIZE - 1)) return LNXRM_EINVAL;
    if (offset + len < offset) return LNXRM_EINVAL; /* offset + len wraps */
    if (fd < 0 && offset) return LNXRM_EINVAL;      /* nothing to be at an offset of */

    u64 size = ALIGN_UP(len, PAGE_SIZE);
    if (size > MMAP_TOP - MMAP_LO) return LNXRM_ENOMEM;

    /* The file half, checked BEFORE an address is chosen: every reason to
     * say no is knowable from the descriptor alone, and a refusal must not
     * have cost a VMA node. */
    struct file *ff = NULL;
    if (fd >= 0) {
        if (fd >= NR_FDS || !current->fds[fd]) return LNXRM_EBADF;
        ff = current->fds[fd];
        if (ff->is_dir || !ff->priv) return LNXRM_EINVAL;
        struct vnode *vn = ff->priv;
        if (vn->type != V_REG) return LNXRM_EINVAL; /* no device, no directory */
        int mode = ff->flags & (O_RDONLY | O_WRONLY | O_RDWR);
        /* Every mapping READS its file -- that is what fills a page -- so
         * O_WRONLY is not enough, exactly as read() on it would fail. */
        if (mode != O_RDONLY && mode != O_RDWR) return LNXRM_EBADF;
        /* ...and a shared range with PROT_WRITE will be written back
         * through this same descriptor, so it must be open for writing. */
        if ((flags & LNXRM_MAP_SHARED) && (prot & LNXRM_PROT_WRITE) &&
            mode != O_RDWR)
            return LNXRM_EACCES;
        __sync_fetch_and_add(&ff->refcnt, 1); /* owned by the VMA from here */
    }

    /* Top-down placement: scan every gap in [MMAP_LO, MMAP_TOP) and take
     * the HIGHEST one that fits (mmap habit: grow down from the top, so
     * regions cluster below the stack floor and the low heap stays
     * contiguous for brk).  Each gap is [prev end, next start); page
     * alignment of both bounds makes `gap_hi - size` page-aligned too. */
    u64 best_hi = 0, prev_end = MMAP_LO;
    for (struct vma *v = current->vmas; v; v = v->next) {
        u64 lo = prev_end > MMAP_LO ? prev_end : MMAP_LO;
        if (v->start > lo && v->start - lo >= size && v->start > best_hi)
            best_hi = v->start;
        prev_end = v->end;
    }
    {
        u64 lo = prev_end > MMAP_LO ? prev_end : MMAP_LO;
        if (MMAP_TOP > lo && MMAP_TOP - lo >= size && MMAP_TOP > best_hi)
            best_hi = MMAP_TOP;
    }
    if (!best_hi) {
        if (ff) vfs_close_file(ff); /* the mapping never happened */
        return LNXRM_ENOMEM;        /* no gap fits: address space full */
    }

    u64 start = best_hi - size;
    int rc = vma_insert(current, start, start + size, (u32)(prot & 0x7),
                        (u32)flags, offset, ff);
    if (rc < 0) {
        if (ff) vfs_close_file(ff); /* the insert never took the reference */
        return rc;
    }
    return (long)start;
}

/* munmap(addr, len): drop [addr, ALIGN_UP(len)) from the VMA list and unmap
 * whatever pages happen to be present there.  POSIX-shaped: a range that
 * only PARTLY overlaps mapping is fine (the overlap is unmapped, the rest
 * of the VMA survives -- vma_erase cuts and splits), while a range that
 * intersects nothing answers EINVAL so a bad pointer is loud instead of a
 * silent no-op.  Shared (COW) frames survive their reference count: each
 * pmm_free here drops one owner, and the frame dies only with the last. */
static long sys_munmap(u64 addr, u64 len)
{
    if (addr & (PAGE_SIZE - 1)) return LNXRM_EINVAL;
    if (!len) return LNXRM_EINVAL;
    if (addr < MMAP_LO || addr >= MMAP_TOP) return LNXRM_EINVAL;

    u64 size = ALIGN_UP(len, PAGE_SIZE);
    if (size == 0 || addr + size < addr) return LNXRM_EINVAL; /* overflow */
    u64 end = addr + size > MMAP_TOP ? MMAP_TOP : addr + size;

    /* Shared pages in this range belong to the FILE, so they have to reach
     * it before the mapping -- and with it the VMA's file reference -- goes
     * away.  Runs first on purpose: if vma_erase() then answers EINVAL
     * (nothing intersects), no page was touched and no byte was written,
     * and if it answers ENOMEM the write-back that already happened was
     * merely early, never wrong (it only ever copies a page onto itself). */
    vma_writeback(current, current->pml4, addr, end);

    /* Range first: if nothing intersects, no page may be unmapped either.
     * vma_erase also guarantees it changed nothing when the split node
     * could not be allocated (ENOMEM), so the page loop below only ever
     * runs against a range the list has already conceded. */
    int rc = vma_erase(current, addr, end);
    if (rc < 0) return rc;

    for (u64 va = addr; va < end; va += PAGE_SIZE) {
        u64 pa = vmm_unmap_user(current->pml4, va);
        if (!pa) continue;
        pmm_free(pa);
        /* the frame may have been the last in its 2 MiB region: hand the
         * now-empty tables back too, or every mmap into a fresh region
         * permanently costs two frames (t_mmap's buddy-exact teardown
         * fails by exactly that on the first run). */
        vmm_reclaim_empty_tables(current->pml4, va);
    }
    /* Re-count rather than decrement: the page tables are the authority
     * (same rule as exec/fork/brk), so an unmapped count can never drift
     * from what ps will later report.  munmap is not a hot path. */
    current->mem_pages = vmm_count_user_pages(current->pml4);
    return 0;
}

/* ---- mprotect(addr, len, prot) (T-MPROTECT) ----
 *
 * Re-protect [addr, ALIGN_UP(len)) over ANY user range, mmap'd or not --
 * that reach is the point: flip a JIT region RW->RX, make .text writable
 * to patch it, seal a stack buffer.  Per page:
 *   - present            -> the PTE changes now (W and NX, below);
 *   - not present but claimed by a VMA -> only the VMA changes, and the
 *     demand pager will map the page with the new prot on first touch;
 *   - neither            -> ENOMEM (a hole: there is nothing to protect),
 *     reported before a single bit is touched.
 *
 * Granting W never allocates.  A frame another owner also holds is MARKED
 * PG_COW instead of copied -- the copy happens on the next write through
 * the existing COW fixer -- so the only allocation in this syscall is the
 * VMA split nodes, and vma_protect charges for those up front (ENOMEM
 * therefore always means "nothing changed").  Revoking W clears PG_COW
 * with it: a COW page left marked would be broken writable by the very
 * next write fault, silently undoing the revoke.
 *
 * Note the read side: x86 has no read-disable without PKU, so PROT_NONE
 * degrades to "no write, no execute" exactly as it does at mmap time. */
static long sys_mprotect(u64 addr, u64 len, u64 prot)
{
    if (addr & (PAGE_SIZE - 1)) return LNXRM_EINVAL;
    if (!len) return LNXRM_EINVAL;
    u64 size = ALIGN_UP(len, PAGE_SIZE);
    if (!size) return LNXRM_EINVAL; /* len near 2^64: ALIGN_UP wrapped */
    u64 end = addr + size;
    if (end < addr || !vmm_is_user_range(addr, end)) return LNXRM_EINVAL;
    prot &= 0x7; /* forward-compatible: unknown bits mean nothing today */

    /* Coverage first, changes second.  Walk in strides: a page inside a VMA
     * is covered whatever its state (untouched is fine -- the VMA will speak
     * for it), so jump to that VMA's end; a page outside one must be
     * present outright, or the range has a hole in it. */
    for (u64 pg = addr; pg < end;) {
        struct vma *v = vma_find(current, pg);
        if (v) {
            pg = v->end < end ? v->end : end;
            continue;
        }
        if (!vmm_user_pte(current->pml4, pg)) return LNXRM_ENOMEM;
        pg += PAGE_SIZE;
    }

    /* VMAs first: vma_protect is all-or-nothing, and once it returns 0 the
     * PTE pass below cannot fail (it only rewrites leaves it just read). */
    int rc = vma_protect(current, addr, end, (u32)prot);
    if (rc < 0) return rc;

    bool want_w = (prot & LNXRM_PROT_WRITE) != 0;
    bool want_x = (prot & LNXRM_PROT_EXEC) != 0;
    for (u64 pg = addr; pg < end; pg += PAGE_SIZE) {
        u64 pte = vmm_user_pte(current->pml4, pg);
        if (!pte) continue; /* untouched: the VMA already carries the new prot */
        u64 pa = pte & PTE_PA_MASK;
        u64 fresh = pte;

        if (want_x) fresh &= ~PG_NX;
        else fresh |= PG_NX;

        /* A MAP_SHARED file page is THE frame every mapping of that file
         * offset uses, so the reference count above is not a reason to hold
         * write back from it: pmm_refcount() > 1 here means "somebody else
         * maps this same file page", which is precisely the case where
         * making it copy-on-write would be wrong (this process would then
         * write a private copy that neither the other mapping nor the file
         * ever sees).  It gets W outright, and never PG_COW -- so nothing
         * later can mistake it for a page owed a private copy. */
        struct vma *v = vma_find(current, pg);
        bool shared = v && v->file && (v->flags & LNXRM_MAP_SHARED);

        if (want_w) {
            if (shared) {
                fresh = (fresh | PG_W) & ~PG_COW;
            } else if (fresh & PG_W) {
                /* W with another owner should be impossible -- fork clears
                 * it on both sides -- but if it ever appears, demote to COW
                 * rather than leave two spaces writing one frame. */
                if (!(fresh & PG_COW) && pmm_refcount(pa) > 1)
                    fresh = (fresh & ~PG_W) | PG_COW;
            } else if (!(fresh & PG_COW)) {
                /* Read-only today: give it W outright if this is the only
                 * owner, else mark PG_COW and let the write fault copy. */
                if (pmm_refcount(pa) > 1) fresh |= PG_COW;
                else fresh |= PG_W;
            }
            /* !W + PG_COW: already promised a private copy on write; the
             * VMA now says W too, so nothing here needs to change. */
        } else {
            fresh &= ~(PG_W | PG_COW); /* revoke: see the header comment */
        }

        if (fresh == pte) continue;
        if (vmm_user_pte_write(current->pml4, pg, fresh) < 0)
            return LNXRM_EFAIL; /* leaf vanished under us: impossible alone */
    }
    return 0;
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
 * until the holder exits (kernel/sched/task.c -> fb_disown).
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

/* ---- stat(45): one file's metadata, no descriptor involved ----
 *
 * The path arrives already copied (h_stat runs copy_user_str, the same
 * door every path syscall uses), and the result leaves through
 * copy_to_user -- so a bad buffer answers EFAULT exactly like a bad path
 * does, and neither can leave a half-filled struct behind (the kernel
 * builds a whole lnxrm_stat first, then moves it in one copy).
 *
 * `sys_stat` takes a KERNEL path on purpose, like sys_unlink and
 * sys_rename: the string gate belongs to the dispatcher, and the self-test
 * calls this directly against paths it built itself.  Everything about the
 * node itself comes from vfs_attr -- the translation below is only
 * "filesystem vocabulary -> st_mode bits", and it is deliberately total:
 * every vnode_type has an arm, so a new type cannot fall through to
 * "regular file" by accident. */
long sys_stat(const char *path, struct lnxrm_stat *buf)
{
    struct vfs_attr a;
    struct lnxrm_stat st;

    if (!path || !buf) return LNXRM_EFAULT;
    long err = vfs_stat_path(path, &a);
    if (err < 0) return err;

    memset(&st, 0, sizeof(st));
    st.st_dev = a.dev;
    st.st_ino = a.ino;
    st.st_size = a.size;
    st.st_blksize = 512;                    /* one sector, what the fs reads */
    st.st_blocks = (a.size + 511) / 512;    /* rounded up, like every df */
    st.st_nlink = 1;                        /* neither FAT32 nor initramfs
                                             * has hard links (see abi.h) */
    st.st_uid = 0;                          /* no per-file owner yet: root */
    st.st_gid = 0;

    switch (a.type) {
    case V_DIR:
        st.st_mode = LNXRM_S_IFDIR | (a.ro ? 0555 : 0755);
        break;
    case V_CHR:
        st.st_mode = LNXRM_S_IFCHR | 0666; /* /dev/console is everyone's */
        break;
    case V_REG:
        st.st_mode = LNXRM_S_IFREG | (a.ro ? 0444 : 0644);
        break;
    }
    if (copy_to_user(buf, &st, sizeof(st)) < 0) return LNXRM_EFAULT;
    return 0;
}

/* ---- gettimeofday(46): the wall clock ----
 *
 * Two arguments, exactly as Linux has them, with Linux's answer to the
 * second one: the timezone struct is written as zeros (this kernel has no
 * DST rule and the RTC reads as UTC -- see kernel/core/rtc.c).  Either
 * pointer may be NULL; both NULL is EINVAL, because a call that asks for
 * nothing is a mistake rather than a request to be satisfied.  The clock
 * itself is wall_clock()'s: an RTC sample advanced by the calibrated TSC,
 * so it never waits after the first call and never steps backwards. */
long sys_gettimeofday(struct lnxrm_timeval *tv, struct lnxrm_timezone *tz)
{
    if (!tv && !tz) return LNXRM_EINVAL;

    if (tz) {
        struct lnxrm_timezone z = {0, 0};
        if (copy_to_user(tz, &z, sizeof(z)) < 0) return LNXRM_EFAULT;
    }
    if (tv) {
        i64 sec = 0, usec = 0;
        int rc = wall_clock(&sec, &usec);
        if (rc < 0) return rc;
        struct lnxrm_timeval v = {sec, usec};
        if (copy_to_user(tv, &v, sizeof(v)) < 0) return LNXRM_EFAULT;
    }
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

/* The four drawing calls below run under console_out_lock: they write the
 * back surface, and the present copies it under the same lock (fb_flip),
 * so a frame being drawn on one CPU cannot land half-way into a present
 * on the other.  The unlock doubles as a batch end: before the first tick
 * the frame shows immediately, after it the tick presents (gate closed). */
static long sys_fb_clear(u32 color)
{
    u64 fl;
    console_out_lock(&fl);
    fb_clear(color);
    console_out_unlock(fl);
    return 0;
}

static long sys_fb_fill(struct lnxrm_fb_rect *urect)
{
    struct lnxrm_fb_rect r;
    if (!user_ptr_ok((u64)urect, sizeof(r))) return LNXRM_EFAULT;
    copy_from_user(&r, urect, sizeof(r));
    u64 fl;
    console_out_lock(&fl);
    fb_fill_rect(r.x, r.y, r.w, r.h, r.color);
    console_out_unlock(fl);
    return 0;
}

static long sys_fb_char(struct lnxrm_fb_char *uch)
{
    struct lnxrm_fb_char c;
    if (!user_ptr_ok((u64)uch, sizeof(c))) return LNXRM_EFAULT;
    copy_from_user(&c, uch, sizeof(c));
    u64 fl;
    console_out_lock(&fl);
    fb_draw_char(c.x, c.y, (char)c.ch, c.fg, c.bg);
    console_out_unlock(fl);
    return 0;
}

static long sys_fb_puts(struct lnxrm_fb_str *ustr)
{
    struct lnxrm_fb_str s;
    if (!user_ptr_ok((u64)ustr, sizeof(s))) return LNXRM_EFAULT;
    copy_from_user(&s, ustr, sizeof(s));
    char kbuf[256];
    if (copy_user_str(kbuf, (u64)s.str, sizeof(kbuf)) < 0) return LNXRM_EFAULT;
    u64 fl;
    console_out_lock(&fl);
    fb_puts(s.x, s.y, kbuf, s.fg, s.bg);
    console_out_unlock(fl);
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

/* ---- mouse syscalls (37-38) ----
 * mouse_get pops one decoded PS/2 packet; like kbd_get the events are
 * single-shot state, so it rides CAP_SYS_ADMIN.  Validate the 16-byte
 * receiver first (EFAULT on NULL/bad -- even with an empty ring, per the
 * EFAULT-on-bad-pointer rule), then pop, then copy.
 * mouse_info is the open counter reader, exactly kbd_info. */
static long sys_mouse_get(struct lnxrm_mouse_state *um)
{
    if (!um) return LNXRM_EFAULT;
    if (!user_ptr_ok((u64)um, sizeof(*um))) return LNXRM_EFAULT;
    struct lnxrm_mouse_state m;
    if (mouse_pop(&m) != 0) return LNXRM_EAGAIN;
    if (copy_to_user(um, &m, sizeof(m)) < 0) return LNXRM_EFAULT;
    return 0;
}

static long sys_mouse_info(struct lnxrm_mouse_info *uinfo)
{
    struct lnxrm_mouse_info info;
    if (!uinfo) return LNXRM_EFAULT;
    ms_stats(&info);
    if (copy_to_user(uinfo, &info, sizeof(info)) < 0) return LNXRM_EFAULT;
    return 0;
}

/* ---- unified input event queue syscalls (39-40) ----
 * input_pop reads the one stream a compositor wants -- keyboard makes,
 * mouse deltas, button transitions, wheel -- over both existing producers
 * at once (see include/sys/input.h and kernel/drivers/input_event.c).  The raw
 * keys inside are exactly what kbd_get 34 refused to hand a plain user,
 * so the same CAP_SYS_ADMIN gate rides along, in the same order: gate
 * first, then the 24-byte receiver check (EFAULT on a bad pointer even
 * with an empty queue), then pop, then copy.  Success answers 1 -- one
 * event -- like a read() answers a byte count; an empty queue is EAGAIN.
 * input_info is the open counter reader, exactly kbd_info/mouse_info. */
static long sys_input_pop(struct lnxrm_input_event *ue)
{
    if (!ue) return LNXRM_EFAULT;
    if (!user_ptr_ok((u64)ue, sizeof(*ue))) return LNXRM_EFAULT;
    struct lnxrm_input_event e;
    if (input_event_pop(&e) != 0) return LNXRM_EAGAIN;
    if (copy_to_user(ue, &e, sizeof(e)) < 0) return LNXRM_EFAULT;
    return 1;
}

static long sys_input_info(struct lnxrm_input_info *uinfo)
{
    struct lnxrm_input_info info;
    if (!uinfo) return LNXRM_EFAULT;
    input_event_stats(&info);
    if (copy_to_user(uinfo, &info, sizeof(info)) < 0) return LNXRM_EFAULT;
    return 0;
}

/* input_wait (41): blocking read of the unified input event queue.
 * Gate: CAP_SYS_ADMIN first (same order as input_pop).  Then validate the
 * receiver pointer (EFAULT even on an empty queue).  Then block on the
 * queue's wait list until an event arrives, an actionable signal lands
 * (-> LNXRM_EINTR), or ms passes (-> 0).  ms==0 answers like input_pop
 * (1 event / EAGAIN); ms==(u64)-1 waits for an event or a signal only.
 *
 * Returns: 1 (one event copied to *ue), -EACCES (gate), -EFAULT (bad
 * pointer), -EINTR (signal), 0 (timeout). */
static long sys_input_wait(struct lnxrm_input_event *ue, u64 ms)
{
    if (!ue) return LNXRM_EFAULT;
    if (!user_ptr_ok((u64)ue, sizeof(*ue))) return LNXRM_EFAULT;
    return input_event_wait(ue, ms);
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

/* ================= dispatch =================
 *
 * One row per system call: the entry point, the gates that must answer
 * before it runs, and the name those gates print when they refuse.  The
 * h_* functions are only the frame-to-C translation each switch case
 * used to do -- one call wide, no dispatch logic of their own.
 *
 * Adding a call: a number in lnxrm_syscalls.def, one wrapper, one row
 * here.  LNXRM_SYSCALL_LAST is the table's bound, so every number above
 * it still answers ENOSYS without needing a row of its own. */
typedef long (*sys_fn)(struct intr_frame *f, u64 a3);

enum {
    /* T-031: the process table and the block-device list enumerate
     * everyone else's state, and so do the raw key/mouse bytes and the
     * unified event queue -- one CAP_SYS_ADMIN receipt each, printed by
     * cap_gate naming the row that said no. */
    GATE_ADMIN = 1u << 0,
    /* T-033: both gates, in this order, on all five surface calls --
     * capability (may you draw at all) then ownership (are you the one
     * on the screen).  fb_flip is the fifth: it publishes rather than
     * draws, but it touches the surface just as directly.  fb_info has
     * neither: it draws nothing. */
    GATE_FB = 1u << 1,
};

struct sysent {
    sys_fn fn;        /* NULL: a number this kernel does not implement */
    u8 gates;         /* GATE_* bits, applied in the order listed above */
    const char *name; /* what a refused gate reports */
};

/* ---- argument marshalling: trap-frame fields -> C types ---- */
static long h_read(struct intr_frame *f, u64 a3)
{
    return sys_read((int)f->rdi, (void *)f->rsi, a3);
}
static long h_write(struct intr_frame *f, u64 a3)
{
    return sys_write((int)f->rdi, (const void *)f->rsi, a3);
}
static long h_open(struct intr_frame *f, u64 a3)
{
    char path[128];
    return copy_user_str(path, f->rdi, sizeof(path)) == 0 ? sys_open(path, (int)f->rsi)
                                                          : LNXRM_EFAULT;
}
static long h_close(struct intr_frame *f, u64 a3) { return sys_close((int)f->rdi); }
static long h_lseek(struct intr_frame *f, u64 a3)
{
    return sys_lseek((int)f->rdi, (long)f->rsi, (int)a3);
}
static long h_brk(struct intr_frame *f, u64 a3) { return sys_brk(f->rdi); }
/* mmap is the one row wider than the dispatcher's three arguments: rdi,
 * rsi and a3 (=r10) are the first three exactly as every other call has
 * them, and flags/fd/offset ride in the next three registers the int 0x80
 * stub already pushed into the frame.  Nothing new is plumbed -- the frame
 * has always carried them. */
static long h_mmap(struct intr_frame *f, u64 a3)
{
    return sys_mmap(f->rdi, f->rsi, a3, f->rdx, (long)f->rcx, f->r8);
}
static long h_munmap(struct intr_frame *f, u64 a3) { return sys_munmap(f->rdi, f->rsi); }
static long h_mprotect(struct intr_frame *f, u64 a3) { return sys_mprotect(f->rdi, f->rsi, a3); }
static long h_getdent(struct intr_frame *f, u64 a3)
{
    return sys_getdent((int)f->rdi, (void *)f->rsi, a3);
}
static long h_dup2(struct intr_frame *f, u64 a3) { return sys_dup2((int)f->rdi, (int)f->rsi); }
static long h_nanosleep(struct intr_frame *f, u64 a3)
{
    return sys_nanosleep(f->rdi, (struct lnxrm_timespec *)a3);
}
static long h_getpid(struct intr_frame *f, u64 a3) { return current->pid; }
static long h_fork(struct intr_frame *f, u64 a3) { return sys_fork(); }
static long h_execve(struct intr_frame *f, u64 a3)
{
    return sys_execve((const char *)f->rdi, (char *const *)f->rsi, (char *const *)a3);
}
/* sys_exit is noreturn, so this row never comes back -- the old switch
 * fell out of `case SYS_exit` into `case SYS_wait4` for want of a break;
 * wait4 is now simply its own row. */
static long h_exit(struct intr_frame *f, u64 a3) { sys_exit((int)f->rdi); }
static long h_wait4(struct intr_frame *f, u64 a3)
{
    return sys_waitpid((int)f->rdi, (int *)f->rsi, (int)a3);
}
static long h_kill(struct intr_frame *f, u64 a3) { return sys_kill((int)f->rdi, (int)f->rsi); }
static long h_uname(struct intr_frame *f, u64 a3)
{
    return sys_uname((struct lnxrm_utsname *)f->rdi);
}
static long h_sigaction(struct intr_frame *f, u64 a3)
{
    return sys_sigaction((int)f->rdi, (const struct lnxrm_sigaction *)f->rsi,
                         (struct lnxrm_sigaction *)a3);
}
static long h_sigprocmask(struct intr_frame *f, u64 a3)
{
    return sys_sigprocmask((int)f->rdi, (const u64 *)f->rsi, (u64 *)a3);
}
/* T-050: the stored pid half of the link; 0 when there is no parent
 * (init, or an orphan whose parent was reaped before reparenting). */
static long h_getppid(struct intr_frame *f, u64 a3) { return (int32_t)current->parent_pid; }
static long h_ps(struct intr_frame *f, u64 a3)
{
    return sys_ps((struct lnxrm_ps_entry *)f->rdi, (int)f->rsi);
}
static long h_sigreturn(struct intr_frame *f, u64 a3) { return sys_sigreturn(); }
static long h_diskinfo(struct intr_frame *f, u64 a3)
{
    /* T-031: fdisk died with user/fdisk.c, and listing the block devices
     * was all it ever did -- so its gate is this one. */
    return blk_list_all((void *)f->rdi, (int)f->rsi);
}
static long h_mkdir(struct intr_frame *f, u64 a3) { return path_syscall(f->rdi, sys_mkdir); }
static long h_fb_info(struct intr_frame *f, u64 a3)
{
    return sys_fb_info((struct lnxrm_fb_info *)f->rdi);
}
static long h_fb_clear(struct intr_frame *f, u64 a3) { return sys_fb_clear((u32)f->rdi); }
static long h_fb_fill(struct intr_frame *f, u64 a3)
{
    return sys_fb_fill((struct lnxrm_fb_rect *)f->rdi);
}
static long h_fb_char(struct intr_frame *f, u64 a3)
{
    return sys_fb_char((struct lnxrm_fb_char *)f->rdi);
}
static long h_fb_puts(struct intr_frame *f, u64 a3)
{
    return sys_fb_puts((struct lnxrm_fb_str *)f->rdi);
}
static long h_fb_flip(struct intr_frame *f, u64 a3) { return sys_fb_flip(); }
static long h_unlink(struct intr_frame *f, u64 a3) { return path_syscall(f->rdi, sys_unlink); }
static long h_rmdir(struct intr_frame *f, u64 a3) { return path_syscall(f->rdi, sys_rmdir); }
static long h_rename(struct intr_frame *f, u64 a3)
{
    return path2_syscall(f->rdi, f->rsi, sys_rename);
}
/* stat carries a path AND a buffer: the string gate runs here (it cannot
 * be path_syscall, whose whole job is calling a one-argument function)
 * and the buffer is checked by the copy at the end of sys_stat. */
static long h_stat(struct intr_frame *f, u64 a3)
{
    char path[128];
    if (copy_user_str(path, f->rdi, sizeof(path)) != 0) return LNXRM_EFAULT;
    return sys_stat(path, (struct lnxrm_stat *)f->rsi);
}
static long h_gettimeofday(struct intr_frame *f, u64 a3)
{
    return sys_gettimeofday((struct lnxrm_timeval *)f->rdi,
                            (struct lnxrm_timezone *)f->rsi);
}
static long h_setuid(struct intr_frame *f, u64 a3) { return sys_setuid((u32)f->rdi); }
/* C27: flush staged bytes to every device */
static long h_sync(struct intr_frame *f, u64 a3) { return sys_sync(); }
static long h_kbd_get(struct intr_frame *f, u64 a3) { return kbd_get_byte(); }
static long h_kbd_inject(struct intr_frame *f, u64 a3)
{
    return sys_kbd_inject((const char *)f->rdi, (u32)f->rsi);
}
static long h_kbd_info(struct intr_frame *f, u64 a3)
{
    return sys_kbd_info((struct lnxrm_kbd_info *)f->rdi);
}
static long h_mouse_get(struct intr_frame *f, u64 a3)
{
    return sys_mouse_get((struct lnxrm_mouse_state *)f->rdi);
}
static long h_mouse_info(struct intr_frame *f, u64 a3)
{
    return sys_mouse_info((struct lnxrm_mouse_info *)f->rdi);
}
static long h_input_pop(struct intr_frame *f, u64 a3)
{
    return sys_input_pop((struct lnxrm_input_event *)f->rdi);
}
static long h_input_info(struct intr_frame *f, u64 a3)
{
    return sys_input_info((struct lnxrm_input_info *)f->rdi);
}
static long h_input_wait(struct intr_frame *f, u64 a3)
{
    return sys_input_wait((struct lnxrm_input_event *)f->rdi, (u64)f->rsi);
}

/* The table itself, in lnxrm_syscalls.def order.  Rows are complete on
 * purpose: clang's -Wmissing-field-initializers (under -Wextra) flags a
 * partial one, so the two zero fields are spelled out rather than left. */
static const struct sysent sysents[LNXRM_SYSCALL_LAST + 1] = {
    [SYS_read]      = { h_read, 0, NULL },
    [SYS_write]     = { h_write, 0, NULL },
    [SYS_open]      = { h_open, 0, NULL },
    [SYS_close]     = { h_close, 0, NULL },
    [SYS_lseek]     = { h_lseek, 0, NULL },
    [SYS_brk]       = { h_brk, 0, NULL },
    [SYS_getdent]   = { h_getdent, 0, NULL },
    [SYS_dup2]      = { h_dup2, 0, NULL },
    [SYS_nanosleep] = { h_nanosleep, 0, NULL },
    [SYS_getpid]    = { h_getpid, 0, NULL },
    [SYS_fork]      = { h_fork, 0, NULL },
    [SYS_execve]    = { h_execve, 0, NULL },
    [SYS_exit]      = { h_exit, 0, NULL },
    [SYS_wait4]     = { h_wait4, 0, NULL },
    [SYS_kill]      = { h_kill, 0, NULL },
    [SYS_uname]     = { h_uname, 0, NULL },
    [SYS_sigaction] = { h_sigaction, 0, NULL },
    [SYS_sigprocmask] = { h_sigprocmask, 0, NULL },
    [SYS_getppid]   = { h_getppid, 0, NULL },
    [SYS_ps]        = { h_ps, GATE_ADMIN, "ps" },
    [SYS_sigreturn] = { h_sigreturn, 0, NULL },
    [SYS_diskinfo]  = { h_diskinfo, GATE_ADMIN, "diskinfo" },
    [SYS_mkdir]     = { h_mkdir, 0, NULL },
    [SYS_fb_info]   = { h_fb_info, 0, NULL },
    [SYS_fb_clear]  = { h_fb_clear, GATE_FB, "fb_clear" },
    [SYS_fb_fill]   = { h_fb_fill, GATE_FB, "fb_fill" },
    [SYS_fb_char]   = { h_fb_char, GATE_FB, "fb_char" },
    [SYS_fb_puts]   = { h_fb_puts, GATE_FB, "fb_puts" },
    [SYS_unlink]    = { h_unlink, 0, NULL },
    [SYS_rmdir]     = { h_rmdir, 0, NULL },
    [SYS_rename]    = { h_rename, 0, NULL },
    [SYS_setuid]    = { h_setuid, 0, NULL },
    [SYS_fb_flip]   = { h_fb_flip, GATE_FB, "fb_flip" },
    [SYS_sync]      = { h_sync, 0, NULL },
    [SYS_kbd_get]   = { h_kbd_get, GATE_ADMIN, "kbd_get" },
    [SYS_kbd_inject] = { h_kbd_inject, GATE_ADMIN, "kbd_inject" },
    [SYS_kbd_info]  = { h_kbd_info, 0, NULL },
    [SYS_mouse_get] = { h_mouse_get, GATE_ADMIN, "mouse_get" },
    [SYS_mouse_info] = { h_mouse_info, 0, NULL },
    [SYS_input_pop] = { h_input_pop, GATE_ADMIN, "input_pop" },
    [SYS_input_info] = { h_input_info, 0, NULL },
    [SYS_input_wait] = { h_input_wait, GATE_ADMIN, "input_wait" },
    [SYS_mmap]      = { h_mmap, 0, NULL },
    [SYS_munmap]    = { h_munmap, 0, NULL },
    [SYS_mprotect]  = { h_mprotect, 0, NULL },
    [SYS_stat]      = { h_stat, 0, NULL },
    [SYS_gettimeofday] = { h_gettimeofday, 0, NULL },
};

/* Look the row up, refuse what the gates refuse, then run it.  Register
 * ABI plumbing lives in syscall_entry(); everything after that is this. */
static long syscall_do(struct intr_frame *f, u64 nr, u64 a3)
{
    const struct sysent *e = nr <= (u64)LNXRM_SYSCALL_LAST ? &sysents[nr] : NULL;

    if (!e || !e->fn) {
        /* Anything the table does not name: numbers above LNXRM_SYSCALL_LAST. */
        kprintf("[sys] unknown syscall %lu from pid %u\n", (unsigned long)nr,
                current ? current->pid : 0);
        return LNXRM_ENOSYS;
    }

    long ret;
    if (e->gates & GATE_ADMIN) {
        if ((ret = cap_gate(CAP_SYS_ADMIN, "CAP_SYS_ADMIN", e->name)) != 0) return ret;
    }
    if (e->gates & GATE_FB) {
        if ((ret = cap_gate(CAP_FB, "CAP_FB", e->name)) != 0) return ret;
        if ((ret = fb_owner_gate(e->name)) != 0) return ret;
    }
    return e->fn(f, a3);
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
    /* The three-argument shape every existing call uses.  mmap is wider
     * than this now, so its last three arguments default to "no file, no
     * flags, no offset" -- which is exactly what an anonymous mapping is --
     * and a test that wants a real descriptor goes through
     * syscall_probe6().  Zeroing them rather than reading a fresh frame
     * is what keeps the two-argument callers of SYS_mmap honest. */
    return syscall_probe6(nr, a1, a2, a3, 0, (u64)-1, 0);
}

long syscall_probe6(u64 nr, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6)
{
    struct intr_frame f;

    memset(&f, 0, sizeof(f));
    f.rax = nr;
    f.rdi = a1;
    f.rsi = a2;
    f.r10 = a3;
    f.rdx = a4; /* mmap's flags */
    f.rcx = a5; /* ... its fd   */
    f.r8 = a6;  /* ... and its offset */
    return syscall_do(&f, nr, a3);
}
#endif
