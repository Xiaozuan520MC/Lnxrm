/* stat(45) and gettimeofday(46) -- the two calls that landed 2026-11-11.
 *
 * stat is the first door into a file's metadata that hands out no
 * descriptor, so the claims worth pinning are the ones an open-then-fstat
 * sequence could not make: that it answers WITHOUT touching the volume
 * (a missing path stays missing -- no O_CREAT creeping in from the open
 * path it shares), that the size it reports is the size the filesystem has
 * right now rather than the one some earlier lookup snapshotted, and that
 * the identity numbers match what getdent already reports for the same
 * record.  Two layers are checked separately on purpose: vfs_stat_path()
 * with kernel strings for the filesystem semantics, and syscall_probe()
 * through user pages for the dispatcher's half -- the string gate and the
 * copy-out gate, which are the parts a direct call would skip.
 *
 * gettimeofday is checked as a CLOCK first and a syscall second: the
 * calendar conversion has known answers a self-test can assert exactly
 * (1970-01-01 is day 0), the returned instant must be a plausible date and
 * must never go backwards across two calls, and the timezone half must
 * come back zeroed -- this kernel keeps no DST rule and says so rather
 * than inventing one.  The log line at the end prints the instant as
 * UTC, which is how a human confirms at a glance that the guest's clock
 * agrees with the host's.
 *
 * Deliberately NOT probed here: writing the clock (there is no setter --
 * the CMOS is the authority and nothing in this kernel changes it), and
 * any path that would need a second mounted volume (that lives in
 * t_vfs.c's mount suite, which owns the fixture). */
#include <sys/ktest.h>
#include <console.h>
#include <mm/mm.h>
#include <sys/sched.h>
#include <sys/cpu.h>
#include <sys/vfs.h>
#include <sys/rtc.h>
#include <string.h>

/* One page for the paths the dispatcher must read, one for everything it
 * must write back.  0xB000/0xD000 are the two slots no other ktest file
 * claims (0xA000 kbd, 0xC000 is everyone's kept-unmapped EFAULT probe,
 * 0xE000 mouse, 0xF000 input). */
#define PATH_VA (USER_BASE + 0xB000)
#define OUT_VA  (USER_BASE + 0xD000)
#define BAD_VA  (USER_BASE + 0xC000)

static u64 path_pa, out_pa;

/* Ring 0 touching a user page has to walk around SMAP exactly the way
 * t_syscall.c does it: enter, memcpy, leave.  (The physical alias would
 * work too; this keeps the access at the address the CALLER named, which
 * is what is actually under test.) */
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

static void map_page(u64 va, u64 *pa)
{
    *pa = pmm_alloc();
    K_ASSERT(*pa != 0);
    K_ASSERT_EQ(vmm_map_user(current->pml4, va, *pa, true, true, false), 0);
}
static void unmap_page(u64 va, u64 *pa)
{
    K_EXPECT_EQ(vmm_unmap_user(current->pml4, va), *pa);
    pmm_free(*pa);
    *pa = 0;
    K_EXPECT_EQ(vmm_translate_in(current->pml4, va), 0);
}

/* ---- filesystem semantics, called with kernel strings ---- */
static void test_stat_paths(void)
{
    struct vfs_attr a;

    if (!vfs_root_ready()) {
        ktest_skip("no root filesystem");
        return;
    }

    /* a file the volume is known to carry: regular, non-empty, with an
     * identity (FAT32 packs where its record sits; initramfs numbers its
     * archive slots) and a mount slot behind it */
    K_EXPECT_EQ(vfs_stat_path("/README.md", &a), 0);
    K_EXPECT_EQ(a.type, V_REG);
    K_EXPECT(a.size > 0);
    K_EXPECT(a.ino != 0);
    K_EXPECT(a.dev != 0); /* 1..MAX_MOUNTS: a real volume, not "no volume" */

    /* the mount root: a directory with no byte length and no record */
    K_EXPECT_EQ(vfs_stat_path("/", &a), 0);
    K_EXPECT_EQ(a.type, V_DIR);
    K_EXPECT_EQ(a.size, 0);
    K_EXPECT(a.dev != 0);

    /* the synthesised device node: no volume (dev 0), the same id
     * dev_getdent() hands out for it, and never read-only */
    K_EXPECT_EQ(vfs_stat_path("/dev/console", &a), 0);
    K_EXPECT_EQ(a.type, V_CHR);
    K_EXPECT_EQ(a.ino, 1);
    K_EXPECT_EQ(a.dev, 0);
    K_EXPECT_EQ(a.size, 0);
    K_EXPECT(!a.ro);

    /* absent is absent, and asking must not be how a file comes into
     * being: stat shares vfs_lookup with open(), whose O_CREAT arm is
     * unreachable from here because the flags are 0 */
    K_EXPECT_EQ(vfs_stat_path("/NOSUCH.T", &a), LNXRM_ENOENT);
    K_EXPECT_EQ(vfs_stat_path("/SSTAT.T", &a), LNXRM_ENOENT);
    K_EXPECT(!vfs_file_exists("/SSTAT.T"));

    /* the size answer must be the filesystem's OWN current number, not
     * the vn->size snapshot a lookup took: write a file, close it, and
     * stat must see exactly what was written (and 0 again once the same
     * open path truncates it).  Both halves matter -- a stale size would
     * lie to a `cp` before it ever read a byte. */
    long fd = sys_open("/SSTAT.T", O_CREAT | O_RDWR | O_TRUNC);
    K_ASSERT(fd >= 0);
    char payload[100];
    memset(payload, 's', sizeof(payload));
    K_ASSERT_EQ(vfs_pwrite(current->fds[fd], payload, sizeof(payload), 0),
                (long)sizeof(payload));
    K_EXPECT_EQ(sys_close(fd), 0);
    K_EXPECT_EQ(vfs_stat_path("/SSTAT.T", &a), 0);
    K_EXPECT_EQ(a.size, (u64)sizeof(payload));
    K_EXPECT_EQ(a.type, V_REG);

    fd = sys_open("/SSTAT.T", O_CREAT | O_RDWR | O_TRUNC);
    K_ASSERT(fd >= 0);
    K_EXPECT_EQ(sys_close(fd), 0);
    K_EXPECT_EQ(vfs_stat_path("/SSTAT.T", &a), 0);
    K_EXPECT_EQ(a.size, 0); /* truncation is visible to the next stat */

    K_EXPECT_EQ(sys_unlink("/SSTAT.T"), 0);
    K_EXPECT(!vfs_file_exists("/SSTAT.T"));
    K_EXPECT_EQ(vfs_stat_path("/SSTAT.T", &a), LNXRM_ENOENT);
}

/* ---- the dispatcher's half: string gate and copy-out gate ---- */
static void test_stat_dispatch(void)
{
    struct lnxrm_stat st;

    if (!vfs_root_ready()) {
        ktest_skip("no root filesystem");
        return;
    }

    map_page(PATH_VA, &path_pa);
    map_page(OUT_VA, &out_pa);

    /* the happy path: a user-side path in, a user-side struct out */
    uput(PATH_VA, "/README.md", 12);
    K_EXPECT_EQ(syscall_probe(SYS_stat, PATH_VA, OUT_VA, 0), 0);
    uget(&st, OUT_VA, sizeof(st));
    K_EXPECT_EQ(st.st_mode & LNXRM_S_IFMT, LNXRM_S_IFREG);
    K_EXPECT_EQ(st.st_mode & 0777, 0644); /* a writable regular file */
    K_EXPECT(st.st_size > 0);
    K_EXPECT_EQ(st.st_nlink, 1);          /* no hard links exist here */
    K_EXPECT_EQ(st.st_blksize, 512);      /* one sector */
    K_EXPECT_EQ(st.st_blocks, (st.st_size + 511) / 512);
    K_EXPECT(st.st_ino != 0);
    K_EXPECT_EQ(st.st_uid, 0);            /* no per-file owner yet */
    K_EXPECT_EQ(st.st_gid, 0);

    /* a directory reports its type and no size */
    uput(PATH_VA, "/", 2);
    K_EXPECT_EQ(syscall_probe(SYS_stat, PATH_VA, OUT_VA, 0), 0);
    uget(&st, OUT_VA, sizeof(st));
    K_EXPECT_EQ(st.st_mode & LNXRM_S_IFMT, LNXRM_S_IFDIR);
    K_EXPECT_EQ(st.st_size, 0);

    /* a character device: the synthesised node, 0666, ino 1, dev 0 */
    uput(PATH_VA, "/dev/console", 13);
    K_EXPECT_EQ(syscall_probe(SYS_stat, PATH_VA, OUT_VA, 0), 0);
    uget(&st, OUT_VA, sizeof(st));
    K_EXPECT_EQ(st.st_mode & LNXRM_S_IFMT, LNXRM_S_IFCHR);
    K_EXPECT_EQ(st.st_mode & 0777, 0666);
    K_EXPECT_EQ(st.st_ino, 1);
    K_EXPECT_EQ(st.st_dev, 0);

    /* ENOENT, twice: the second call proves the first did not create it */
    uput(PATH_VA, "/NOSUCH.T", 10);
    K_EXPECT_EQ(syscall_probe(SYS_stat, PATH_VA, OUT_VA, 0), LNXRM_ENOENT);
    K_EXPECT_EQ(syscall_probe(SYS_stat, PATH_VA, OUT_VA, 0), LNXRM_ENOENT);
    K_EXPECT(!vfs_file_exists("/NOSUCH.T"));

    /* Put a path that RESOLVES back before the buffer gates: otherwise
     * every refusal below would be answered by the path half first (ENOENT
     * for /NOSUCH.T), which proves nothing about the copy-out gate -- the
     * two must be walked in this order to be tested at all. */
    uput(PATH_VA, "/README.md", 12);

    /* every refusal, each from a gate the other path does not run */
    K_EXPECT_EQ(syscall_probe(SYS_stat, PATH_VA, 0, 0), LNXRM_EFAULT);   /* NULL buffer */
    K_EXPECT_EQ(syscall_probe(SYS_stat, 0, OUT_VA, 0), LNXRM_EFAULT);    /* NULL path */
    K_EXPECT_EQ(syscall_probe(SYS_stat, (u64)"/README.md", OUT_VA, 0),
                LNXRM_EFAULT); /* kernel .rodata is not a user pointer */
    K_EXPECT_EQ(syscall_probe(SYS_stat, PATH_VA, BAD_VA, 0), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_stat, PATH_VA, (u64)&st, 0), LNXRM_EFAULT);

    /* one past the table is still ENOSYS -- 45 and 46 did not eat it */
    K_EXPECT_EQ(syscall_probe(LNXRM_SYSCALL_LAST + 1, 0, 0, 0), LNXRM_ENOSYS);

    unmap_page(PATH_VA, &path_pa);
    unmap_page(OUT_VA, &out_pa);
}

/* ---- the clock itself, then the syscall that reports it ---- */
static void test_wall_clock(void)
{
    struct rtc_utc r;
    struct lnxrm_timeval tv, tv2;
    struct lnxrm_timezone tz;

    /* the conversion has answers a test can state exactly: epoch day 0,
     * a date 30 years later that crosses eight leap days and the leap
     * century (2000 was leap, 1900 was not -- era arithmetic, not
     * trial division, is what keeps both right) */
    K_EXPECT_EQ(rtc_days_from_civil(1970, 1, 1), 0);
    K_EXPECT_EQ(rtc_days_from_civil(1970, 1, 2), 1);
    K_EXPECT_EQ(rtc_days_from_civil(2000, 3, 1), 11017);
    K_EXPECT_EQ(rtc_days_from_civil(1980, 1, 1), 3652); /* 10 y = 3650 + 2 leap */

    /* and its inverse, through the formatter */
    char buf[24];
    rtc_format(0, buf, sizeof(buf));
    K_EXPECT_EQ(strcmp(buf, "1970-01-01T00:00:00Z"), 0);
    rtc_format(86399, buf, sizeof(buf));
    K_EXPECT_EQ(strcmp(buf, "1970-01-01T23:59:59Z"), 0);
    rtc_format((i64)11017 * 86400 + 45296, buf, sizeof(buf)); /* 12:34:56 */
    K_EXPECT_EQ(strcmp(buf, "2000-03-01T12:34:56Z"), 0);

    /* the RTC answers with a date in this machine's lifetime: QEMU keeps
     * CMOS time from the host, and a reading outside these bounds means
     * the BCD/century decode went wrong somewhere, not that time travel
     * happened */
    K_EXPECT(rtc_utc_now(&r));
    K_EXPECT(r.year >= 2020 && r.year <= 2099);
    K_EXPECT(r.mon >= 1 && r.mon <= 12);
    K_EXPECT(r.day >= 1 && r.day <= 31);
    K_EXPECT(r.hour >= 0 && r.hour <= 23);
    K_EXPECT(r.min >= 0 && r.min <= 59);
    K_EXPECT(r.sec >= 0 && r.sec <= 59);

    map_page(OUT_VA, &out_pa);

    /* the syscall's answer: a plausible instant, a legal fraction, and
     * the same second the raw reading gave (the sample IS that reading,
     * so agreeing within a minute is agreement -- and anything looser
     * would let a mis-scaled century byte through) */
    K_EXPECT_EQ(syscall_probe(SYS_gettimeofday, OUT_VA, 0, 0), 0);
    uget(&tv, OUT_VA, sizeof(tv));
    K_EXPECT(tv.tv_sec >= 1577836800LL && tv.tv_sec < 4102444800LL); /* 2020..2100 */
    K_EXPECT(tv.tv_usec >= 0 && tv.tv_usec < 1000000);
    i64 rtc_epoch = rtc_days_from_civil(r.year, r.mon, r.day) * 86400 +
                    r.hour * 3600 + r.min * 60 + r.sec;
    i64 skew = tv.tv_sec - rtc_epoch;
    if (skew < 0) skew = -skew;
    K_EXPECT(skew < 60);

    /* monotonic: a later answer is never an earlier one -- this is what
     * sampling the RTC once and advancing with the TSC buys, and what a
     * per-call register read (which can catch an update mid-flight) does
     * not.
     *
     * The gap between the two calls is a counted pause loop on purpose.
     * Waiting on `jiffies` here would wait forever: this stage of the
     * self-test runs BEFORE the LAPIC timer starts ticking, so jiffies is
     * still 0 -- which is also why wall_clock() has to fall back to it
     * only after the TSC rate is known (it is not yet, at this point) and
     * the two answers below may legitimately be identical.  The assertion
     * is "never smaller", never "bigger"; the pause just makes "bigger"
     * the likely outcome so the floor logic gets exercised. */
    for (volatile int i = 0; i < 8000000; i++) __asm__ volatile("pause");
    K_EXPECT_EQ(syscall_probe(SYS_gettimeofday, OUT_VA, 0, 0), 0);
    uget(&tv2, OUT_VA, sizeof(tv2));
    K_EXPECT(tv2.tv_sec > tv.tv_sec ||
             (tv2.tv_sec == tv.tv_sec && tv2.tv_usec >= tv.tv_usec));

    /* the timezone half: written, and zeroed (no DST rule exists here,
     * and the kernel says so instead of guessing) */
    memset(&tz, 0xAA, sizeof(tz));
    uput(OUT_VA + 64, &tz, sizeof(tz));
    K_EXPECT_EQ(syscall_probe(SYS_gettimeofday, OUT_VA, OUT_VA + 64, 0), 0);
    uget(&tz, OUT_VA + 64, sizeof(tz));
    K_EXPECT_EQ(tz.tz_minuteswest, 0);
    K_EXPECT_EQ(tz.tz_dsttime, 0);

    /* tv == NULL with a timezone is legal (ask only for the zone), but
     * both NULL asks for nothing and is refused rather than ignored */
    K_EXPECT_EQ(syscall_probe(SYS_gettimeofday, 0, OUT_VA + 64, 0), 0);
    K_EXPECT_EQ(syscall_probe(SYS_gettimeofday, 0, 0, 0), LNXRM_EINVAL);

    /* a kernel address never reaches the clock: EFAULT, nothing written */
    K_EXPECT_EQ(syscall_probe(SYS_gettimeofday, (u64)&tv, 0, 0), LNXRM_EFAULT);

    /* the line a human checks: guest clock against host clock */
    rtc_format(tv.tv_sec, buf, sizeof(buf));
    kprintf("[rtc] now=%s (%ld.%06ld s since 1970)\n", buf, (long)tv.tv_sec,
            (long)tv.tv_usec);

    unmap_page(OUT_VA, &out_pa);
}

KTEST("stat", KTEST_LATE, test_stat_paths);
KTEST("stat", KTEST_LATE, test_stat_dispatch);
KTEST("stat", KTEST_LATE, test_wall_clock);
