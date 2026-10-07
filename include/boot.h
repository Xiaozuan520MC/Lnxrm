#pragma once
#include <types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Linux boot_params (zero page) subset we consume. */
struct e820_entry {
    u64 addr;
    u64 size;
    u32 type;
} __attribute__((packed));

struct boot_params {
    u8 _pad0[0x1e8];
    u8 e820_entries;
    u8 _pad1[0x2d0 - 0x1e9];
    struct e820_entry e820_map[128];
} __attribute__((packed));

#define E820_RAM 1

/* VBE mode descriptor that arch/setup.asm leaves at physical 0x8C00.
 * Layout MUST match the VBE_* equates in arch/setup.asm. */
struct vbe_lfb_info {
    u32 phys_addr;      /* VBE PhysBasePtr                        (0x8C00) */
    u32 pitch;          /* BytesPerScanLine                       (0x8C04) */
    u16 width;          /* XResolution                            (0x8C08) */
    u16 height;         /* YResolution                            (0x8C0A) */
    u8 bpp;             /* BitsPerPixel                           (0x8C0C) */
    u8 ok;              /* 1 when a mode was found and set        (0x8C0D) */
    u8 memory_model;    /* 4 = packed pixel, 6 = direct colour    (0x8C0E) */
    u8 r_pos;           /* RedFieldPosition                       (0x8C0F) */
    u8 r_size;          /* RedMaskSize                            (0x8C10) */
    u8 g_pos;           /* GreenFieldPosition                     (0x8C11) */
    u8 g_size;          /* GreenMaskSize                          (0x8C12) */
    u8 b_pos;           /* BlueFieldPosition                      (0x8C13) */
    u8 b_size;          /* BlueMaskSize                           (0x8C14) */
    u16 mode;           /* VBE mode number                        (0x8C15) */
} __attribute__((packed));

/* boot_params.screen_info subset: the LFB the boot loader programmed.
 * GRUB fills this in for the `linux` command when gfxpayload selects a
 * graphics mode (setup.asm never runs on that path).  Offsets are ABI. */

struct screen_lfb {
    u8 _pad0[0x0f];
    u8 orig_video_isVGA; /* 0x0f */
    u8 _pad1[0x12 - 0x10];
    u16 lfb_width;      /* 0x12 */
    u16 lfb_height;     /* 0x14 */
    u16 lfb_depth;      /* 0x16 */
    u32 lfb_base;       /* 0x18 */
    u32 lfb_size;       /* 0x1c, KiB */
    u16 _cl[2];         /* 0x20 */
    u16 lfb_linelength; /* 0x24, bytes per scan line */
    u8 red_size;        /* 0x26 */
    u8 red_pos;         /* 0x27 */
    u8 green_size;      /* 0x28 */
    u8 green_pos;       /* 0x29 */
    u8 blue_size;       /* 0x2a */
    u8 blue_pos;        /* 0x2b */
    u8 rsvd_size;       /* 0x2c */
    u8 rsvd_pos;        /* 0x2d */
    u8 _pad2[0x3a - 0x2e];
    u32 ext_lfb_base;   /* 0x3a, high 32 bits of the LFB address */
} __attribute__((packed));

/* Kernel-side description of the linear framebuffer. */
struct fb_info {
    u64 phys_addr;
    u32 width;
    u32 height;
    u32 pitch;
    u8 bpp;
    u8 ok;
    u8 memory_model;
    u8 r_pos, r_size;
    u8 g_pos, g_size;
    u8 b_pos, b_size;
    u16 mode; /* VBE mode number, 0 when the loader set the mode */
};

struct boot_info {
    char cmdline[512];
    struct e820_entry map[64];
    int map_len;
    struct fb_info fb;
};

extern struct boot_info bootinfo;

#ifndef __cplusplus
_Static_assert(sizeof(struct screen_lfb) == 0x3e, "screen_info layout");
_Static_assert(offsetof(struct screen_lfb, lfb_width) == 0x12, "screen_info layout");
_Static_assert(offsetof(struct screen_lfb, lfb_linelength) == 0x24, "screen_info layout");
_Static_assert(offsetof(struct screen_lfb, ext_lfb_base) == 0x3a, "screen_info layout");
_Static_assert(sizeof(struct vbe_lfb_info) == 0x17, "setup.asm block layout");
#endif

/* start_kernel: C entry, called from entry64.S */
void start_kernel(void);

#ifdef __cplusplus
}
#endif
