// SPDX-License-Identifier: Apache-2.0

/* Shared-frame cache behind MAP_SHARED file mappings: two processes mapping
 * the same (file, offset) must see each other's writes, so they need the ONE
 * frame allocated and filled for it.  MAP_PRIVATE stays out -- it owns its
 * bytes (see pagefault.c).  Ownership is the ordinary order-0 refcount
 * (mm/pmm.c): the cache holds one, the caller gets one for its PTE, and the
 * unmap drops it, so the frame dies with whichever goes last.  Entries go when
 * the file's last reference dies (vfs_close_file), which cannot happen while
 * any mapping still holds a frame.
 *
 * KNOWN LIMITATION (deliberate): write() does not update an already-cached
 * frame, so do not write() a file somebody has MAP_SHARED-mapped. */
#pragma once
#include <types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct file;

/* THE frame backing `off` of `f`, or 0.  On success the caller owns one
 * reference, which it transfers to the PTE it installs (and which the eventual
 * unmap drops with pmm_free).  `off` must be page-aligned; the file is read
 * only on the first request for an offset. */
u64 filepage_get(struct file *f, u64 off);

/* Forget every frame belonging to `f` and drop the cache's reference on each.
 * Called from vfs_close_file when the file's last reference dies; `f` is only
 * ever compared, never dereferenced. */
void filepage_drop_file(struct file *f);

#ifdef CONFIG_KTEST
/* Entries currently held.  ktest pins the whole lifecycle with it -- a leaked
 * entry would otherwise surface much later as a frame that never returns. */
u64 filepage_resident(void);
#endif

#ifdef __cplusplus
}
#endif
