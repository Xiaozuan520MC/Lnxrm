/* shm_open(51) / shm_unlink(52): named shared-memory objects (S0).
 *
 * A shm object is the minimum a microkernel needs to let two address spaces
 * agree on bytes: the kernel keeps a NAME -> object table and hands every
 * opener THE SAME struct file, so mm/filepage.c's shared-frame cache (whose
 * key is the struct file pointer) finds one entry for the pair and both
 * mappings are installed over one physical frame.  Nothing else -- no
 * ownership, no byte order, no synchronisation -- lives here; those are
 * userland's business.
 *
 * The descriptor's shape is deliberately the ordinary one: a V_REG vnode
 * behind file->priv (never `drv`, so C42 stays true and sys_mmap's
 * "!priv / type != V_REG" gate opens for it), a size fixed at creation, a
 * read door that answers zeros and a write door that answers success and
 * stores nothing -- the frames ARE the bytes, so there is nowhere for a
 * write-back to put them.  That is what lets mmap, the demand pager, the
 * write-back path and fork's shared-VMA handling all be reused untouched.
 *
 * Lifetime is two independent lines, and the entry's own reference is what
 * joins them: shm_open() gives the entry ONE reference on the file that no
 * descriptor ever drops, and shm_unlink() is the only thing that releases
 * it.  Until then the object -- name, size, frames -- survives every close,
 * which is POSIX's "the object lives until unlinked" and is also what makes
 * a rendezvous work when the two ends never overlap their open windows.
 * The entry itself is freed by fs_ops->close_node, which runs exactly once
 * when the file's last reference goes (i.e. after unlink AND after the last
 * descriptor and mapping are gone).
 */
#pragma once
#include <types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Longest object name, NUL included: 31 usable bytes.  The leading '/' of
 * the POSIX spelling ("/x") is accepted and dropped, so "/x" and "x" name
 * one object; a name that is empty once that is done, or longer than this,
 * is EINVAL / ENAMETOOLONG. */
#define LNXRM_SHM_NAME_MAX 32

/* Largest object a create may ask for (1 GiB), and the most that may exist
 * at once (ENOSPC past that).  Neither is a promise of RAM: an object costs
 * one entry + one struct file until it is touched, and every frame after
 * that is metered against cred_mem_quota() by the demand pager exactly as
 * an anonymous or file-backed page is, so a large `size` cannot be used to
 * sidestep the per-task cap. */
#define LNXRM_SHM_MAX_SIZE    (1ULL << 30)
#define LNXRM_SHM_MAX_OBJECTS 64

/* shm_open(name, flags, size) -> descriptor, or a negative LNXRM error:
 * EINVAL (no name / size 0 / unknown flag bit), ENAMETOOLONG, ENOENT (no
 * such object and no O_CREAT), EEXIST (O_CREAT|O_EXCL and it exists),
 * ENOMEM (size above LNXRM_SHM_MAX_SIZE or no memory), ENOSPC (too many
 * objects), EMFILE (no free descriptor slot).
 *
 * `size` is honoured only when this call CREATES the object; it is rounded
 * up to a page and then fixed forever.  On an existing object it is
 * ignored, which is what lets the second end of a rendezvous pass 0. */
long sys_shm_open(const char *name, int flags, u64 size);
/* shm_unlink(name) -> 0, or ENOENT / EINVAL / ENAMETOOLONG.  The name goes
 * away at once; the object does not, and descriptors and mappings of it
 * keep working until their last reference goes. */
long sys_shm_unlink(const char *name);

#ifdef CONFIG_KTEST
/* How many entries exist right now, including unlinked ones still held open
 * -- the number close_node() will subtract from.  ktest uses it to prove the
 * entry dies with the file's last reference rather than leaking. */
u32 shm_live_objects(void);
#endif

#ifdef __cplusplus
}
#endif
