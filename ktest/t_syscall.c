/* Dispatch-level pointer validation: syscall_do()'s gates are what stands
 * between a user pointer and a kernel dereference, and no other self-test
 * reaches them -- t_usercopy proves the copy primitives in isolation,
 * usr/systest exercises only the paths a shell happens to walk from ring
 * 3.  syscall_probe() runs the real dispatch table (gates, pointer
 * checks, per-syscall error codes) against caller-built arguments; the
 * only thing it skips is the int 0x80 entry plumbing, which a kernel-side
 * case cannot raise on itself.
 *
 * Per call: a bad pointer -- NULL, kernel, non-canonical, unmapped user,
 * address-space wrap -- answers -14 (or the refusal documented as coming
 * first: EBADF for a bad fd, EINVAL for a bad SIGKILL action, ENOSYS past
 * the table), never a crash and never success-without-effect.  The read
 * and getdent position assertions fail against the pre-2026-10-20 code,
 * which consumed the file position / directory cookie before discovering
 * the destination was bad.
 *
 * Deliberately NOT probed here: nanosleep (it sleeps -- the LAPIC timer
 * does not run yet at KTEST_LATE, so jiffies never advances and the call
 * would hang the boot), fork/exit/sigreturn/setuid (they mutate the
 * caller's identity or task state, which the self-test must not do), and
 * the pure-scalar calls that take no pointer at all. (2026-10-20) */
#include <sys/ktest.h>
#include <console.h>
#include <mm/mm.h>
#include <sys/sched.h>
#include <sys/cpu.h>
#include <sys/vfs.h>
#include <framebuffer.h>

#define SWAP_VA (USER_BASE + 0x7000) /* other ktest files unmap theirs */
#define NONCANON 0x8000000000000000ULL
#define UNMAPPED (USER_BASE + 0x9000)

static u64 swpa;

static void uput(u64 va, const void *src, size_t n)
{
    u64 ac = smap_enter();
    memcpy((void *)va, src, n);
    smap_leave(ac);
}
static void uget(void *dst, u64 va, size_t n)
{
    u64 ac = smap_enter();
    memcpy(dst, (const void *)va, n);
    smap_leave(ac);
}
/* sequential objects inside the one test page: exact sizes, no layout math */
static u64 ualloc(u64 *cur, size_t n)
{
    u64 p = *cur;
    *cur += ((u64)n + 15) & ~(u64)15;
    return p;
}

static void test_syscall_pointer_sweep(void)
{
    u8 klocal[8];
    long r;

    if (!vfs_root_ready()) {
        ktest_skip("no root filesystem");
        return;
    }

    swpa = pmm_alloc();
    K_ASSERT(swpa != 0);
    K_ASSERT_EQ(vmm_map_user(current->pml4, SWAP_VA, swpa, true, true, false), 0);

    u64 ub = SWAP_VA + 0x100;
    u64 p_readme = ualloc(&ub, 16);
    u64 p_nosuch = ualloc(&ub, 24);
    u64 p_root = ualloc(&ub, 4);
    u64 p_uname = ualloc(&ub, sizeof(struct lnxrm_utsname));
    u64 p_info = ualloc(&ub, sizeof(struct lnxrm_fb_info));
    u64 p_ps = ualloc(&ub, 2 * sizeof(struct lnxrm_ps_entry));
    u64 p_blk = ualloc(&ub, 4 * 28);
    u64 p_rd = ualloc(&ub, 32);
    u64 p_de1 = ualloc(&ub, 2 * sizeof(struct lnxrm_dirent));
    u64 p_de2 = ualloc(&ub, 2 * sizeof(struct lnxrm_dirent));
    u64 p_fbs = ualloc(&ub, sizeof(struct lnxrm_fb_str));
    K_ASSERT(ub <= SWAP_VA + PAGE_SIZE); /* the page must hold the layout */

    uput(p_readme, "/README.md", 11);
    uput(p_nosuch, "/bin/no_such_elf", 17);
    uput(p_root, "/", 2);

    /* ---- plumbing: a fully valid call through the probe ---- */
    K_EXPECT_EQ(syscall_probe(SYS_uname, p_uname, 0, 0), 0);
    struct lnxrm_utsname u1;
    uget(&u1, p_uname, sizeof(u1));
    K_EXPECT_EQ(strcmp(u1.sysname, "lnxrm"), 0);

    /* ---- string-path calls: every bad flavor answers EFAULT ---- */
#define SWEEP_STR(nr)                                                          \
    do {                                                                       \
        K_EXPECT_EQ(syscall_probe(nr, 0, 0, 0), LNXRM_EFAULT);                 \
        K_EXPECT_EQ(syscall_probe(nr, (u64)klocal, 0, 0), LNXRM_EFAULT);       \
        K_EXPECT_EQ(syscall_probe(nr, NONCANON, 0, 0), LNXRM_EFAULT);          \
        K_EXPECT_EQ(syscall_probe(nr, UNMAPPED, 0, 0), LNXRM_EFAULT);          \
        K_EXPECT_EQ(syscall_probe(nr, ~0ULL - 7, 0, 0), LNXRM_EFAULT);         \
    } while (0)

    SWEEP_STR(SYS_open);
    SWEEP_STR(SYS_mkdir);
    SWEEP_STR(SYS_unlink);
    SWEEP_STR(SYS_rmdir);
    SWEEP_STR(SYS_rename); /* path1 bad: refused before path2 is read */
    SWEEP_STR(SYS_execve); /* path first, so nothing is ever loaded */
#undef SWEEP_STR

    /* rename with a good path1 and a bad path2: both copies run before the
     * operation, so the second failure must not half-rename anything */
    K_EXPECT_EQ(syscall_probe(SYS_rename, p_readme, NONCANON, 0), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_rename, p_readme, UNMAPPED, 0), LNXRM_EFAULT);

    /* execve: NULL argv stays legal; a NON-NULL but unreachable array is
     * EFAULT (the pre-fix code dropped it and ran with argc 0).  The path
     * is a real miss either way, so a regression here answers -2 without
     * ever spawning anything. */
    K_EXPECT_EQ(syscall_probe(SYS_execve, p_nosuch, NONCANON, 0), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_execve, p_nosuch, UNMAPPED, 0), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_execve, p_nosuch, 0, 0), LNXRM_ENOENT);

    /* ---- read: bad destination refused WITHOUT consuming the file ---- */
    long fd = syscall_probe(SYS_open, p_readme, 0, 0);
    K_ASSERT(fd >= 0);
    K_EXPECT_EQ(syscall_probe(SYS_lseek, (u64)fd, 0, LNXRM_SEEK_CUR), 0);
    K_EXPECT_EQ(syscall_probe(SYS_read, (u64)fd, (u64)klocal, 16), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_read, (u64)fd, 0, 16), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_read, (u64)fd, NONCANON, 16), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_read, (u64)fd, UNMAPPED, 16), LNXRM_EFAULT);
    /* the position still reads 0: the refused reads moved nothing */
    K_EXPECT_EQ(syscall_probe(SYS_lseek, (u64)fd, 0, LNXRM_SEEK_CUR), 0);
    /* fd gate first: a bad fd is EBADF, not EFAULT */
    K_EXPECT_EQ(syscall_probe(SYS_read, 12345, (u64)klocal, 16), LNXRM_EBADF);
    /* the happy path still works, and a zero count is a legal no-op */
    K_EXPECT_EQ(syscall_probe(SYS_read, (u64)fd, p_rd, 16), 16);
    K_EXPECT_EQ(syscall_probe(SYS_read, (u64)fd, p_rd, 0), 0);
    K_EXPECT_EQ(syscall_probe(SYS_close, (u64)fd, 0, 0), 0);

    /* ---- write: the copy happens first, a bad buffer never touches the
     * file (README.md must stay byte-identical for the smoke steps) ---- */
    long wf = syscall_probe(SYS_open, p_readme, 1 /* O_WRONLY */, 0);
    K_ASSERT(wf >= 0);
    K_EXPECT_EQ(syscall_probe(SYS_write, (u64)wf, (u64)klocal, 8), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_write, (u64)wf, 0, 8), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_write, (u64)wf, NONCANON, 8), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_close, (u64)wf, 0, 0), 0);

    /* ---- getdent: a refused destination must not eat a directory entry.
     * d1 is refused while still fresh, then read; d2 is a fresh iterator.
     * Both first reads must yield the same name -- against the pre-fix
     * code the refusal consumed d1's first entry and the names differ. */
    long d1 = syscall_probe(SYS_open, p_root, 0, 0);
    long d2 = syscall_probe(SYS_open, p_root, 0, 0);
    K_ASSERT(d1 >= 0);
    K_ASSERT(d2 >= 0);
    K_EXPECT_EQ(syscall_probe(SYS_getdent, (u64)d1, (u64)klocal, 144), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_getdent, (u64)d1, UNMAPPED, 144), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_getdent, (u64)d1, 0, 144), LNXRM_EFAULT);
    r = syscall_probe(SYS_getdent, (u64)d1, p_de1, 144);
    K_ASSERT(r >= (long)sizeof(struct lnxrm_dirent));
    long r2 = syscall_probe(SYS_getdent, (u64)d2, p_de2, 144);
    K_ASSERT(r2 >= (long)sizeof(struct lnxrm_dirent));
    struct lnxrm_dirent de1, de2;
    uget(&de1, p_de1, sizeof(de1));
    uget(&de2, p_de2, sizeof(de2));
    K_EXPECT(de1.d_name[0] != 0);
    K_EXPECT(strcmp(de1.d_name, de2.d_name) == 0);
    /* not a directory: EBADF, the documented answer of the is_dir check */
    long fr = syscall_probe(SYS_open, p_readme, 0, 0);
    K_ASSERT(fr >= 0);
    K_EXPECT_EQ(syscall_probe(SYS_getdent, (u64)fr, p_de1, 144), LNXRM_EBADF);
    K_EXPECT_EQ(syscall_probe(SYS_close, (u64)d1, 0, 0), 0);
    K_EXPECT_EQ(syscall_probe(SYS_close, (u64)d2, 0, 0), 0);
    K_EXPECT_EQ(syscall_probe(SYS_close, (u64)fr, 0, 0), 0);

    /* ---- output-struct calls ---- */
    K_EXPECT_EQ(syscall_probe(SYS_uname, (u64)klocal, 0, 0), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_uname, 0, 0, 0), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_uname, NONCANON, 0, 0), LNXRM_EFAULT);

    /* fb_info: no capability or owner gate, so this is pure validation */
    K_EXPECT_EQ(syscall_probe(SYS_fb_info, (u64)klocal, 0, 0), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_fb_info, 0, 0, 0), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_fb_info, p_info, 0, 0), 0);
    struct lnxrm_fb_info fin;
    uget(&fin, p_info, sizeof(fin));
    K_EXPECT(fin.width > 0 && fin.height > 0);

    /* the five drawing calls: gates pass for this caller (kxld holds every
     * capability, the surface is free), then the pointer check refuses */
    K_ASSERT_EQ(fb_owner_get(), 0);
    K_EXPECT_EQ(syscall_probe(SYS_fb_fill, (u64)klocal, 0, 0), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_fb_fill, 0, 0, 0), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_fb_char, NONCANON, 0, 0), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_fb_puts, UNMAPPED, 0, 0), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_fb_flip, 0, 0, 0), 0); /* pointer-free publish */
    /* two-level check: a well-formed struct whose string field is bad */
    struct lnxrm_fb_str fs = {0, 0, 0xffffff, 0, (const char *)NONCANON};
    uput(p_fbs, &fs, sizeof(fs));
    K_EXPECT_EQ(syscall_probe(SYS_fb_puts, p_fbs, 0, 0), LNXRM_EFAULT);

    /* ps: bad buffers refused, NULL with max 0 stays the legal count-only
     * form, a real buffer answers */
    K_EXPECT_EQ(syscall_probe(SYS_ps, (u64)klocal, 4, 0), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_ps, 0, 4, 0), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_ps, NONCANON, 4, 0), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_ps, 0, 0, 0), 0);
    K_EXPECT(syscall_probe(SYS_ps, p_ps, 2, 0) >= 0);

    /* diskinfo: a bad buffer used to answer 0 ("no devices"), a negative
     * count used to answer that negative number */
    K_EXPECT_EQ(syscall_probe(SYS_diskinfo, (u64)klocal, 4, 0), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_diskinfo, UNMAPPED, 4, 0), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_diskinfo, 0, -1, 0), LNXRM_EINVAL);
    K_EXPECT(syscall_probe(SYS_diskinfo, p_blk, 4, 0) >= 0);

    /* wait4: the bad status pointer is refused BEFORE the (non-)reap;
     * idle has no children, so the good-pointer form is ECHILD */
    K_EXPECT_EQ(syscall_probe(SYS_wait4, 0, (u64)klocal, 0), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_wait4, 0, UNMAPPED, 0), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_wait4, 0, 0, 0), LNXRM_ECHILD);

    /* sigaction: uold is copied first (bad old -> EFAULT), NULL uact is
     * the legal query form, and SIGKILL answers EINVAL before any pointer
     * is touched at all */
    K_EXPECT_EQ(syscall_probe(SYS_sigaction, SIGUSR1, 0, (u64)klocal), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_sigaction, SIGUSR1, NONCANON, 0), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_sigaction, SIGUSR1, 0, 0), 0);
    K_EXPECT_EQ(syscall_probe(SYS_sigaction, SIGKILL, NONCANON, NONCANON), LNXRM_EINVAL);

    /* sigprocmask: same two output/input pointer answers, empty set legal */
    K_EXPECT_EQ(syscall_probe(SYS_sigprocmask, LNXRM_SIG_BLOCK, UNMAPPED, 0),
                LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_sigprocmask, LNXRM_SIG_BLOCK, 0, (u64)klocal),
                LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_sigprocmask, LNXRM_SIG_BLOCK, 0, 0), 0);

    /* past the end of the table: ENOSYS, with its one receipt line */
    K_EXPECT_EQ(syscall_probe(99, 0, 0, 0), LNXRM_ENOSYS);

    /* teardown */
    u64 back = vmm_unmap_user(current->pml4, SWAP_VA);
    K_EXPECT_EQ(back, swpa);
    pmm_free(swpa);
    K_EXPECT_EQ(vmm_translate_in(current->pml4, SWAP_VA), 0);
}

KTEST("syscall", KTEST_LATE, test_syscall_pointer_sweep);
