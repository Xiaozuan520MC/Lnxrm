/* The shared-frame cache behind MAP_SHARED file mappings.
 *
 * Two processes that map the same file at the same offset must see each
 * other's writes, and the only way that can be true with ordinary demand
 * paging is if both of their PTEs name the SAME physical frame.  So a
 * mapping of a file offset does not simply allocate a frame and read the
 * file into it (that is what MAP_PRIVATE does -- see pagefault.c): it asks
 * here for THE frame for (file, offset), which is allocated and filled
 * once and then handed to every mapping that wants it.
 *
 * Ownership of a frame is the ordinary order-0 reference count (mm/pmm.c):
 * the cache entry holds one reference, and filepage_get() hands the caller
 * one more for the PTE it is about to install.  Every unmap path already
 * pmm_free()s what it unmapped, so a mapping's reference goes away with
 * the mapping and the cache's goes away with the file -- whichever is last
 * frees the frame.  Nothing here therefore has to know how many address
 * spaces currently point at a page.
 *
 * MAP_PRIVATE deliberately does NOT come through here: a private mapping
 * owns its bytes, so sharing the frame would make one process's write
 * visible in another's (and in the file).  It gets a frame of its own.
 *
 * Lifetime: entries are dropped when the LAST reference on the struct file
 * goes away (vfs_close_file).  A VMA holds a reference on its file for as
 * long as the range is mapped, so that moment cannot arrive while any
 * mapping of the file still exists -- the cache outlives every PTE that
 * was filled from it, and no address space can be holding a frame whose
 * entry has already been forgotten.
 *
 * KNOWN LIMITATION (deliberate, documented rather than papered over): the
 * cache and the write() path do not know about each other.  A store()
 * through a regular descriptor updates the disk, not a frame already
 * cached for that offset -- so a MAP_SHARED mapping of a file that is also
 * being write()d shows stale bytes until it is unmapped, and if its page is
 * dirty the write-back overwrites what write() put there.  Linux never
 * faces this because one page cache serves both doors; here it would take
 * unmapping every PTE of that file range across every address space (which
 * nothing can do cheaply today), so the rule instead is: do not write() a
 * file that somebody has MAP_SHARED-mapped at the same time.
 */
#pragma once
#include <types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct file;

/* THE frame backing `off` of `f`, or 0.  On success the caller owns one
 * reference, which it transfers to the PTE it installs (and which the
 * eventual unmap drops with pmm_free).  `off` must be page-aligned; the
 * file is read only on the first request for an offset. */
u64 filepage_get(struct file *f, u64 off);

/* Forget every frame belonging to `f` and drop the cache's reference on
 * each.  Called from vfs_close_file at the moment the file's last
 * reference dies; `f` is only ever compared, never dereferenced. */
void filepage_drop_file(struct file *f);

#ifdef CONFIG_KTEST
/* Entries currently held.  ktest pins the whole lifecycle with it: a case
 * maps a file, checks the count went up, closes it, and checks the cache
 * came back down -- a leaked entry would otherwise show up much later as a
 * frame that never returns to the buddy. */
u64 filepage_resident(void);
#endif

#ifdef __cplusplus
}
#endif
