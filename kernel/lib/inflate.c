// SPDX-License-Identifier: Apache-2.0
/* lnxrm minimal gzip inflate for boot-time decompression.
 *
 * Supports:
 *   - gzip header (magic 1f 8b, CM=8, FLG fields skipped)
 *   - DEFLATE block types 00 (stored), 01 (fixed Huffman), 10 (dynamic Huffman)
 *   - 32 KiB sliding window with LZ77 back-references
 *   - gzip trailer (CRC32 + ISIZE)
 *
 * Does NOT support: block type 11 (reserved), multi-member gzip, FEXTRA/FNAME
 * comments beyond what the header skip needs.  This is sufficient for the
 * output of `gzip -n` on a single file.
 */

#include "inflate.h"
#include <types.h>

/* freestanding: no libc.  Hand-rolled copies avoid clang built-in
 * expansion (which would need libgcc) in the boot stub. */
static void inflate_memset(void *s, int c, size_t n)
{
    u8 *p = (u8 *)s;
    for (size_t i = 0; i < n; i++)
        p[i] = (u8)c;
}

static void inflate_memcpy(void *dest, const void *src, size_t n)
{
    u8 *d = (u8 *)dest;
    const u8 *s = (const u8 *)src;
    for (size_t i = 0; i < n; i++)
        d[i] = s[i];
}

/* ---- CRC32 (ISO 3309, reflected polynomial 0xEDB88320) ---- */
static u32 crc32_table[256];

static void crc32_init(void)
{
    for (u32 i = 0; i < 256; i++) {
        u32 c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
}

static u32 crc32_update(u32 crc, const u8 *buf, size_t len)
{
    crc ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        crc = crc32_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/* ---- bit reader ---- */
struct bitreader {
    const u8 *in;
    size_t in_pos;
    size_t in_len;
    u32 bitbuf;
    int bitcnt;
};

static int bit_fill(struct bitreader *br, int need)
{
    while (br->bitcnt < need) {
        if (br->in_pos >= br->in_len)
            return -1; /* out of input */
        br->bitbuf |= (u32)br->in[br->in_pos++] << br->bitcnt;
        br->bitcnt += 8;
    }
    return 0;
}

static u32 bit_read(struct bitreader *br, int n)
{
    if (n == 0)
        return 0;
    if (bit_fill(br, n) < 0)
        return 0xFFFFFFFFu;
    u32 val = br->bitbuf & ((1u << n) - 1);
    br->bitbuf >>= n;
    br->bitcnt -= n;
    return val;
}

/* ---- canonical Huffman tables ---- */
#define MAX_BITS 15
#define MAX_CODES 288

struct huffman {
    u16 start[MAX_BITS + 1];  /* first code of each bit length */
    u16 count[MAX_BITS + 2];  /* cumulative symbol count up to each length */
    u16 symbol[MAX_CODES];    /* symbols sorted by (length, code) */
};

static int huffman_build(struct huffman *h, const u8 *lengths, int num)
{
    u32 bl_count[MAX_BITS + 1] = {0};

    for (int i = 0; i < num; i++)
        if (lengths[i])
            bl_count[lengths[i]]++;

    u32 code = 0;
    bl_count[0] = 0;
    for (int bits = 1; bits <= MAX_BITS; bits++) {
        code = (code + bl_count[bits - 1]) << 1;
        h->start[bits] = (u16)code;
    }

    u32 offset = 0;
    for (int bits = 1; bits <= MAX_BITS; bits++) {
        h->count[bits] = (u16)offset;
        for (int i = 0; i < num; i++)
            if (lengths[i] == bits)
                h->symbol[offset++] = (u16)i;
        h->count[bits + 1] = (u16)offset;
    }
    h->count[0] = 0;
    return 0;
}


/* Decode one symbol. Returns symbol value, or -1 on error. */
static int huffman_decode(struct bitreader *br, struct huffman *h)
{
    u32 code = 0;
    int len = 0;
    u32 first = 0;

    for (;;) {
        code = (code << 1) | bit_read(br, 1);
        len++;
        u32 cnt = h->count[len + 1] - h->count[len];
        if (cnt != 0) {
            first = h->start[len];
            if (code - first < cnt)
                return h->symbol[h->count[len] + (code - first)];
        }
        if (len >= MAX_BITS)
            return -1;
        first <<= 1;
    }
}

/* ---- fixed Huffman tables (RFC 1951) ---- */
static struct huffman fixed_litlen;
static struct huffman fixed_dist;
static int fixed_tables_ready = 0;

static void fixed_tables_init(void)
{
    u8 lengths[MAX_CODES];
    inflate_memset(lengths, 0, sizeof(lengths));
    for (int i = 0; i <= 143; i++) lengths[i] = 8;
    for (int i = 144; i <= 255; i++) lengths[i] = 9;
    for (int i = 256; i <= 279; i++) lengths[i] = 7;
    for (int i = 280; i <= 287; i++) lengths[i] = 8;
    huffman_build(&fixed_litlen, lengths, 288);

    for (int i = 0; i < 32; i++) lengths[i] = 5;
    huffman_build(&fixed_dist, lengths, 32);

    fixed_tables_ready = 1;
}

/* ---- LZ77 length/distance base and extra-bit tables (RFC 1951) ---- */
static const u16 length_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const u8 length_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const u16 dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577
};
static const u8 dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11,
    12, 12, 13, 13
};

#define WINDOW_SIZE 32768
#define WINDOW_MASK (WINDOW_SIZE - 1)

static u8 window[WINDOW_SIZE];
static u32 window_pos;

/* ---- output window ---- */
static int window_put(u8 byte)
{
    window[window_pos & WINDOW_MASK] = byte;
    window_pos++;
    return 0;
}

/* ---- inflate block processing ---- */

/* Forward declaration: defined after inflate_block() */
static int inflate_huffman_block(struct bitreader *br, u8 *out, size_t *out_pos,
                                 size_t out_size, struct huffman *litlen,
                                 struct huffman *dist);

static int inflate_block(struct bitreader *br, u8 *out, size_t *out_pos,
                         size_t out_size)
{
    int last = bit_read(br, 1);
    if (last < 0)
        return -1;
    int type = bit_read(br, 2);
    if (type < 0)
        return -1;

    switch (type) {
    case 0: { /* stored */
        /* skip to byte boundary */
        br->bitcnt &= ~7;
        br->bitbuf = 0;
        u32 len = br->in[br->in_pos++] | (br->in[br->in_pos++] << 8);
        u32 nlen = br->in[br->in_pos++] | (br->in[br->in_pos++] << 8);
        if ((u16)len != (u16)(~nlen))
            return -1;
        if ((size_t)(br->in_pos + len) > br->in_len)
            return -1;
        if (*out_pos + len > out_size)
            return -1;
        inflate_memcpy(out + *out_pos, br->in + br->in_pos, len);
        br->in_pos += len;
        *out_pos += len;
        break;
    }

    case 1: { /* fixed Huffman */
        if (!fixed_tables_ready)
            fixed_tables_init();
        struct huffman *litlen = &fixed_litlen;
        struct huffman *dist = &fixed_dist;
        if (inflate_huffman_block(br, out, out_pos, out_size,
                                  litlen, dist) < 0)
            return -1;
        return last;
    }

    case 2: { /* dynamic Huffman */
        u8 litlen_lengths[MAX_CODES];
        u8 dist_lengths[32];
        struct huffman litlen_h, dist_h;

        int hlit = bit_read(br, 5) + 257;
        int hdist = bit_read(br, 5) + 1;
        int hclen = bit_read(br, 4) + 4;
        if (hlit < 0 || hdist < 0 || hclen < 0)
            return -1;

        static const u8 cl_order[19] = {
            16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
        };
        u8 cl_lengths[19] = {0};
        for (int i = 0; i < hclen; i++) {
            int v = bit_read(br, 3);
            if (v < 0)
                return -1;
            cl_lengths[cl_order[i]] = (u8)v;
        }
        struct huffman cl_h;
        huffman_build(&cl_h, cl_lengths, 19);

        int total = hlit + hdist;
        int idx = 0;
        while (idx < total) {
            int sym = huffman_decode(br, &cl_h);
            if (sym < 0)
                return -1;
            if (sym < 16) {
                if (idx < hlit)
                    litlen_lengths[idx] = (u8)sym;
                else
                    dist_lengths[idx - hlit] = (u8)sym;
                idx++;
            } else if (sym == 16) {
                u32 rep = bit_read(br, 2) + 3;
                /* RFC 1951 3.2.7: repeat the last code length, whether it was
                 * a literal/length code or a distance code.  When idx == hlit
                 * (first distance code), the previous code is the last
                 * litlen code (litlen_lengths[hlit-1]), not dist_lengths[-1]. */
                u8 prev = (idx == 0) ? 0 :
                    (idx <= hlit) ? litlen_lengths[idx - 1] : dist_lengths[idx - hlit - 1];
                for (u32 i = 0; i < rep; i++) {
                    if (idx >= total)
                        return -1;
                    if (idx < hlit)
                        litlen_lengths[idx] = prev;
                    else
                        dist_lengths[idx - hlit] = prev;
                    idx++;
                }
            } else if (sym == 17) {
                u32 rep = bit_read(br, 3) + 3;
                for (u32 i = 0; i < rep; i++) {
                    if (idx >= total)
                        return -1;
                    if (idx < hlit)
                        litlen_lengths[idx] = 0;
                    else
                        dist_lengths[idx - hlit] = 0;
                    idx++;
                }
            } else if (sym == 18) {
                u32 rep = bit_read(br, 7) + 11;
                for (u32 i = 0; i < rep; i++) {
                    if (idx >= total)
                        return -1;
                    if (idx < hlit)
                        litlen_lengths[idx] = 0;
                    else
                        dist_lengths[idx - hlit] = 0;
                    idx++;
                }
            } else {
                return -1;
            }
        }

        huffman_build(&litlen_h, litlen_lengths, hlit);
        huffman_build(&dist_h, dist_lengths, hdist);
        if (inflate_huffman_block(br, out, out_pos, out_size,
                                  &litlen_h, &dist_h) < 0)
            return -1;
        return last;
    }

    default:
        return -1; /* block type 3 is reserved */
    }

    return last;
}

/* Shared Huffman block body: decodes symbols until end-of-block (256). */
static int inflate_huffman_block(struct bitreader *br, u8 *out, size_t *out_pos,
                                 size_t out_size, struct huffman *litlen,
                                 struct huffman *dist)
{
    for (;;) {
        int sym = huffman_decode(br, litlen);
        if (sym < 0)
            return -1;

        if (sym < 256) {
            /* literal */
            if (*out_pos >= out_size)
                return -1;
            window_put((u8)sym);
            out[*out_pos] = (u8)sym;
            (*out_pos)++;
        } else if (sym == 256) {
            /* end of block */
            return 0;
        } else {
            /* length/distance pair */
            int len_index = sym - 257;
            if (len_index < 0 || len_index >= 29)
                return -1;
            u32 length = length_base[len_index] + bit_read(br, length_extra[len_index]);
            if (length > 0xFFFFFFFF)
                return -1;

            int dist_sym = huffman_decode(br, dist);
            if (dist_sym < 0 || dist_sym >= 30)
                return -1;
            u32 distance = dist_base[dist_sym] + bit_read(br, dist_extra[dist_sym]);
            if (distance == 0 || distance > window_pos)
                return -1;

            if (*out_pos + length > out_size)
                return -1;

            for (u32 i = 0; i < length; i++) {
                u8 b = window[(window_pos - distance) & WINDOW_MASK];
                window_put(b);
                out[*out_pos] = b;
                (*out_pos)++;
            }
        }
    }
}

/* ---- gzip header ---- */
static int gzip_header(struct bitreader *br, const u8 *in, size_t in_len,
                       size_t *in_pos)
{
    /* Magic + method + flags */
    if (*in_pos + 10 > in_len)
        return -1;
    if (in[*in_pos] != 0x1f || in[*in_pos + 1] != 0x8b)
        return -1;
    if (in[*in_pos + 2] != 0x08)
        return -1;
    u8 flg = in[*in_pos + 3];
    *in_pos += 10; /* skip magic(2) method(1) flags(1) mtime(4) xfl(1) os(1) */

    /* Optional fields */
    if (flg & 0x02) { /* FEXTRA */
        if (*in_pos + 2 > in_len)
            return -1;
        u16 xflen = in[*in_pos] | (in[*in_pos + 1] << 8);
        *in_pos += 2 + xflen;
        if (*in_pos > in_len)
            return -1;
    }
    if (flg & 0x04) { /* FNAME */
        while (*in_pos < in_len && in[*in_pos])
            (*in_pos)++;
        (*in_pos)++;
    }
    if (flg & 0x08) { /* FCOMMENT */
        while (*in_pos < in_len && in[*in_pos])
            (*in_pos)++;
        (*in_pos)++;
    }
    if (flg & 0x10) { /* FHCRC */
        *in_pos += 2;
    }
    return 0;
}

/* ---- public inflate ---- */
int inflate(const void *src, size_t src_len, void *dst, size_t *dst_len)
{
    /* No static "initialized" flag: on the bare-metal target inflate.elf is
     * copied as a raw binary (.bss is NOT zeroed), so every entry must
     * (re)initialize the static tables.  These writes are idempotent and
     * cheap, so this is correct for both single and repeated calls. */
    crc32_init();
    fixed_tables_init();
    window_pos = 0;

    const u8 *in = (const u8 *)src;
    u8 *out = (u8 *)dst;
    size_t out_pos = 0;

    if (*dst_len < 1)
        return -1;

    struct bitreader br;
    br.in = in;
    br.in_pos = 0;
    br.in_len = src_len;
    br.bitbuf = 0;
    br.bitcnt = 0;

    /* skip gzip header (byte-based, not bit-based) */
    size_t hdr_end = 0;
    if (gzip_header(&br, in, src_len, &hdr_end) < 0)
        return -1;

    /* realign bit reader to the start of compressed data */
    br.in_pos = hdr_end;
    br.bitbuf = 0;
    br.bitcnt = 0;

    int last = 0;
    while (!last) {
        int rc = inflate_block(&br, out, &out_pos, *dst_len);
        if (rc < 0)
            return -1;
        last = rc;
    }

    /* byte-align */
    br.bitcnt &= ~7;
    br.bitbuf = 0;
    br.in_pos += br.bitcnt / 8;
    br.bitcnt = 0;

    /* gzip trailer: CRC32 (4) + ISIZE (4) */
    if (br.in_pos + 8 > src_len)
        return -1;
    u32 crc_expected = in[br.in_pos] | (in[br.in_pos + 1] << 8) |
                       (in[br.in_pos + 2] << 16) | (in[br.in_pos + 3] << 24);
    u32 isize_expected = in[br.in_pos + 4] | (in[br.in_pos + 5] << 8) |
                         (in[br.in_pos + 6] << 16) | (in[br.in_pos + 7] << 24);

    u32 crc_actual = crc32_update(0, out, out_pos);
    if (crc_actual != crc_expected)
        return -1;
    if ((u32)out_pos != isize_expected)
        return -1;

    *dst_len = out_pos;
    return 0;
}

/* ---- entry point called from decompress_head.S (linked at 0x200000) ----
 *
 * System V AMD64 ABI:
 *   rdi = src   (compressed gzip data, copied to 0x300000)
 *   rsi = dst   (output destination, 0x100000)
 *   rdx = src_len (compressed size)
 *   rcx = entry (physical address of the kernel's 64-bit entry, _start64)
 *-------------------------------------------------------------------------*/
void inflate_main(void *src, void *dst, size_t src_len, void *entry)
{
    /* NOTE: no .bss zeroing here.  inflate() re-initializes all static
     * tables on every call (crc32_table, fixed Huffman tables), and
     * window_pos starts at 0 so LZ77 back-references only read previously
     * written window entries.  The binary copy at 0x200000 therefore runs
     * correctly without any BSS initialization. */

    size_t out_size = 0x400000; /* 4 MiB output window at 0x100000 */
    if (inflate(src, src_len, dst, &out_size) < 0) {
        /* Decompression failed: halt. */
        for (;;) __asm__ volatile("cli; hlt");
    }

    /* Jump to the decompressed kernel's 64-bit entry (_start64).  Do NOT
     * jump to dst (0x100000): that is _start32, 32-bit code which faults
     * under the long-mode CS. */
    __asm__ volatile("jmp *%0" :: "r" (entry));
    for (;;) __asm__ volatile("cli; hlt");
}
