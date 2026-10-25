/* Late self-tests: the root filesystem, exercised through the same
 * vfs_open_file()/vfs_read_file() path user space reaches via sys_open.
 *
 * These run after vfs_try_mount_disk() and before /bin/init is spawned, so
 * a broken FAT32 driver fails the boot log instead of showing up later as a
 * user process that mysteriously cannot read its own files. */
#include <sys/ktest.h>
#include <sys/vfs.h>
#include <console.h>
#include <mm/mm.h>

/* C46: a path that does not fit the 128-byte scratch buffer abs_path()
 * normalises into is refused with ENAMETOOLONG.  Cutting it short instead
 * would hand vfs_lookup() a *shorter* path -- the kernel would open a file
 * the caller never named.  An absolute path never enters that buffer, so
 * length alone must not refuse one. */
static void test_path_too_long(void)
{
    char longp[200];
    size_t i;
    for (i = 0; i + 1 < sizeof(longp); i++) longp[i] = 'a';
    longp[sizeof(longp) - 1] = 0;

    struct file *f = NULL;
    K_EXPECT_EQ(vfs_open_file(longp, O_RDONLY, &f), (u64)LNXRM_ENAMETOOLONG);
    K_EXPECT(f == NULL);

    char longabs[200];
    longabs[0] = '/';
    for (i = 1; i + 1 < sizeof(longabs); i++) longabs[i] = 'a';
    longabs[sizeof(longabs) - 1] = 0;

    struct file *g = NULL;
    long r = vfs_open_file(longabs, O_RDONLY, &g);
    K_EXPECT(g == NULL);
    K_EXPECT(r < 0 && r != LNXRM_ENAMETOOLONG); /* named, just not present */
}

/* Matches what the Makefile writes into build/README.md -- if these two ever
 * drift apart the image, not the test, is what needs looking at.
 * 2026-10-18: "+ Rust" dropped from both sides -- the Rust component was
 * removed (uart.c port, smp.rs folded into smp.cpp, lib.rs deleted);
 * the same day smp.cpp itself became kernel/smp.c (pure-C SMP rewrite).
 * 2026-10-18 (evening): "+ C++" dropped from both sides, string 41 -> 35
 * bytes -- the C++ component reached zero files (cpp/smp.hpp deleted,
 * Makefile CXX/`.opp` rules removed with the pure-C SMP rewrite). */
#define README_TEXT "A tiny unix-like kernel in ASM + C!"

static bool root_ready(void)
{
    if (vfs_root_ready()) return true;
    ktest_skip("no root filesystem mounted");
    return false;
}

static void test_read_readme(void)
{
    if (!root_ready()) return;

    static const char line[] = README_TEXT;
    const size_t len = sizeof(line) - 1;

    struct file *f = NULL;
    K_ASSERT_EQ(vfs_open_file("/README.md", O_RDONLY, &f), 0);
    K_ASSERT(f != NULL);
    K_EXPECT(!f->is_dir);
    /* echo adds one newline, so the file is the text plus that byte */
    K_EXPECT_EQ(vfs_file_size(f), len + 1);

    char buf[64];
    memset(buf, 0xAA, sizeof(buf));
    K_EXPECT_EQ(vfs_read_file(f, buf, len + 1), (u64)(len + 1));
    K_EXPECT_EQ(memcmp(buf, line, len), 0);
    K_EXPECT_EQ(buf[len], '\n');

    /* a second read from the advanced file position must return EOF, not
     * loop over the same bytes */
    K_EXPECT_EQ(vfs_read_file(f, buf, 1), 0);
    vfs_close_file(f);

    /* a missing path must answer ENOENT -- not a zero-length file */
    struct file *g = NULL;
    K_EXPECT_EQ(vfs_open_file("/no-such-file.xyz", O_RDONLY, &g), (u64)LNXRM_ENOENT);
    K_EXPECT(g == NULL);
}

static void test_root_listing(void)
{
    if (!root_ready()) return;

    struct file *d = NULL;
    K_ASSERT_EQ(vfs_open_file("/", O_RDONLY, &d), 0);
    K_ASSERT(d != NULL);
    K_EXPECT(d->is_dir);
    K_EXPECT(d->ops->getdent != NULL);
    K_EXPECT_EQ(vfs_file_size(d), 0); /* directories are not byte streams */

    struct lnxrm_dirent ents[16];
    long n = d->ops->getdent(d, ents, sizeof(ents));
    K_ASSERT(n > 0);
    K_EXPECT_EQ(n % (long)sizeof(struct lnxrm_dirent), 0);

    bool saw_readme = false, saw_bin = false;
    long count = n / (long)sizeof(struct lnxrm_dirent);
    for (long i = 0; i < count; i++) {
        /* the name must terminate inside the field: strlen() downstream
         * (shell, sys_execve, ...) walks it as a C string */
        size_t l = 0;
        while (l < sizeof(ents[i].d_name) && ents[i].d_name[l]) l++;
        K_EXPECT(l < sizeof(ents[i].d_name));
        if (strcasecmp(ents[i].d_name, "readme.md") == 0) saw_readme = true;
        if (strcasecmp(ents[i].d_name, "bin") == 0) saw_bin = true;
    }
    K_EXPECT(saw_readme); /* the file we just read must be visible here */
    K_EXPECT(saw_bin);    /* ... and the program directory */
    vfs_close_file(d);

    /* regular files expose no getdent: the syscall layer's EBADF for
     * "not a directory" rests on that hole in the ops table */
    struct file *r = NULL;
    K_ASSERT_EQ(vfs_open_file("/README.md", O_RDONLY, &r), 0);
    K_EXPECT(r != NULL);
    K_EXPECT(r->ops->getdent == NULL);
    K_EXPECT(!r->is_dir);
    vfs_close_file(r);
}

/* C42: file_alloc() deliberately leaves `priv` NULL for a character device
 * (there is no backing vnode), so vfs_file_size() must not read a size out
 * of a NULL vnode -- /dev/console is what user space lseeks and exec's. */
static void test_char_device_size(void)
{
    struct file *c = NULL;
    K_ASSERT_EQ(vfs_open_file("/dev/console", O_RDONLY, &c), 0);
    K_ASSERT(c != NULL);
    K_EXPECT(!c->is_dir);
    K_EXPECT(c->priv == NULL); /* the exact thing that must not be deref'd */
    K_EXPECT_EQ(vfs_file_size(c), 0);
    vfs_close_file(c);
}

/* C53/C54: the capacity field is where a fabricated disk size comes from.
 * Four IDENTIFY shapes, one answer each -- the whole "2097151" bug is the
 * second row turning into the first row's value. */
static void test_ata_capacity(void)
{
    u16 id[256];

    /* plain 28-bit device: words 60-61 say it, words 100-103 are 0 */
    memset(id, 0, sizeof(id));
    id[60] = 131072 & 0xFFFF;
    id[61] = 131072 >> 16;
    K_EXPECT_EQ(ata_total_sectors(id), 131072u);

    /* ATA's "read words 100-103 instead" marker in the 28-bit field.  This is
     * the row that used to be published verbatim as num_sectors = 0xFFFFFFFF,
     * which `num_sectors / 2048` printed as 2097151. */
    memset(id, 0, sizeof(id));
    id[60] = 0xFFFF;
    id[61] = 0xFFFF;
    id[100] = 976773168 & 0xFFFF;
    id[101] = (976773168 >> 16) & 0xFFFF;
    K_EXPECT_EQ(ata_total_sectors(id), 976773168ull);

    /* marker with no 48-bit data behind it: unknown, not 2 TiB.
     * 0 must come back so the caller declines to register the device. */
    memset(id, 0, sizeof(id));
    id[60] = 0xFFFF;
    id[61] = 0xFFFF;
    K_EXPECT_EQ(ata_total_sectors(id), 0u);

    /* >= 2 TiB saturates the 28-bit field but not the 48-bit one */
    memset(id, 0, sizeof(id));
    id[60] = 0xFFFF;
    id[61] = 0xFFFF;
    id[100] = 0x0000;
    id[101] = 0x0000;
    id[102] = 0x0004; /* bits 47:32 -> sector 0x4_0000_0000 = 2^34 */
    K_EXPECT_EQ(ata_total_sectors(id), 0x400000000ull);
}

/* Fill the boot volume until it answers ENOSPC, then prove the failure is
 * clean: the write reports -28 instead of a swallowed short count, the
 * file still unlinks, and one write succeeds again afterwards -- space
 * that does not come back is a leak one write away from a volume that
 * never recovers.  Guarded to the real FAT32 root: a ram-backed
 * initramfs has no bottom to hit (vfs_root_is_fat()).  No K_ASSERT before
 * the unlink: a mid-case abort would leave the volume full for every
 * later smoke step. (2026-10-20) */
#define ENSPC_CHUNK (16 * 1024)
/* 64 MiB of attempts: past that, skip, do not fail.  A 16 KiB payload is
 * sized so this case never *allocates* while the volume is under test --
 * a heap buffer handed back to the block layer mid-fill would have the
 * allocator racing the very ENOSPC it is measuring. */
#define ENSPC_MAX 4096

static void test_enospc_on_full_volume(void)
{
    if (!vfs_root_ready()) {
        ktest_skip("no root filesystem");
        return;
    }
    if (!vfs_root_is_fat()) {
        ktest_skip("root is not the FAT32 volume");
        return;
    }

    /* kmalloc, not a static buffer: a 16 KiB array in every ktest object
     * is .bss the linker does not account for when it places the heap. */
    u8 *buf = kmalloc(ENSPC_CHUNK);
    if (!buf) {
        ktest_skip("no memory for the fill buffer");
        return;
    }

    struct file *f = NULL;
    long r = vfs_open_file("/ENOSPC.T", O_CREAT | 1 /* O_WRONLY */ | O_TRUNC, &f);
    K_EXPECT_EQ(r, 0);
    if (r != 0 || !f) return;

    long hit = 0, bad = 0, nwrite = 0;
    for (int i = 0; i < ENSPC_MAX; i++) {
        long w = f->ops->write(f, buf, ENSPC_CHUNK);
        if (w == LNXRM_ENOSPC) { hit = 1; break; }
        if (w != ENSPC_CHUNK) { bad = w; break; }
        nwrite++;
    }
    kprintf("[t-enspc] wrote=%ld hit=%d bad=%ld\n", nwrite, (int)hit, bad);
    vfs_close_file(f);
    kfree(buf);

    /* remove first, judge after: the assertions must not run before the
     * volume has been given back its space */
    long un = sys_unlink("/ENOSPC.T");

    K_EXPECT_EQ(bad, 0); /* no short count, no surprise errno */
    if (!hit) {
        /* a volume larger than the attempt cap is not a failure */
        K_EXPECT_EQ(un, 0);
        ktest_skip("volume did not fill within the attempt cap");
        return;
    }
    K_EXPECT(nwrite > 100); /* it really filled a volume, not one cluster */
    K_EXPECT_EQ(un, 0);

    /* space is back: one more file writes normally and removes again */
    buf = kmalloc(ENSPC_CHUNK);
    if (!buf) {
        ktest_skip("no memory for the recovery buffer");
        return;
    }
    struct file *g = NULL;
    r = vfs_open_file("/ENOSPC.T", O_CREAT | 1 | O_TRUNC, &g);
    K_EXPECT_EQ(r, 0);
    if (r == 0 && g) {
        long w2 = g->ops->write(g, buf, ENSPC_CHUNK);
        K_EXPECT_EQ(w2, ENSPC_CHUNK);
        vfs_close_file(g);
    }
    kfree(buf);
    K_EXPECT_EQ(sys_unlink("/ENOSPC.T"), 0);
}

/* C40: a regular file's priv is a kmalloc'd vnode copy; a close that freed
 * nothing lost it forever, one open at a time.  The allocator is the
 * assertion: a warmed open/close pair must settle back to exactly the
 * handed-out total it started from.  The first pair warms any one-time
 * lookup caches the path resolution keeps, so the measured pair is pure
 * file lifetime. */
static void test_open_close_leaks_nothing(void)
{
    struct file *w = NULL;
    K_EXPECT_EQ(vfs_open_file("/README.md", O_RDONLY, &w), 0); /* warm */
    if (w) vfs_close_file(w);

    u64 before = kheap_used_bytes();
    struct file *f = NULL;
    K_ASSERT_EQ(vfs_open_file("/README.md", O_RDONLY, &f), 0);
    K_ASSERT(f != NULL);
    u64 opened = kheap_used_bytes();
    vfs_close_file(f);

    K_EXPECT(opened > before);              /* the copy was really handed out */
    K_EXPECT_EQ(kheap_used_bytes(), before); /* and came back, byte for byte */
}

/* C41: access modes are enforced and O_APPEND lands at the fs's current
 * end.  The append check is differential: an ignored O_APPEND would have
 * written "B\n" over "A\n" at pos 0, so the readback must fail. */
static void test_access_mode_and_append(void)
{
    if (!vfs_root_ready()) {
        ktest_skip("no root filesystem");
        return;
    }
    u8 buf[16];

    /* O_RDONLY: reads pass, writes refuse */
    struct file *ro = NULL;
    K_ASSERT_EQ(vfs_open_file("/README.md", O_RDONLY, &ro), 0);
    K_ASSERT(ro != NULL);
    K_EXPECT(ro->ops->read(ro, buf, sizeof(buf)) > 0);
    K_EXPECT_EQ(ro->ops->write(ro, "x", 1), LNXRM_EBADF);
    vfs_close_file(ro);

    /* O_WRONLY: writes pass, reads refuse */
    struct file *wo = NULL;
    long r = vfs_open_file("/C41AP.T", O_CREAT | O_WRONLY | O_TRUNC, &wo);
    K_EXPECT_EQ(r, 0);
    if (r == 0 && wo) {
        K_EXPECT_EQ(wo->ops->read(wo, buf, sizeof(buf)), LNXRM_EBADF);
        K_EXPECT_EQ(wo->ops->write(wo, "A\n", 2), 2);
        vfs_close_file(wo);
    }

    /* O_APPEND on its own handle: pos poked to zero, the write still lands
     * at the current end -- differential: an ignored O_APPEND would have
     * overwritten "A\n" and left the file "B\n" */
    struct file *ap = NULL;
    r = vfs_open_file("/C41AP.T", O_APPEND | O_WRONLY, &ap);
    K_EXPECT_EQ(r, 0);
    if (r == 0 && ap) {
        ap->pos = 0;
        K_EXPECT_EQ(ap->ops->write(ap, "B\n", 2), 2);
        vfs_close_file(ap);
    }

    /* O_RDWR: both directions; a plain write honours pos (end after read) */
    struct file *rw = NULL;
    r = vfs_open_file("/C41AP.T", O_RDWR, &rw);
    K_EXPECT_EQ(r, 0);
    if (r == 0 && rw) {
        memset(buf, 0, sizeof(buf));
        K_EXPECT_EQ(rw->ops->read(rw, buf, sizeof(buf)), 4);
        K_EXPECT(memcmp(buf, "A\nB\n", 4) == 0); /* O_APPEND did its job */
        K_EXPECT_EQ(rw->ops->write(rw, "C\n", 2), 2);
        vfs_close_file(rw);
    }

    struct file *rd = NULL;
    if (vfs_open_file("/C41AP.T", O_RDONLY, &rd) == 0 && rd) {
        memset(buf, 0, sizeof(buf));
        K_EXPECT_EQ(rd->ops->read(rd, buf, sizeof(buf)), 6);
        K_EXPECT(memcmp(buf, "A\nB\nC\n", 6) == 0);
        vfs_close_file(rd);
    }
    K_EXPECT_EQ(sys_unlink("/C41AP.T"), 0);
}

/* C44: a getdent buffer that cannot hold one record refuses with EINVAL --
 * the zero it used to answer would keep a data-hungry caller looping. */
static void test_getdent_small_buffer_refused(void)
{
    struct file *d = NULL;
    K_ASSERT_EQ(vfs_open_file("/bin", O_RDONLY, &d), 0);
    K_ASSERT(d != NULL && d->is_dir);
    u8 buf[72];
    K_EXPECT_EQ(d->ops->getdent(d, buf, 0), LNXRM_EINVAL);
    K_EXPECT_EQ(d->ops->getdent(d, buf, 1), LNXRM_EINVAL);
    K_EXPECT_EQ(d->ops->getdent(d, buf, 71), LNXRM_EINVAL);
    K_EXPECT(d->ops->getdent(d, buf, sizeof(buf)) > 0); /* a record fits */
    vfs_close_file(d);
}

KTEST("vfs", KTEST_LATE, test_ata_capacity);
KTEST("vfs", KTEST_LATE, test_read_readme);
KTEST("vfs", KTEST_LATE, test_root_listing);
KTEST("vfs", KTEST_LATE, test_char_device_size);
KTEST("vfs", KTEST_LATE, test_path_too_long);
KTEST("vfs", KTEST_LATE, test_enospc_on_full_volume);
KTEST("vfs", KTEST_LATE, test_open_close_leaks_nothing);
KTEST("vfs", KTEST_LATE, test_access_mode_and_append);
KTEST("vfs", KTEST_LATE, test_getdent_small_buffer_refused);
