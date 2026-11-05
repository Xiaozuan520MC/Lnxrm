/* Late self-tests: the root filesystem, exercised through the same
 * vfs_open_file()/vfs_read_file() path user space reaches via sys_open.
 *
 * These run after vfs_try_mount_disk() and before /bin/init is spawned, so
 * a broken FAT32 driver fails the boot log instead of showing up later as a
 * user process that mysteriously cannot read its own files. */
#include <sys/ktest.h>
#include <sys/vfs.h>
#include <console.h>
#include <disk.h>
#include <fat32.h>
#include <mm/mm.h>
#include <string.h>
#include "kblk.h"

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
 * the same day smp.cpp itself became kernel/core/smp.c (pure-C SMP rewrite).
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

/* C43: dropping a reference must not be able to run the count below zero,
 * and a drop that arrives with nothing left to drop must leave the file
 * alone instead of freeing it out from under whoever still holds it. */
static void test_close_saturates_the_refcount(void)
{
    struct file *f = NULL;
    K_ASSERT_EQ(vfs_open_file("/README.md", O_RDONLY, &f), 0);
    K_ASSERT(f != NULL);
    K_EXPECT_EQ(f->refcnt, 1);

    /* two holders: the first close is not the last one */
    f->refcnt = 2;
    vfs_close_file(f);
    K_EXPECT_EQ(f->refcnt, 1);
    char buf[8];
    K_EXPECT_EQ(vfs_read_file(f, buf, sizeof(buf)), (u64)sizeof(buf));

    /* nothing left to drop: refused, not underflowed, and not freed --
     * the reads below only hold if the object is still there */
    f->refcnt = 0;
    vfs_close_file(f);
    K_EXPECT_EQ(f->refcnt, 0);
    K_EXPECT(f->ops != NULL && f->ops->read != NULL);
    K_EXPECT_EQ(vfs_read_file(f, buf, sizeof(buf)), (u64)sizeof(buf));

    /* the same for a count already driven negative */
    f->refcnt = -3;
    vfs_close_file(f);
    K_EXPECT_EQ(f->refcnt, -3);

    /* and the real last drop still frees exactly once */
    f->refcnt = 1;
    vfs_close_file(f);
}

/* C39: a node carries the mount it was resolved through, and the freespace
 * hook has a caller.  Both used to be placeholders -- vnode.mnt_data was
 * (void*)1 and nothing ever called ops->freespace -- which is what made the
 * whole mount layer decorative. */
static void test_mount_data_reaches_the_node(void)
{
    if (!root_ready()) return;

    struct file *f = NULL;
    K_ASSERT_EQ(vfs_open_file("/README.md", O_RDONLY, &f), 0);
    K_ASSERT(f != NULL && !f->is_dir);
    struct vnode *vn = f->priv;
    K_ASSERT(vn != NULL);
    K_EXPECT(vn->ops != NULL);
    /* the whole point: not a placeholder, and specifically not the old
     * (void*)1 sentinel */
    K_EXPECT(vn->mnt_data != NULL);
    K_EXPECT(vn->mnt_data != (void *)1);
    if (vfs_root_is_fat()) K_EXPECT_EQ((u64)vn->mnt_data, (u64)fat_priv);
    vfs_close_file(f);

    /* a directory's iterator carries the same mount, since file_alloc() has
     * to hand the ops layer something to call the volume with */
    struct file *d = NULL;
    K_ASSERT_EQ(vfs_open_file("/", O_RDONLY, &d), 0);
    K_ASSERT(d != NULL && d->is_dir);
    struct dir_iter *it = d->priv;
    K_EXPECT(it != NULL);
    K_EXPECT(it->mnt_data != NULL && it->mnt_data != (void *)1);
    if (vfs_root_is_fat()) K_EXPECT_EQ((u64)it->mnt_data, (u64)fat_priv);
    vfs_close_file(d);

    /* vfs_free_space() routes a path to its mount's hook and answers with
     * bytes: the same number the volume's own ops entry gives when asked
     * directly, and never an error code for a path that is mounted. */
    long via_path = vfs_free_space("/");
    K_EXPECT(via_path >= 0);
    if (vfs_root_is_fat())
        K_EXPECT_EQ((u64)via_path, fat32_ops.freespace(fat_priv));
    else
        K_EXPECT_EQ(via_path, 0); /* a read-only archive has no free space */
}

/* C39/C47: two mounts of the SAME ops table, each with its own volume.
 * Only that shape can falsify "the mount travels with the path": with one
 * volume, routing could hand back any priv at all and every answer would
 * still look right.  So a second FAT32 volume is forged, mounted at "/alt",
 * and both roots are asked to list themselves -- the answer each gives is
 * the volume routing actually picked. */
static void test_second_mount_routes_to_its_own_volume(void)
{
    if (!root_ready() || !vfs_root_is_fat()) {
        ktest_skip("no FAT32 root to bind a second mount point to");
        return;
    }

    /* before it exists, "/alt" is just a name under the root mount */
    struct file *pre = NULL;
    K_EXPECT_EQ(vfs_open_file("/alt", O_RDONLY, &pre), (u64)LNXRM_ENOENT);
    K_EXPECT(pre == NULL);
    struct file *pref = NULL;
    K_EXPECT_EQ(vfs_open_file("/alt/README.md", O_RDONLY, &pref), (u64)LNXRM_ENOENT);
    K_EXPECT(pref == NULL);

    struct kblk *k = kblk_make("kmntalt", 256, 16);
    K_ASSERT(k != NULL);
    u8 *s0 = kblk_sector(k, 0);
    K_ASSERT(s0 != NULL);
    kblk_forge_fat32_bpb(s0, 1, 8, 2, 1 /* FAT = 512 B = 128 cells */, 256, 2);

    /* fat_mount() publishes its result into fat_priv, which belongs to the
     * real root volume -- park it and put it straight back, or every case
     * behind this one would be reading the forged disk. */
    struct fat_mount *active = fat_priv;
    struct fat_mount *m2 = fat_mount(&k->dev);
    fat_priv = active;
    K_ASSERT(m2 != NULL); /* leaked (m2 + kblk) rather than left mounted */

    K_ASSERT_EQ(vfs_mount_fs(&fat32_ops, m2, "/alt"), 0);

    struct file *a = NULL, *r = NULL;
    K_EXPECT_EQ(vfs_open_file("/alt", O_RDONLY, &a), 0);
    K_EXPECT_EQ(vfs_open_file("/", O_RDONLY, &r), 0);
    if (a && r) {
        struct dir_iter *ia = a->priv, *ir = r->priv;
        K_EXPECT(a->is_dir && r->is_dir);
        /* C39: which mount each iterator belongs to */
        K_EXPECT_EQ((u64)ia->mnt_data, (u64)m2);
        K_EXPECT_EQ((u64)ir->mnt_data, (u64)fat_priv);
        K_EXPECT(ia->mnt_data != ir->mnt_data);

        /* ... and the answers come from those volumes, not from whichever
         * one the ops table happened to name */
        char bufa[256], bufr[256];
        memset(bufa, 0, sizeof(bufa));
        memset(bufr, 0, sizeof(bufr));
        K_EXPECT_EQ(a->ops->getdent(a, bufa, sizeof(bufa)), 0); /* forged root: empty */
        K_EXPECT(r->ops->getdent(r, bufr, sizeof(bufr)) > 0);   /* real root: has names */
    }
    if (a) vfs_close_file(a);
    if (r) vfs_close_file(r);

    /* C47: taking it back out of the table removes the route, and the root
     * mount underneath is neither disturbed nor removed with it. */
    K_EXPECT_EQ(vfs_umount("/alt"), 0);

    struct file *post = NULL;
    K_EXPECT_EQ(vfs_open_file("/alt/README.md", O_RDONLY, &post), (u64)LNXRM_ENOENT);
    K_EXPECT(post == NULL);
    K_EXPECT_EQ(vfs_umount("/alt"), (u64)LNXRM_ENOENT); /* not mounted: says so */

    struct file *ok = NULL;
    K_EXPECT_EQ(vfs_open_file("/README.md", O_RDONLY, &ok), 0);
    if (ok) vfs_close_file(ok);

    fat_pages_free(m2);
    kfree(m2);
    /* drop the forged volume's cache lines before the device goes away, so
     * a later writeback cannot find a pointer to freed memory */
    blk_cache_purge(&k->dev);
    kblk_free(k);
}

/* C51: a disk that carries no /bin/init is data, not root -- and the only
 * honest way to ask that question is with "/" free, so the real root goes
 * down first and the very same volume comes back afterwards (C47: umount
 * deactivates a mount, it never releases it).  The forged volume is both
 * empty and init-less, so placement that put it at "/", or that displaced
 * the root with it, shows up in the return code and in what each path then
 * answers.  The other half of the rule -- a volume WITH /bin/init takes "/"
 * -- is what every boot does, and what the smoke log's
 * "[vfs] FAT32 mounted at /" line records. */
static void test_a_disk_without_init_is_placed_below_root(void)
{
    if (!root_ready() || !vfs_root_is_fat()) {
        ktest_skip("no FAT32 root to take down");
        return;
    }

    /* The root's identity is fetched before anything is unmounted, so a
     * later failure still leaves a way back to it. */
    struct fs_ops *rops;
    void *rpriv;
    K_ASSERT_EQ(vfs_root_mount(&rops, &rpriv), 0);

    /* Forge the data volume BEFORE touching "/": a failed allocation must
     * not be able to leave the machine with no root at all. */
    struct kblk *k = kblk_make("kmntdata", 256, 16);
    K_ASSERT(k != NULL);
    u8 *s0 = kblk_sector(k, 0);
    K_ASSERT(s0 != NULL);
    kblk_forge_fat32_bpb(s0, 1, 8, 2, 1 /* FAT = 512 B = 128 cells */, 256, 2);

    /* "/" free -- the state the rule exists for */
    K_EXPECT_EQ(vfs_umount("/"), 0);
    K_EXPECT(!vfs_root_ready()); /* derived from the table, not remembered */

    void *m = NULL;
    int rc = vfs_probe_home(&k->dev, &m);
    K_EXPECT_EQ(rc, 1);        /* a data prefix, not the root slot */
    K_EXPECT(m != NULL);
    K_EXPECT(!vfs_root_ready());  /* an init-less volume took "/" anyway? */
    K_EXPECT(vfs_data_volume_ready());   /* ...it did not: it is a data volume */
    K_EXPECT(vfs_free_space("/mnt") >= 0); /* and it answers under /mnt */

    /* Root back: same ops, same volume, no second mount of "/" created */
    if (rc == 0) vfs_umount("/"); /* bug path: give the slot back first */
    K_EXPECT_EQ(vfs_mount_fs(rops, rpriv, "/"), 0);
    K_EXPECT(vfs_root_ready());

    /* Both volumes are mounted now, so routing can be told apart by what
     * each path answers: the root is the real disk, /mnt the empty forge. */
    struct file *r = NULL;
    K_EXPECT_EQ(vfs_open_file("/README.md", O_RDONLY, &r), 0);
    if (r) vfs_close_file(r);
    struct file *d = NULL;
    K_EXPECT_EQ(vfs_open_file("/mnt/README.md", O_RDONLY, &d), (u64)LNXRM_ENOENT);
    K_EXPECT(d == NULL);
    K_EXPECT(vfs_free_space("/mnt") < vfs_free_space("/"));

    /* Take the data volume back out: the root stays, and so does the answer
     * to "is anything mounted below /". */
    K_EXPECT_EQ(vfs_umount("/mnt"), 0);
    K_EXPECT(vfs_root_ready());
    K_EXPECT(!vfs_data_volume_ready());
    K_EXPECT(vfs_file_exists("/bin/init"));

    if (m) {
        fat_pages_free(m);
        kfree(m);
    }
    /* drop the forged volume's cache lines before the device goes away, so
     * a later writeback cannot find a pointer to freed memory */
    blk_cache_purge(&k->dev);
    kblk_free(k);
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
KTEST("vfs", KTEST_LATE, test_close_saturates_the_refcount);
KTEST("vfs", KTEST_LATE, test_mount_data_reaches_the_node);
KTEST("vfs", KTEST_LATE, test_second_mount_routes_to_its_own_volume);
KTEST("vfs", KTEST_LATE, test_a_disk_without_init_is_placed_below_root);
