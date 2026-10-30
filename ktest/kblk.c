/* The device half of kblk.h.  Raw blkdev, no cache, no injection: this is
 * the lowest layer the parser and mount code see. */
#include "kblk.h"
#include <sys/ktest.h>
#include <mm/mm.h>
#include <string.h>

static int kblk_read(struct blkdev *dev, u64 lba, u32 count, void *buf)
{
    struct kblk *k = dev->drv;
    if (lba + count > dev->num_sectors) return LNXRM_EIO;
    k->reads++;
    u8 *dst = buf;
    for (u32 s = 0; s < count; s++, lba++) {
        if (lba < k->backed)
            memcpy(dst, k->data + lba * 512, 512);
        else
            memset(dst, 0, 512);
        dst += 512;
    }
    return 0;
}

static int kblk_write(struct blkdev *dev, u64 lba, u32 count, const void *buf)
{
    struct kblk *k = dev->drv;
    if (lba + count > dev->num_sectors || lba + count > k->backed) return LNXRM_EIO;
    k->writes++;
    memcpy(k->data + lba * 512, buf, (size_t)count * 512);
    return 0;
}

struct kblk *kblk_make(const char *name, u64 sectors, u32 backed)
{
    struct kblk *k = kmalloc(sizeof(*k));
    if (!k) return NULL;
    memset(k, 0, sizeof(*k));
    k->backed = backed;
    k->data = kmalloc((size_t)backed * 512);
    if (!k->data) {
        kfree(k);
        return NULL;
    }
    memset(k->data, 0, (size_t)backed * 512);
    k->dev.name = name;
    k->dev.sector_size = 512;
    k->dev.num_sectors = sectors;
    k->dev.read = kblk_read;
    k->dev.write = kblk_write;
    k->dev.drv = k;
    return k;
}

u8 *kblk_sector(struct kblk *k, u64 lba)
{
    /* NULL instead of K_ASSERT: the helper can return a value, so the
     * loud refusal happens at the call site's K_ASSERT(ptr). */
    if (lba >= k->backed) return NULL;
    return k->data + lba * 512;
}

void kblk_free(struct kblk *k)
{
    if (!k) return;
    kfree(k->data);
    kfree(k);
}
