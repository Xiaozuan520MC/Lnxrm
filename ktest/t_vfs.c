/* Late self-tests: the root filesystem, exercised through the same
 * vfs_open_file()/vfs_read_file() path user space reaches via sys_open.
 *
 * These run after vfs_try_mount_disk() and before /bin/init is spawned, so
 * a broken FAT32 driver fails the boot log instead of showing up later as a
 * user process that mysteriously cannot read its own files. */
#include <sys/ktest.h>
#include <sys/vfs.h>
#include <console.h>

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
 * drift apart the image, not the test, is what needs looking at. */
#define README_TEXT "A tiny unix-like kernel in ASM + C + C++ + Rust!"

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

KTEST("vfs", KTEST_LATE, test_ata_capacity);
KTEST("vfs", KTEST_LATE, test_read_readme);
KTEST("vfs", KTEST_LATE, test_root_listing);
KTEST("vfs", KTEST_LATE, test_char_device_size);
KTEST("vfs", KTEST_LATE, test_path_too_long);
