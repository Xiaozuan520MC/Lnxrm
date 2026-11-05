#pragma once
#include <types.h>

#ifdef __cplusplus
extern "C" {
#endif

enum vnode_type { V_REG, V_DIR, V_CHR };

struct file;

struct vnode {
    enum vnode_type type;
    u64 size;
    void *fs_data; /* fs-private node */
    struct fs_ops *ops;
    void *mnt_data; /* fs-private mount data (fs_ops.mnt) */
};

struct dirent_out {
    char name[56];
    u8 type;
    /* Stable object id within the filesystem (0 only if the fs cannot
     * produce one).  Forwarded verbatim into lnxrm_dirent.d_ino. */
    u64 ino;
};

/* Filesystem-specific operations. All paths are relative to mount root. */
struct fs_ops {
    int (*lookup)(void *mnt, const char *path, struct vnode *out);
    int (*getdent)(void *mnt, void *dirnode, u64 *cookie, struct dirent_out *d);
    int (*read)(void *mnt, void *node, u64 off, void *buf, size_t n);
    int (*write)(void *mnt, void *node, u64 off, const void *buf, size_t n);
    u64 (*size)(void *mnt, void *node); /* size as the fs has it right now */
    void (*close_node)(void *mnt, void *node); /* release a lookup's node */
    int (*create)(void *mnt, const char *path);
    int (*mkdir)(void *mnt, const char *path);
    int (*unlink)(void *mnt, const char *path);
    int (*rmdir)(void *mnt, const char *path);
    int (*rename)(void *mnt, const char *oldpath, const char *newpath);
    u64 (*freespace)(void *mnt);
};

struct file_ops {
    long (*read)(struct file *, void *buf, size_t n);
    long (*write)(struct file *, const void *buf, size_t n);
    long (*getdent)(struct file *, void *ubuf, size_t n);
    int (*close)(struct file *);
};

#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   0100
#define O_TRUNC   01000
#define O_APPEND  02000
#define O_CLOEXEC 010000

struct dir_iter {
    void *node;
    struct fs_ops *ops;
    u64 cookie;
    void *mnt_data;
    /* T-005: a scan that died with an I/O error stays dead.  The cookie no
     * longer means "resume here" once a transfer failed, so the error is
     * remembered and re-reported instead of the next getdent claiming the
     * directory simply ended. */
    int err;
};

struct file {
    struct file_ops *ops;
    u64 pos;
    void *priv; /* vnode / device private */
    bool is_dir;
    int flags;
    int refcnt;
};

/* Interrupt frame pushed by entry64.S (defined in sched.h). */
struct intr_frame;

void syscall_entry(struct intr_frame *f);
#ifdef CONFIG_KTEST
long syscall_probe(u64 nr, u64 a1, u64 a2, u64 a3); /* ktest only: dispatch, no user-entry */
#endif
long sys_open(const char *path, int flags);

void vfs_init(void);
/* Place every registered disk volume where C51 puts it: "/" only to one
 * carrying /bin/init, everything else to /mnt, /mnt2 or /mnt3.  Returns 0
 * only when "/" got a disk -- a data volume mounted is not that answer
 * (see vfs_data_volume_ready()). */
int vfs_try_mount_disk(void);
/* C51: does any volume answer *below* "/"?  Separates "no disk" from "a
 * disk, just not one this kernel can spawn /bin/init from". */
bool vfs_data_volume_ready(void);
/* Mount the initramfs that `make INITRAMFS=` linked into the kernel image
 * at "/", as a read-only root.  Returns LNXRM_ENOENT when the build carried
 * no archive or the bytes there are not a newc cpio archive -- a build
 * without INITRAMFS= behaves exactly as it did before this existed. */
int vfs_mount_initramfs(void);
/* Publish {ops, priv} under `prefix` and take it back out again (C47).
 * Neither is a syscall -- mounting stays a kernel decision -- but both are
 * the single implementation the disk probe and vfs_mount_initramfs() share,
 * so a second mount point is routed the same way and unmounted the same
 * way.  vfs_umount() answers LNXRM_ENOENT when nothing is mounted there. */
int vfs_mount_fs(struct fs_ops *ops, void *priv, const char *prefix);
int vfs_umount(const char *prefix);
/* Free space of the filesystem owning `path`, in bytes, or a negative LNXRM
 * error (C39: the one caller that makes fs_ops->freespace real). */
long vfs_free_space(const char *path);
/* Does `path` name a file that can be opened for reading?  Telling "the
 * disk mounted" apart from "the disk can boot": a FAT32 data disk with no
 * /bin/init on it mounts perfectly and still cannot start anything. */
bool vfs_file_exists(const char *path);
bool vfs_root_ready(void);    /* a filesystem answers at "/" (boot self-test) */
bool vfs_root_is_fat(void);   /* ...and it is the on-disk FAT32 volume (ENOSPC test) */
/* prefix a relative path with '/', returning `path` itself when it is
 * already absolute; NULL when the normalised path would not fit `buf` --
 * the caller answers LNXRM_ENAMETOOLONG instead of using a truncated
 * path.  Published for the boot self-test (C46, fixed). */
const char *vfs_abs_path(const char *path, char *buf, size_t bufsz);
long vfs_open_file(const char *path, int flags, struct file **out);
size_t vfs_file_size(struct file *f);
long vfs_read_file(struct file *f, void *buf, size_t n);
void vfs_close_file(struct file *f);

long sys_read(int fd, void *buf, size_t n);
long sys_write(int fd, const void *buf, size_t n);
long sys_close(int fd);
long sys_lseek(int fd, long off, int whence);
long sys_getdent(int fd, void *ubuf, size_t len);
long sys_dup2(int oldfd, int newfd);
long sys_mkdir(const char *upath);
long sys_unlink(const char *upath);
long sys_rmdir(const char *upath);
/* C27: make every cached dirty sector durable before returning. */
long sys_sync(void);
long sys_rename(const char *uold, const char *unew);

/* Capacity from an ATA IDENTIFY DEVICE data block, in 512-byte sectors.
 *
 * words 60-61 are the 28-bit field, and ATA-6/7 gives FFFFFFFFh in them a
 * second meaning: "48-bit is supported, read words 100-103 instead".  It is
 * also what that field saturates to on a disk >= 2 TiB.  Taking it verbatim
 * makes num_sectors = 0xFFFFFFFF, which `num_sectors / 2048` then prints as
 * 2097151 -- a number that looks measured but is just 32 ones truncated by
 * integer division, one megabyte short of 2 TiB.
 *
 * words 100-103 are 0 when a device does not implement them, so a non-zero
 * value there is authoritative (the usual case, and how Linux's
 * ata_id_n_sectors() reads it without needing word 83 either).  When neither
 * field yields a size the answer is 0 == "unknown": the caller must decline
 * to register the device rather than publish a fabricated capacity. */
static inline u64 ata_total_sectors(const u16 *id)
{
    u32 l28 = (u32)id[60] | ((u32)id[61] << 16);
    u64 l48 = (u64)id[100] | ((u64)id[101] << 16) | ((u64)id[102] << 32) |
              ((u64)id[103] << 48);
    if (l48) return l48;
    if (l28 == 0xFFFFFFFFu) return 0; /* sentinel: unknown, never 2 TiB */
    return l28;
}

/* block devices (drivers/ide.c + drivers/ahci.c register; fs/fat32.c consumes) */
struct blkdev {
    const char *name;
    u32 sector_size; /* bytes */
    u64 num_sectors;
    int (*read)(struct blkdev *, u64 lba, u32 count, void *buf);
    int (*write)(struct blkdev *, u64 lba, u32 count, const void *buf);
    void *drv;
};
/* Capacity of the block registry below.  It is a fixed array rather than a
 * growable list because nothing ever grows it at runtime: drivers register
 * at init, partitions at probe, and the count is what diskinfo reports. */
#define BLK_MAX 8

/* Publish `b` in the block registry that diskinfo enumerates (C28).
 * Returns 0, or LNXRM_EFAIL when it could not be taken -- the table is a
 * fixed array, and a full one refuses the newcomer out loud rather than
 * accepting it and dropping it. */
int blk_register(struct blkdev *b);

#ifdef CONFIG_KTEST
/* C28 test support only: move the registry out of the way of a case that
 * drives blk_register() to capacity, then put it back.  `count`/`at` read
 * what the registry would hand diskinfo. */
int blk_registry_take(void);
void blk_registry_put(void);
int blk_registry_count(void);
struct blkdev *blk_registry_at(int i);
#endif

#ifdef CONFIG_KTEST
/* C51 test support only (ktest/t_vfs.c).  vfs_root_mount() hands out the
 * root mount's {ops, priv} so a case can take "/" down and put the very
 * same volume back.  vfs_probe_home() drives the placement rules against
 * `bd`: 0 = it became the root, 1 = it took a data prefix, <0 = nothing
 * mountable, and `mounted` receives the mount object so the case can
 * release it after unmounting (fs_ops has no release hook). */
int vfs_root_mount(struct fs_ops **ops, void **priv);
int vfs_probe_home(struct blkdev *bd, void **mounted);
#endif

#ifdef __cplusplus
}
#endif
