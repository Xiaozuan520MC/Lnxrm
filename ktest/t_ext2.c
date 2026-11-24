// SPDX-License-Identifier: Apache-2.0

/* What the ext2 driver must leave on disk, driven through its own entry
 * points the way t_fatfile.c drives the FAT32 ones: the four S2 capabilities
 * (read a block, write a block, read a file, write a file) plus the directory
 * and rename semantics that decide whether the volume still checks out after
 * the kernel has been used -- e2fsck is the judge of every number asserted
 * here, so the link counts, free counts and directory shapes below are the
 * ones it derives from the tree.
 *
 * Every case forges its own volume (kblk_forge_ext2(): 192 blocks over three
 * groups, 192 inodes) and mounts it with ext2_mount(), so the cases never
 * touch the root filesystem and a failed run cannot leave files behind for
 * the one behind it.  That makes teardown part of every case: fs_ops has no
 * unmount hook, so env_close() frees the mount, its group descriptors and --
 * the half that is easy to forget -- the block cache's copy of the volume.  A
 * purge that did not run would leave sectors keyed by a device pointer the
 * next forge is likely to reuse, and the next case would read this one's
 * bytes as its own.
 *
 * Locking: the fs_ops callbacks take ext2_fs_lock for themselves; the raw
 * entry points (ext2_*_inode, ext2_block_alloc, ext2_bmap, ext2_sync_space)
 * do not, so the helpers below wrap them in EXT2_ENTER()/EXT2_LEAVE().
 *
 * Cases do not K_ASSERT once the volume is open: an early `return` would leak
 * the 768 KiB backing store, and the heap is at its most carved up when the
 * late stage runs.  They record with K_EXPECT and, where a failure would make
 * the next line meaningless, jump to `done:` -- which is also where the
 * buffers go. */
#include <sys/ktest.h>
#include <console.h>
#include <string.h>
#include <ext2.h>
#include "kblk.h"

/* The volume the forge writes: 192 blocks, three groups of 64, 192 inodes of
 * 256 B, root inode 2 in group 0.  Repeated here as the numbers the cases
 * compare against, because "free space came back" is only meaningful next to
 * the exact count it started at. */
#define FORGED_BLOCKS   171u /* free at mount: 55 + 58 + 58 */
#define FORGED_INODES   182u /* free at mount: 54 + 64 + 64 */

/* ---- the mount every case builds and tears down ------------------------ */

struct ext2_env {
    struct kblk *k;
    struct ext2_mount *fs;
};

/* Release everything env_open() took.  The cache goes first, while the
 * device is still alive: purge writes back what is staged and then drops the
 * entries, so nothing outlives the pointer they are keyed by. */
static void env_close(struct ext2_env *e)
{
    if (e->k) blk_cache_purge(&e->k->dev);
    if (e->fs) {
        kfree(e->fs->gd);
        kfree(e->fs);
        e->fs = NULL;
    }
    if (e->k) {
        kblk_free(e->k);
        e->k = NULL;
    }
}

/* Forge and mount a volume.  A forge without memory is the case giving up
 * (skip); a mount that refuses a volume the forge just wrote to spec is the
 * driver failing, and is recorded as such. */
static bool env_open(struct ext2_env *e)
{
    memset(e, 0, sizeof(*e));
    e->k = kblk_forge_ext2("kext2");
    if (!e->k) {
        ktest_skip("no memory for a forged ext2 volume");
        return false;
    }
    e->fs = ext2_mount(&e->k->dev);
    if (!e->fs) {
        (void)ktest_check(false, __FILE__, __LINE__, "ext2_mount(&k->dev)");
        env_close(e);
        return false;
    }
    return true;
}

/* ---- helpers ----------------------------------------------------------- */

/* lookup() into `out`.  A miss is recorded as a failed check -- every path
 * these cases look up is one they just made -- and answered with NULL so the
 * caller has nothing to dereference or release. */
static void *env_lookup(struct ext2_env *e, const char *path, struct vnode *out)
{
    memset(out, 0, sizeof(*out));
    int rc = ext2_ops.lookup(e->fs, path, out);
    if (rc != 0) {
        (void)ktest_check(rc == 0, __FILE__, __LINE__, path);
        memset(out, 0, sizeof(*out));
        return NULL;
    }
    return out->fs_data;
}

/* lookup() that is EXPECTED to fail: hands back the errno, and releases the
 * node if the path answered anyway so a surprise costs nothing. */
static int env_lookup_rc(struct ext2_env *e, const char *path)
{
    struct vnode vn;
    memset(&vn, 0, sizeof(vn));
    int rc = ext2_ops.lookup(e->fs, path, &vn);
    if (rc == 0 && vn.fs_data) ext2_ops.close_node(e->fs, vn.fs_data);
    return rc;
}

/* The inode behind a path that must exist (0, with a failed check, if it
 * does not) -- later assertions on that number then compare against
 * something rather than against a stale value. */
static u32 env_ino(struct ext2_env *e, const char *path)
{
    struct vnode vn;
    void *node = env_lookup(e, path, &vn);
    if (!node) return 0;
    u32 ino = (u32)vn.ino;
    ext2_ops.close_node(e->fs, node);
    return ino;
}

/* The inode table does not lock for itself: the fs_ops callbacks own that,
 * and these helpers are the raw door. */
static int env_inode(struct ext2_env *e, u32 ino, struct ext2_inode *out)
{
    EXT2_ENTER();
    int rc = ext2_read_inode(e->fs, ino, out);
    EXT2_LEAVE();
    return rc;
}

static int env_write_inode(struct ext2_env *e, u32 ino, const struct ext2_inode *in)
{
    EXT2_ENTER();
    int rc = ext2_write_inode(e->fs, ino, in);
    EXT2_LEAVE();
    return rc;
}

/* Find `name` inside `dir_ino` and copy its raw record out.  "." and ".."
 * never reach getdent(), so this is the only way a case can see where a
 * directory's ".." points -- which is the whole of rename's directory
 * contract.  0 = found, LNXRM_ENOENT = not there, negative = the directory
 * itself would not answer. */
static int env_find_raw(struct ext2_env *e, u32 dir_ino, const char *name, u32 nlen,
                        struct ext2_dirent *out)
{
    struct ext2_mount *fs = e->fs;
    struct ext2_inode in;
    int answer = LNXRM_ENOENT;
    u8 *buf = (u8 *)kmalloc(fs->block_size);
    if (!buf) return LNXRM_ENOMEM;

    EXT2_ENTER();
    int rc = ext2_read_inode(fs, dir_ino, &in);
    if (rc) {
        answer = rc;
    } else {
        u32 bs = fs->block_size;
        u32 nblk = (u32)((in.i_size + bs - 1) / bs);
        for (u32 lblk = 0; lblk < nblk && answer == LNXRM_ENOENT; lblk++) {
            u32 pblk = 0;
            rc = ext2_bmap(fs, &in, lblk, &pblk);
            if (rc) { answer = rc; break; }
            if (!pblk) continue; /* hole: no entries to be found there */

            rc = ext2_read_data_block(fs, pblk, buf);
            if (rc) { answer = rc; break; }

            u32 off = 0;
            while (off + 8 <= bs) {
                u32 dino;
                u16 rec;
                u8 nl = buf[off + 6];
                memcpy(&dino, buf + off, 4);
                memcpy(&rec, buf + off + 4, 2);
                if (rec < 8 || (rec & 3) || off + rec > bs) {
                    answer = LNXRM_EIO;
                    break;
                }
                if (dino && nl == nlen && !memcmp(buf + off + 8, name, nlen)) {
                    memset(out, 0, sizeof(*out));
                    memcpy(out, buf + off, 8); /* inode, rec_len, name, type */
                    /* name_len is a u8, so at most 255 bytes land in the
                     * 256-byte field the memset already cleared. */
                    memcpy(out->name, buf + off + 8, nl);
                    answer = 0;
                    break;
                }
                off += rec;
            }
        }
    }
    EXT2_LEAVE();
    kfree(buf);
    return answer;
}

/* create() + a first write: the setup every file-shaped case needs.  Returns
 * the byte count write() answered with, or the errno that stopped it. */
static int env_make_file(struct ext2_env *e, const char *path, const void *data, size_t n)
{
    int rc = ext2_ops.create(e->fs, path);
    if (rc) return rc;
    struct vnode vn;
    void *node = env_lookup(e, path, &vn);
    if (!node) return LNXRM_ENOENT;
    rc = ext2_ops.write(e->fs, node, 0, data, n);
    ext2_ops.close_node(e->fs, node);
    return rc;
}

/* Read a whole file the way a user would, so a case can assert content
 * without repeating the open/offset/release dance. */
static int env_read_all(struct ext2_env *e, const char *path, void *buf, size_t n)
{
    struct vnode vn;
    void *node = env_lookup(e, path, &vn);
    if (!node) return LNXRM_ENOENT;
    int rc = ext2_ops.read(e->fs, node, 0, buf, n);
    ext2_ops.close_node(e->fs, node);
    return rc;
}

/* ---- case 1: the mount and the object it answers for ------------------- */

/* The numbers ext2_mount() read out of the superblock, and the root vnode:
 * directories carry no byte length, and the permission bit stat() publishes
 * comes from the file's own mode (0755 still has the owner's write). */
static void test_ext2_mount_and_root(void)
{
    struct ext2_env e;
    if (!env_open(&e)) return;
    struct ext2_mount *fs = e.fs;

    K_EXPECT_EQ(fs->block_size, 4096);
    K_EXPECT_EQ(fs->sectors_per_block, 8);
    K_EXPECT_EQ(fs->blocks_count, 192);
    K_EXPECT_EQ(fs->inodes_count, 192);
    K_EXPECT_EQ(fs->blocks_per_group, 64);
    K_EXPECT_EQ(fs->inodes_per_group, 64);
    K_EXPECT_EQ(fs->groups, 3);
    K_EXPECT_EQ(fs->first_data_block, 0);
    K_EXPECT_EQ(fs->inode_size, 256);
    K_EXPECT_EQ(fs->first_ino, 11);
    K_EXPECT_EQ(fs->free_blocks, FORGED_BLOCKS);
    K_EXPECT_EQ(fs->free_inodes, FORGED_INODES);
    K_EXPECT_EQ(fs->gd[0].bg_free_blocks_count, 55);
    K_EXPECT_EQ(fs->gd[0].bg_free_inodes_count, 54);
    K_EXPECT_EQ(fs->gd[0].bg_used_dirs_count, 1);  /* root, and only root */
    K_EXPECT_EQ(fs->gd[1].bg_block_bitmap, 64);
    K_EXPECT_EQ(fs->gd[1].bg_free_blocks_count, 58);
    K_EXPECT_EQ(fs->gd[2].bg_inode_table, 130);
    K_EXPECT_EQ(fs->gd[2].bg_free_inodes_count, 64);

    struct vnode vn;
    void *root = env_lookup(&e, "", &vn);
    if (root) {
        K_EXPECT_EQ(vn.type, V_DIR);
        K_EXPECT_EQ(vn.ino, EXT2_ROOT_INO);
        K_EXPECT_EQ(vn.size, 0); /* a directory has no byte length */
        K_EXPECT(!vn.ro);        /* 0755 keeps the owner's write bit */
        K_EXPECT(vn.ops == &ext2_ops);
        K_EXPECT(vn.mnt_data == fs);
        struct ext2_node *n = root;
        K_EXPECT_EQ(n->ino, EXT2_ROOT_INO);
        K_EXPECT_EQ(n->is_dir, 1);
        ext2_ops.close_node(fs, root);
    }

    K_EXPECT_EQ(env_lookup_rc(&e, "nothing-here"), LNXRM_ENOENT);
    K_EXPECT_EQ(ext2_ops.freespace(fs), (u64)FORGED_BLOCKS * 4096);

    env_close(&e);
}

/* ---- case 2: reading and writing one block ----------------------------- */

/* The block layer under the filesystem: the root directory's own block as it
 * sits on the forge, the superblock through the same door, both refusals past
 * the end of the volume, and -- the part only a multi-group volume can prove
 * -- an allocator that walks every group, stops exactly where the bitmaps do,
 * and leaves counters the superblock on the device agrees with. */
static void test_ext2_block_io(void)
{
    struct ext2_env e;
    if (!env_open(&e)) return;
    struct ext2_mount *fs = e.fs;
    u32 bs = fs->block_size;
    u32 blk = 0;
    u32 *taken = NULL;
    u8 *buf = kmalloc(bs);
    int rc;

    if (!buf) {
        ktest_skip("no memory for a block buffer");
        goto done;
    }

    /* The root directory, read as the bytes an e2fsck would see: "." then
     * "..", the second record's rec_len reaching the block's last byte. */
    K_EXPECT_EQ(ext2_read_data_block(fs, 8, buf), 0);
    struct ext2_dirent *dot = (struct ext2_dirent *)buf;
    K_EXPECT_EQ(dot->inode, EXT2_ROOT_INO);
    K_EXPECT_EQ(dot->rec_len, 12);
    K_EXPECT_EQ(dot->name_len, 1);
    K_EXPECT_EQ(dot->file_type, EXT2_FT_DIR);
    K_EXPECT_EQ(dot->name[0], '.');
    struct ext2_dirent *dd = (struct ext2_dirent *)(buf + 12);
    K_EXPECT_EQ(dd->inode, EXT2_ROOT_INO);
    K_EXPECT_EQ(dd->rec_len, bs - 12);
    K_EXPECT_EQ(dd->name_len, 2);
    K_EXPECT(dd->name[0] == '.' && dd->name[1] == '.');

    /* block 0 carries the superblock at byte 1024 -- the same door, no
     * special case in the caller */
    K_EXPECT_EQ(ext2_read_data_block(fs, 0, buf), 0);
    struct ext2_super_block *sb = (struct ext2_super_block *)(buf + EXT2_SUPER_OFFSET);
    K_EXPECT_EQ(sb->s_magic, EXT2_SUPER_MAGIC);
    K_EXPECT_EQ(sb->s_free_blocks_count, FORGED_BLOCKS);
    K_EXPECT_EQ(sb->s_free_inodes_count, FORGED_INODES);

    /* Past the volume, both directions: the forged-image case where a
     * corrupted i_block names a sector the disk does not have. */
    K_EXPECT_EQ(ext2_read_data_block(fs, fs->blocks_count, buf), LNXRM_EIO);
    K_EXPECT_EQ(ext2_write_data_block(fs, fs->blocks_count, buf), LNXRM_EIO);
    K_EXPECT_EQ(ext2_read_data_block(fs, 0xFFFFFFFFu, buf), LNXRM_EIO);
    K_EXPECT_EQ(ext2_write_data_block(fs, 0xFFFFFFFFu, buf), LNXRM_EIO);

    /* One block claimed, written, read back through the cache and -- after
     * the flush that proves it -- straight off the device. */
    EXT2_ENTER();
    rc = ext2_block_alloc(fs, &blk);
    EXT2_LEAVE();
    K_EXPECT_EQ(rc, 0);
    if (!rc) {
        K_EXPECT(blk >= 9 && blk < fs->blocks_count); /* group 0's free run */
        K_EXPECT_EQ(ext2_ops.freespace(fs), (u64)(FORGED_BLOCKS - 1) * 4096);

        for (u32 i = 0; i < bs; i++) buf[i] = (u8)(i * 7 + 3);
        K_EXPECT_EQ(ext2_write_data_block(fs, blk, buf), 0);
        K_EXPECT_EQ(blk_cache_flush(fs->dev), 0);

        u8 *raw = kblk_sector(e.k, (u64)blk * fs->sectors_per_block);
        K_EXPECT(raw != NULL);
        if (raw) K_EXPECT(memcmp(raw, buf, bs) == 0);

        memset(buf, 0, bs);
        K_EXPECT_EQ(ext2_read_data_block(fs, blk, buf), 0);
        K_EXPECT(buf[0] == 3 && buf[4095] == (u8)(4095 * 7 + 3));

        EXT2_ENTER();
        ext2_block_free(fs, blk);
        EXT2_LEAVE();
        K_EXPECT_EQ(ext2_ops.freespace(fs), (u64)FORGED_BLOCKS * 4096);
    }

    /* The allocator's real contract.  Group 0 has 55 free blocks (0..8 are
     * metadata), groups 1 and 2 have 58 each; an allocator that only ever
     * finds room in group 0 passes every single-group test and is wrong on
     * the first disk that fills up. */
    taken = kmalloc(200 * sizeof(u32));
    if (!taken) {
        ktest_skip("no memory for the allocator sweep");
        goto done;
    }
    u32 n = 0;
    for (;;) {
        u32 b = 0;
        EXT2_ENTER();
        rc = ext2_block_alloc(fs, &b);
        EXT2_LEAVE();
        if (rc) break;
        taken[n++] = b;
        if (n >= 200) break; /* the forge has 171: unreachable in practice */
    }
    K_EXPECT_EQ(rc, LNXRM_ENOSPC);
    K_EXPECT_EQ(n, FORGED_BLOCKS);
    K_EXPECT_EQ(ext2_ops.freespace(fs), 0);

    u32 g0 = 0, g1 = 0, g2 = 0;
    for (u32 i = 0; i < n; i++) {
        if (taken[i] < 64) g0++;
        else if (taken[i] < 128) g1++;
        else g2++;
    }
    K_EXPECT_EQ(g0, 55);
    K_EXPECT_EQ(g1, 58);
    K_EXPECT_EQ(g2, 58);

    /* Give every one of them back, persist, and read the result back from
     * the device: superblock and group descriptors must say what the bitmaps
     * say, which is the half of e2fsck -n that a live mount can break. */
    EXT2_ENTER();
    for (u32 i = 0; i < n; i++) ext2_block_free(fs, taken[i]);
    ext2_sync_space(fs);
    EXT2_LEAVE();
    K_EXPECT_EQ(ext2_ops.freespace(fs), (u64)FORGED_BLOCKS * 4096);
    K_EXPECT_EQ(blk_cache_flush(fs->dev), 0);

    K_EXPECT_EQ(ext2_read_data_block(fs, 0, buf), 0);
    sb = (struct ext2_super_block *)(buf + EXT2_SUPER_OFFSET);
    K_EXPECT_EQ(sb->s_free_blocks_count, FORGED_BLOCKS);
    K_EXPECT_EQ(sb->s_free_inodes_count, FORGED_INODES);

    K_EXPECT_EQ(ext2_read_data_block(fs, 1, buf), 0);
    struct ext2_group_desc *gd = (struct ext2_group_desc *)buf;
    K_EXPECT_EQ(gd[0].bg_free_blocks_count, 55);
    K_EXPECT_EQ(gd[1].bg_free_blocks_count, 58);
    K_EXPECT_EQ(gd[2].bg_free_blocks_count, 58);
    K_EXPECT_EQ(gd[0].bg_free_inodes_count, 54);
    K_EXPECT_EQ(gd[0].bg_used_dirs_count, 1);

done:
    kfree(taken);
    kfree(buf);
    env_close(&e);
}

/* ---- case 3: reading and writing a file -------------------------------- */

/* The file level: grow, read back, straddle a block boundary, truncate to
 * nothing and grow again with a hole in the middle -- plus the three
 * refusals that are not a file, and the permission bit stat() will publish
 * once the mode changes. */
static void test_ext2_file_read_write(void)
{
    struct ext2_env e;
    struct ext2_mount *fs;
    struct vnode vn;
    struct ext2_inode in;
    struct dirent_out d;
    void *node = NULL, *dnode = NULL, *ro = NULL;
    u8 *pat = NULL, *out = NULL;
    u64 base = 0, cookie = 0;
    u32 pblk = 0;
    int rc;

    if (!env_open(&e)) return;
    fs = e.fs;
    base = ext2_ops.freespace(fs);

    pat = kmalloc(8192);
    out = kmalloc(8192);
    if (!pat || !out) {
        ktest_skip("no memory for two 8 KiB file buffers");
        goto done;
    }

    /* create() makes an empty regular file: mode 0664, one link, size 0 */
    K_EXPECT_EQ(ext2_ops.create(fs, "alpha"), 0);
    node = env_lookup(&e, "alpha", &vn);
    if (!node) goto done;
    K_EXPECT_EQ(vn.type, V_REG);
    K_EXPECT_EQ(vn.size, 0);
    K_EXPECT(!vn.ro); /* create() hands out 0664 */
    K_EXPECT(vn.mnt_data == fs);
    K_EXPECT_EQ(ext2_ops.size(fs, node), 0);

    /* What is NOT a file: a directory node, a NULL node, and a NULL buffer
     * that is supposed to carry bytes (only buf==NULL with n==0 truncates). */
    dnode = env_lookup(&e, "", &vn);
    if (dnode) {
        K_EXPECT_EQ(ext2_ops.read(fs, dnode, 0, out, 16), LNXRM_EFAIL);
        K_EXPECT_EQ(ext2_ops.write(fs, dnode, 0, out, 16), LNXRM_EFAIL);
        K_EXPECT_EQ(ext2_ops.write(fs, dnode, 0, NULL, 1), LNXRM_EFAIL);
        K_EXPECT_EQ(ext2_ops.size(fs, dnode), 0); /* directories are size 0 */
    }
    K_EXPECT_EQ(ext2_ops.read(fs, NULL, 0, out, 4), LNXRM_EFAIL);
    K_EXPECT_EQ(ext2_ops.write(fs, NULL, 0, out, 4), LNXRM_EFAIL);
    K_EXPECT_EQ(ext2_ops.read(fs, node, 0, NULL, 4), LNXRM_EFAIL);
    cookie = 0;
    K_EXPECT_EQ(ext2_ops.getdent(fs, node, &cookie, &d), LNXRM_EINVAL);
    K_EXPECT_EQ(ext2_ops.getdent(fs, NULL, &cookie, &d), LNXRM_EINVAL);

    /* Two blocks of data: the length, the accounting and the free count all
     * move together, and none of it is left to the caller to flush. */
    for (u32 i = 0; i < 6000; i++) pat[i] = (u8)(i ^ 0x5A);
    K_EXPECT_EQ(ext2_ops.write(fs, node, 0, pat, 6000), 6000);
    K_EXPECT_EQ(ext2_ops.size(fs, node), 6000);
    K_EXPECT_EQ(env_inode(&e, ((struct ext2_node *)node)->ino, &in), 0);
    K_EXPECT_EQ(in.i_size, 6000);
    K_EXPECT_EQ(in.i_blocks, 16); /* two 4 KiB blocks = 8 sectors each */
    K_EXPECT_EQ(in.i_links_count, 1);
    K_EXPECT_EQ(in.i_mode, EXT2_S_IFREG | 0664);
    K_EXPECT_EQ(ext2_ops.freespace(fs), base - 2 * 4096);

    K_EXPECT_EQ(ext2_ops.read(fs, node, 0, out, 6000), 6000);
    K_EXPECT(memcmp(pat, out, 6000) == 0);
    K_EXPECT_EQ(ext2_ops.read(fs, node, 5990, out, 100), 10); /* short at EOF */
    K_EXPECT(memcmp(pat + 5990, out, 10) == 0);
    K_EXPECT_EQ(ext2_ops.read(fs, node, 6000, out, 10), 0);   /* none past it */
    K_EXPECT_EQ(ext2_ops.read(fs, node, 0, out, 0), 0);

    EXT2_ENTER();
    rc = ext2_bmap(fs, &in, 0, &pblk);
    EXT2_LEAVE();
    K_EXPECT_EQ(rc, 0);
    if (!rc) {
        u8 *raw = kblk_sector(e.k, (u64)pblk * fs->sectors_per_block);
        K_EXPECT(raw != NULL);
        if (raw) K_EXPECT(memcmp(raw, pat, 4096) == 0);
    }

    /* A write straddling the block boundary lands in both blocks and, being
     * inside the file, does not move EOF. */
    K_EXPECT_EQ(ext2_ops.write(fs, node, 4090, "0123456789AB", 12), 12);
    K_EXPECT_EQ(ext2_ops.size(fs, node), 6000);
    K_EXPECT_EQ(ext2_ops.read(fs, node, 4090, out, 12), 12);
    K_EXPECT(memcmp(out, "0123456789AB", 12) == 0);

    /* O_TRUNC: buf==NULL with n==0 empties the file and gives every block
     * back -- on disk, not just in the free-space counter in memory. */
    K_EXPECT_EQ(ext2_ops.write(fs, node, 0, NULL, 0), 0);
    K_EXPECT_EQ(ext2_ops.size(fs, node), 0);
    K_EXPECT_EQ(ext2_ops.read(fs, node, 0, out, 6000), 0);
    K_EXPECT_EQ(ext2_ops.freespace(fs), base);
    K_EXPECT_EQ(env_inode(&e, ((struct ext2_node *)node)->ino, &in), 0);
    K_EXPECT_EQ(in.i_size, 0);
    K_EXPECT_EQ(in.i_blocks, 0);

    /* Two writes with a gap: the gap is a hole and reads as zeroes -- never
     * as whatever the block used to hold -- and the block that does exist
     * starts zeroed too. */
    K_EXPECT_EQ(ext2_ops.write(fs, node, 0, "head", 4), 4);
    K_EXPECT_EQ(ext2_ops.write(fs, node, 12000, "tail", 4), 4);
    K_EXPECT_EQ(ext2_ops.size(fs, node), 12004);
    K_EXPECT_EQ(env_inode(&e, ((struct ext2_node *)node)->ino, &in), 0);
    K_EXPECT_EQ(in.i_blocks, 16); /* two blocks, with the hole between them */

    K_EXPECT_EQ(ext2_ops.read(fs, node, 4096, out, 4096), 4096);
    rc = 0;
    for (u32 i = 0; i < 4096; i++)
        if (out[i]) { rc = 1; break; }
    K_EXPECT_EQ(rc, 0); /* the hole came back as zeroes */

    /* The read stops at EOF like any other: the file is 12004 bytes, so
     * 3812 come back from offset 8192 and the tail still sits 3808 bytes
     * into them. */
    K_EXPECT_EQ(ext2_ops.read(fs, node, 8192, out, 4096), 12004 - 8192);
    K_EXPECT(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0);
    K_EXPECT(memcmp(out + 3808, "tail", 4) == 0); /* 12000 - 8192 into the block */

    /* stat's read-only bit comes from the file's own mode: ext2 has real
     * permission bits where FAT32 only ever had an attribute. */
    K_EXPECT_EQ(env_inode(&e, ((struct ext2_node *)node)->ino, &in), 0);
    in.i_mode = EXT2_S_IFREG | 0444;
    K_EXPECT_EQ(env_write_inode(&e, ((struct ext2_node *)node)->ino, &in), 0);
    ro = env_lookup(&e, "alpha", &vn);
    if (ro) {
        K_EXPECT(vn.ro);
        ext2_ops.close_node(fs, ro);
        ro = NULL;
    }

    /* Unlink drops the name, the blocks and the inode, and the volume's free
     * numbers are exactly where they started. */
    K_EXPECT_EQ(ext2_ops.unlink(fs, "alpha"), 0);
    K_EXPECT_EQ(env_lookup_rc(&e, "alpha"), LNXRM_ENOENT);
    K_EXPECT_EQ(ext2_ops.unlink(fs, "alpha"), LNXRM_ENOENT);
    K_EXPECT_EQ(ext2_ops.freespace(fs), base);
    K_EXPECT_EQ(fs->free_inodes, FORGED_INODES);

done:
    if (node) ext2_ops.close_node(fs, node);
    if (dnode) ext2_ops.close_node(fs, dnode);
    kfree(pat);
    kfree(out);
    env_close(&e);
}

/* ---- case 4: directories ------------------------------------------------ */

/* mkdir/create/unlink/rmdir and the errno each wrong shape answers with,
 * with the link counts and used-directory count e2fsck derives from the
 * tree, and getdent() walking a directory to a clean end.  The case cleans
 * up after itself and then asserts the root is byte-for-byte the empty
 * directory the forge started with. */
static void test_ext2_directory_ops(void)
{
    struct ext2_env e;
    if (!env_open(&e)) return;
    struct ext2_mount *fs = e.fs;
    const u64 base = ext2_ops.freespace(fs);
    struct vnode vn;
    struct ext2_inode in;
    struct ext2_dirent de;
    struct dirent_out d;
    u64 cookie;
    void *root = NULL, *fnode = NULL;

    /* mkdir publishes the entry, the two dot records, the parent's new link
     * and the group's used-directory count -- all four together. */
    K_EXPECT_EQ(ext2_ops.mkdir(fs, "d1"), 0);
    u32 d1 = env_ino(&e, "d1");
    K_EXPECT(d1 > EXT2_ROOT_INO);
    root = env_lookup(&e, "d1", &vn);
    if (root) {
        K_EXPECT_EQ(vn.type, V_DIR);
        K_EXPECT_EQ(vn.size, 0);
        K_EXPECT(!vn.ro); /* 0755 keeps the owner's write bit */
        ext2_ops.close_node(fs, root);
        root = NULL;
    }
    K_EXPECT_EQ(env_inode(&e, EXT2_ROOT_INO, &in), 0);
    K_EXPECT_EQ(in.i_links_count, 3); /* "." + the entry + d1's ".." */
    K_EXPECT_EQ(env_inode(&e, d1, &in), 0);
    K_EXPECT_EQ(in.i_mode, EXT2_S_IFDIR | 0755);
    K_EXPECT_EQ(in.i_links_count, 2);
    K_EXPECT_EQ(in.i_size, 4096);
    K_EXPECT_EQ(in.i_blocks, 8);
    K_EXPECT_EQ(ext2_ops.freespace(fs), base - 4096);
    K_EXPECT_EQ(fs->free_inodes, FORGED_INODES - 1);
    K_EXPECT_EQ(fs->gd[0].bg_used_dirs_count, 2);

    K_EXPECT_EQ(env_find_raw(&e, d1, ".", 1, &de), 0);
    K_EXPECT_EQ(de.inode, d1);
    K_EXPECT_EQ(de.file_type, EXT2_FT_DIR);
    K_EXPECT_EQ(env_find_raw(&e, d1, "..", 2, &de), 0);
    K_EXPECT_EQ(de.inode, EXT2_ROOT_INO);

    /* A name that is already there is EEXIST, whichever way it was made. */
    K_EXPECT_EQ(ext2_ops.mkdir(fs, "d1"), LNXRM_EEXIST);
    K_EXPECT_EQ(ext2_ops.create(fs, "d1"), LNXRM_EEXIST);

    /* A directory below a directory: the parent gains a link, the child
     * starts at two, and its ".." names the parent. */
    K_EXPECT_EQ(ext2_ops.mkdir(fs, "d1/d2"), 0);
    u32 d2 = env_ino(&e, "d1/d2");
    K_EXPECT_EQ(env_inode(&e, d1, &in), 0);
    K_EXPECT_EQ(in.i_links_count, 3);
    K_EXPECT_EQ(env_inode(&e, d2, &in), 0);
    K_EXPECT_EQ(in.i_links_count, 2);
    K_EXPECT_EQ(in.i_size, 4096);
    K_EXPECT_EQ(env_find_raw(&e, d2, "..", 2, &de), 0);
    K_EXPECT_EQ(de.inode, d1);
    K_EXPECT_EQ(fs->gd[0].bg_used_dirs_count, 3);

    /* A file: regular mode, one link, the length the write delivered. */
    K_EXPECT_EQ(env_make_file(&e, "d1/f1", "hello", 5), 5);
    u32 f1 = env_ino(&e, "d1/f1");
    K_EXPECT(f1 > EXT2_ROOT_INO);
    K_EXPECT_EQ(env_inode(&e, f1, &in), 0);
    K_EXPECT_EQ(in.i_mode, EXT2_S_IFREG | 0664);
    K_EXPECT_EQ(in.i_links_count, 1);
    K_EXPECT_EQ(in.i_size, 5);
    K_EXPECT_EQ(fs->free_inodes, FORGED_INODES - 3);
    K_EXPECT_EQ(ext2_ops.create(fs, "d1/f1"), LNXRM_EEXIST);

    /* Every wrong parent and wrong type the namespace promises. */
    K_EXPECT_EQ(ext2_ops.create(fs, "nope/x"), LNXRM_ENOENT);
    K_EXPECT_EQ(ext2_ops.create(fs, "d1/f1/deep"), LNXRM_ENOTDIR);
    K_EXPECT_EQ(ext2_ops.mkdir(fs, "nope/d"), LNXRM_ENOENT);
    K_EXPECT_EQ(ext2_ops.mkdir(fs, "d1/f1"), LNXRM_EEXIST);
    K_EXPECT_EQ(ext2_ops.rmdir(fs, "d1/f1"), LNXRM_ENOTDIR);
    K_EXPECT_EQ(ext2_ops.unlink(fs, "d1/f1/deep"), LNXRM_ENOTDIR);
    K_EXPECT_EQ(ext2_ops.unlink(fs, "d1"), LNXRM_EFAIL); /* a dir goes via rmdir */
    K_EXPECT_EQ(ext2_ops.unlink(fs, "d1/nope"), LNXRM_ENOENT);
    K_EXPECT_EQ(ext2_ops.rmdir(fs, "d1/nope"), LNXRM_ENOENT);
    K_EXPECT_EQ(ext2_ops.rename(fs, "d1/nope", "d1/q"), LNXRM_ENOENT);
    K_EXPECT_EQ(env_lookup_rc(&e, "d1/f1/deep"), LNXRM_ENOTDIR);
    K_EXPECT_EQ(env_lookup_rc(&e, "nope"), LNXRM_ENOENT);

    /* "." and ".." are entries every directory already carries: creating,
     * removing or renaming one would rewrite those very records, so the
     * mutation is refused with EINVAL instead. */
    K_EXPECT_EQ(ext2_ops.create(fs, "."), LNXRM_EINVAL);
    K_EXPECT_EQ(ext2_ops.mkdir(fs, "d1/."), LNXRM_EINVAL);
    K_EXPECT_EQ(ext2_ops.unlink(fs, "d1/.."), LNXRM_EINVAL);
    K_EXPECT_EQ(ext2_ops.rmdir(fs, "."), LNXRM_EINVAL);

    /* getdent(): real entries only (the dots are never reported), the type
     * byte the VFS hands out, and a cookie that walks to a clean 0. */
    root = env_lookup(&e, "", &vn);
    if (root) {
        cookie = 0;
        K_EXPECT_EQ(ext2_ops.getdent(fs, root, &cookie, &d), 1);
        K_EXPECT_EQ(strcmp(d.name, "d1"), 0);
        K_EXPECT_EQ(d.type, 4); /* DT_DIR */
        K_EXPECT_EQ(d.ino, d1);
        K_EXPECT_EQ(ext2_ops.getdent(fs, root, &cookie, &d), 0);
        cookie = 0; /* restart: the same scan sees the same single entry */
        K_EXPECT_EQ(ext2_ops.getdent(fs, root, &cookie, &d), 1);
        K_EXPECT_EQ(d.ino, d1);
        ext2_ops.close_node(fs, root);
        root = NULL;
    }
    fnode = env_lookup(&e, "d1/f1", &vn);
    if (fnode) {
        cookie = 0;
        K_EXPECT_EQ(ext2_ops.getdent(fs, fnode, &cookie, &d), LNXRM_EINVAL);
        ext2_ops.close_node(fs, fnode);
        fnode = NULL;
    }

    /* rmdir only when nothing but the dots remain. */
    K_EXPECT_EQ(ext2_ops.create(fs, "d1/d2/x"), 0);
    K_EXPECT_EQ(ext2_ops.rmdir(fs, "d1/d2"), LNXRM_ENOTEMPTY);
    K_EXPECT_EQ(ext2_ops.unlink(fs, "d1/d2"), LNXRM_EFAIL);
    K_EXPECT_EQ(ext2_ops.unlink(fs, "d1/d2/x"), 0);
    K_EXPECT_EQ(ext2_ops.rmdir(fs, "d1/d2"), 0);
    K_EXPECT_EQ(env_lookup_rc(&e, "d1/d2"), LNXRM_ENOENT);
    K_EXPECT_EQ(env_inode(&e, d1, &in), 0);
    K_EXPECT_EQ(in.i_links_count, 2);      /* the subdirectory's ".." went */
    K_EXPECT_EQ(fs->free_inodes, FORGED_INODES - 2);
    K_EXPECT_EQ(fs->gd[0].bg_used_dirs_count, 2);

    /* Everything the case created goes away, and the root is the empty
     * directory the forge left behind. */
    K_EXPECT_EQ(ext2_ops.unlink(fs, "d1/f1"), 0);
    K_EXPECT_EQ(ext2_ops.rmdir(fs, "d1"), 0);
    K_EXPECT_EQ(env_lookup_rc(&e, "d1"), LNXRM_ENOENT);
    K_EXPECT_EQ(env_inode(&e, EXT2_ROOT_INO, &in), 0);
    K_EXPECT_EQ(in.i_links_count, 2);
    K_EXPECT_EQ(fs->free_inodes, FORGED_INODES);
    K_EXPECT_EQ(ext2_ops.freespace(fs), base);
    K_EXPECT_EQ(fs->gd[0].bg_used_dirs_count, 1);

    root = env_lookup(&e, "", &vn);
    if (root) {
        cookie = 0;
        K_EXPECT_EQ(ext2_ops.getdent(fs, root, &cookie, &d), 0);
        ext2_ops.close_node(fs, root);
        root = NULL;
    }

    env_close(&e);
}

/* ---- case 5: rename ----------------------------------------------------- */

/* Linux's ext2_rename() order, asserted through the effects: a rename that
 * moves the name and its bytes, a replacement that retargets the victim's
 * record (so the name exists at every instant) and gives the victim's inode
 * back, the directory rules -- ".." follows a moved directory, both parents'
 * link counts step, a directory may not enter its own subtree, and a
 * non-empty victim stops it -- and the type answers this errno table spells
 * ENOTDIR in both directions. */
static void test_ext2_rename(void)
{
    struct ext2_env e;
    if (!env_open(&e)) return;
    struct ext2_mount *fs = e.fs;
    struct ext2_inode in;
    struct ext2_dirent de;
    char buf[32];
    u32 before;

    /* Two directories, one subdirectory, two files to move between them. */
    K_EXPECT_EQ(ext2_ops.mkdir(fs, "a"), 0);
    K_EXPECT_EQ(ext2_ops.mkdir(fs, "b"), 0);
    u32 A = env_ino(&e, "a");
    u32 B = env_ino(&e, "b");
    K_EXPECT_EQ(env_inode(&e, EXT2_ROOT_INO, &in), 0);
    K_EXPECT_EQ(in.i_links_count, 4); /* root + a + b */

    K_EXPECT_EQ(ext2_ops.mkdir(fs, "a/sub"), 0);
    u32 S = env_ino(&e, "a/sub");
    K_EXPECT_EQ(env_inode(&e, A, &in), 0);
    K_EXPECT_EQ(in.i_links_count, 3);

    K_EXPECT_EQ(env_make_file(&e, "a/f1", "one", 3), 3);
    u32 F1 = env_ino(&e, "a/f1");
    before = fs->free_inodes; /* as it is before f2 exists */
    K_EXPECT_EQ(env_make_file(&e, "a/f2", "two", 3), 3);
    u32 F2 = env_ino(&e, "a/f2");
    K_EXPECT_EQ(fs->free_inodes, before - 1);

    /* A path renamed onto itself changes nothing -- and must not run the
     * replacement half, which would free the source's own blocks. */
    K_EXPECT_EQ(ext2_ops.rename(fs, "a/f1", "a/f1"), 0);
    K_EXPECT_EQ(env_read_all(&e, "a/f1", buf, sizeof(buf)), 3);
    K_EXPECT(memcmp(buf, "one", 3) == 0);
    K_EXPECT_EQ(env_inode(&e, F1, &in), 0);
    K_EXPECT_EQ(in.i_links_count, 1);
    K_EXPECT_EQ(fs->free_inodes, before - 1);

    /* A plain rename: the name moves, the inode and its bytes follow. */
    K_EXPECT_EQ(ext2_ops.rename(fs, "a/f1", "a/ren"), 0);
    K_EXPECT_EQ(env_lookup_rc(&e, "a/f1"), LNXRM_ENOENT);
    K_EXPECT_EQ(env_ino(&e, "a/ren"), F1);

    /* Replacement: the victim's own record is retargeted at the source, so
     * no instant passes without a name, and the victim's inode is wiped and
     * returned to the pool. */
    K_EXPECT_EQ(ext2_ops.rename(fs, "a/ren", "a/f2"), 0);
    K_EXPECT_EQ(env_lookup_rc(&e, "a/ren"), LNXRM_ENOENT);
    K_EXPECT_EQ(env_ino(&e, "a/f2"), F1); /* f1's inode under f2's name */
    K_EXPECT_EQ(env_inode(&e, F2, &in), 0);
    K_EXPECT_EQ(in.i_links_count, 0);     /* the victim went */
    K_EXPECT_EQ(fs->free_inodes, before);
    K_EXPECT_EQ(env_read_all(&e, "a/f2", buf, sizeof(buf)), 3);
    K_EXPECT(memcmp(buf, "one", 3) == 0); /* f1's bytes, not f2's */

    /* Type mismatch answers ENOTDIR in both directions (no EISDIR here),
     * and a refusal changes nothing. */
    K_EXPECT_EQ(ext2_ops.rename(fs, "a/f2", "a/sub"), LNXRM_ENOTDIR);
    K_EXPECT_EQ(ext2_ops.rename(fs, "a/sub", "a/f2"), LNXRM_ENOTDIR);
    K_EXPECT_EQ(env_ino(&e, "a/f2"), F1);
    K_EXPECT_EQ(env_ino(&e, "a/sub"), S);

    /* A directory may not be moved into its own subtree -- walked through
     * "..", so a different spelling of the same move is caught too. */
    K_EXPECT_EQ(ext2_ops.rename(fs, "a", "a/inside"), LNXRM_EINVAL);
    K_EXPECT_EQ(ext2_ops.rename(fs, "a/sub", "a/sub/deep"), LNXRM_EINVAL);
    K_EXPECT_EQ(env_ino(&e, "a/sub"), S);

    /* Missing ends, and a source that survives the refusal. */
    K_EXPECT_EQ(ext2_ops.rename(fs, "a/nope", "a/x"), LNXRM_ENOENT);
    K_EXPECT_EQ(ext2_ops.rename(fs, "a/f2", "nope/x"), LNXRM_ENOENT);
    K_EXPECT_EQ(env_ino(&e, "a/f2"), F1);

    /* A file crossing parents: the name moves, no link count does. */
    K_EXPECT_EQ(ext2_ops.rename(fs, "a/f2", "b/moved"), 0);
    K_EXPECT_EQ(env_lookup_rc(&e, "a/f2"), LNXRM_ENOENT);
    K_EXPECT_EQ(env_ino(&e, "b/moved"), F1);
    K_EXPECT_EQ(env_read_all(&e, "b/moved", buf, sizeof(buf)), 3);
    K_EXPECT(memcmp(buf, "one", 3) == 0);
    K_EXPECT_EQ(env_inode(&e, A, &in), 0);
    K_EXPECT_EQ(in.i_links_count, 3); /* still one subdirectory */
    K_EXPECT_EQ(env_inode(&e, B, &in), 0);
    K_EXPECT_EQ(in.i_links_count, 2); /* files are not links */

    /* Same name twice: the guard runs before anything is written. */
    K_EXPECT_EQ(ext2_ops.rename(fs, "b/moved", "b/moved"), 0);
    K_EXPECT_EQ(env_read_all(&e, "b/moved", buf, sizeof(buf)), 3);
    K_EXPECT(memcmp(buf, "one", 3) == 0);

    /* A directory inside the same parent: net zero links (".. does not
     * move") and an unchanged ".." record. */
    K_EXPECT_EQ(ext2_ops.rename(fs, "a/sub", "a/sub2"), 0);
    K_EXPECT_EQ(env_ino(&e, "a/sub2"), S);
    K_EXPECT_EQ(env_inode(&e, A, &in), 0);
    K_EXPECT_EQ(in.i_links_count, 3); /* +1 for the add, -1 for the drop */
    K_EXPECT_EQ(env_find_raw(&e, S, "..", 2, &de), 0);
    K_EXPECT_EQ(de.inode, A);

    /* A directory crossing parents: both parents' counts follow the move and
     * the moved directory's ".." points at its new home. */
    K_EXPECT_EQ(ext2_ops.mkdir(fs, "b/box"), 0);
    u32 BOX = env_ino(&e, "b/box");
    K_EXPECT_EQ(env_inode(&e, B, &in), 0);
    K_EXPECT_EQ(in.i_links_count, 3); /* b + box's ".." */

    K_EXPECT_EQ(ext2_ops.rename(fs, "a/sub2", "b/sub3"), 0);
    K_EXPECT_EQ(env_lookup_rc(&e, "a/sub2"), LNXRM_ENOENT);
    K_EXPECT_EQ(env_ino(&e, "b/sub3"), S);
    K_EXPECT_EQ(env_inode(&e, A, &in), 0);
    K_EXPECT_EQ(in.i_links_count, 2); /* the subdirectory left a */
    K_EXPECT_EQ(env_inode(&e, B, &in), 0);
    K_EXPECT_EQ(in.i_links_count, 4); /* and arrived at b */
    K_EXPECT_EQ(env_find_raw(&e, S, "..", 2, &de), 0);
    K_EXPECT_EQ(de.inode, B);
    K_EXPECT_EQ(env_inode(&e, EXT2_ROOT_INO, &in), 0);
    K_EXPECT_EQ(in.i_links_count, 4); /* root's own children did not move */

    /* The same subtree rule, from a name that was not the one above. */
    K_EXPECT_EQ(ext2_ops.rename(fs, "b/box", "b/box/inside"), LNXRM_EINVAL);

    /* Replacing a directory with a directory: the victim must be empty. */
    K_EXPECT_EQ(ext2_ops.mkdir(fs, "a/ful"), 0);
    K_EXPECT_EQ(env_make_file(&e, "a/ful/z", "zz", 2), 2);
    K_EXPECT_EQ(ext2_ops.rename(fs, "b/box", "a/ful"), LNXRM_ENOTEMPTY);
    K_EXPECT(env_ino(&e, "a/ful") > EXT2_ROOT_INO); /* still there */

    /* ... and an empty one is taken: the victim's link for "." and for its
     * record both go, the moved directory's ".." follows it, and the group's
     * used-directory count steps down with the victim. */
    K_EXPECT_EQ(ext2_ops.mkdir(fs, "a/old"), 0);
    u32 OLD = env_ino(&e, "a/old");
    u32 dirs = fs->gd[0].bg_used_dirs_count;
    K_EXPECT_EQ(ext2_ops.rename(fs, "b/box", "a/old"), 0);
    K_EXPECT_EQ(env_lookup_rc(&e, "b/box"), LNXRM_ENOENT);
    K_EXPECT_EQ(env_ino(&e, "a/old"), BOX);   /* the victim's name, the mover's inode */
    K_EXPECT_EQ(env_inode(&e, OLD, &in), 0);
    K_EXPECT_EQ(in.i_links_count, 0);
    K_EXPECT_EQ(env_find_raw(&e, BOX, "..", 2, &de), 0);
    K_EXPECT_EQ(de.inode, A);                 /* the move followed it */
    K_EXPECT_EQ(env_inode(&e, A, &in), 0);
    K_EXPECT_EQ(in.i_links_count, 4);         /* ful + the record now holding box */
    K_EXPECT_EQ(env_inode(&e, B, &in), 0);
    K_EXPECT_EQ(in.i_links_count, 3);         /* box went, sub3 remains */
    K_EXPECT_EQ(fs->gd[0].bg_used_dirs_count, dirs - 1);

    /* Cleanup, then the numbers the forge wrote. */
    K_EXPECT_EQ(ext2_ops.unlink(fs, "b/moved"), 0);
    K_EXPECT_EQ(ext2_ops.unlink(fs, "a/ful/z"), 0);
    K_EXPECT_EQ(ext2_ops.rmdir(fs, "a/ful"), 0);
    K_EXPECT_EQ(ext2_ops.rmdir(fs, "a/old"), 0); /* holds box, which is empty */
    K_EXPECT_EQ(ext2_ops.rmdir(fs, "b/sub3"), 0);
    K_EXPECT_EQ(ext2_ops.rmdir(fs, "b"), 0);
    K_EXPECT_EQ(ext2_ops.rmdir(fs, "a"), 0);
    K_EXPECT_EQ(env_lookup_rc(&e, "a"), LNXRM_ENOENT);
    K_EXPECT_EQ(env_inode(&e, EXT2_ROOT_INO, &in), 0);
    K_EXPECT_EQ(in.i_links_count, 2);
    K_EXPECT_EQ(fs->free_inodes, FORGED_INODES);
    K_EXPECT_EQ(fs->gd[0].bg_used_dirs_count, 1);
    K_EXPECT_EQ(ext2_ops.freespace(fs), (u64)FORGED_BLOCKS * 4096);

    env_close(&e);
}

KTEST("ext2", KTEST_LATE, test_ext2_mount_and_root);
KTEST("ext2", KTEST_LATE, test_ext2_block_io);
KTEST("ext2", KTEST_LATE, test_ext2_file_read_write);
KTEST("ext2", KTEST_LATE, test_ext2_directory_ops);
KTEST("ext2", KTEST_LATE, test_ext2_rename);
