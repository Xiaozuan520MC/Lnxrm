// SPDX-License-Identifier: Apache-2.0

#ifndef __LNXRM_INFLATE_H
#define __LNXRM_INFLATE_H

#include <types.h>

/* Inflate a gzip stream from src into dst.
 *
 * src:      compressed gzip data
 * src_len:  length of compressed data
 * dst:      output buffer (must be large enough for the full stream)
 * dst_len:  on entry, size of dst; on success, updated to the actual
 *           number of bytes produced.
 *
 * Returns 0 on success, negative on error.
 */
int inflate(const void *src, size_t src_len, void *dst, size_t *dst_len);

#endif /* __LNXRM_INFLATE_H */
