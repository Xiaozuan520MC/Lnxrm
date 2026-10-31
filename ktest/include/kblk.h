/* RAM block device for ktest cases that must drive mount/partition code
 * against a volume they forge byte by byte.  The backing store covers only
 * the first `backed` sectors; a forged BPB may claim a giant geometry the
 * RAM never carries -- reads beyond it answer zeros, so code that checks
 * bounds before touching data still sees the shapes its checks act on. */
#ifndef LNXRM_KBLK_H
#define LNXRM_KBLK_H

#include <sys/vfs.h>
#include <abi/lnxrm_abi.h>
#include <types.h>

struct kblk {
    struct blkdev dev;
    u32 reads, writes; /* blk_io_* calls that reached us */
    u32 backed;        /* sectors covered by data[] */
    u8 *data;
};

struct kblk *kblk_make(const char *name, u64 sectors, u32 backed);
/* Pointer to a forged sector, or NULL when outside the backing store
 * (test cases assert the pointer before writing). */
u8 *kblk_sector(struct kblk *k, u64 lba);
void kblk_free(struct kblk *k);

#endif
