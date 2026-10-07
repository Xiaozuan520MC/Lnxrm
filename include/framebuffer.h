#pragma once
#include <types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* sanity limit on a mode's dimensions; the real ceiling is FB_MAX_BYTES */
#define FB_MAX_DIM 8192

#define FB_VMA 0xffffffff8d000000UL /* PD_HI[104], 4 KiB pages */

/* The framebuffer lives in its own high-half PD window: slots
 * [FB_PD_BASE, FB_PD_BASE + FB_PD_SLOTS) = [104,112) = 16 MiB.  Those PD
 * entries are not physical aliases, so the pages they would otherwise map
 * have no high-half window left and must stay out of the buddy allocator
 * (see start_kernel). */
#define FB_PD_BASE    104
#define FB_PD_SLOTS   8
#define FB_MAX_BYTES  (FB_PD_SLOTS * 0x200000UL)

/* runtime values set from VBE mode info */
extern u32 fb_width;
extern u32 fb_height;
extern u32 fb_pitch;
extern u32 fb_bpp;   /* actual depth of the running mode (8/15/16/24/32) */
extern u32 fb_pxsz;  /* bytes per stored pixel */

void fb_init(void);

/* drawing primitives shared with the fb_* syscalls (24-28) and the
 * console above; all clip to the visible area.  Every colour argument is a
 * true-colour 0x00RRGGBB value (24-bit RGB carried in a 32-bit word); there
 * is no palette lookup anywhere in the drawing path. */
void fb_clear(u32 color);
void fb_fill_rect(u32 x, u32 y, u32 w, u32 h, u32 color);
void fb_draw_char(u32 x, u32 y, char ch, u32 fg, u32 bg);
void fb_puts(u32 x, u32 y, const char *s, u32 fg, u32 bg);

/* ---- ownership (T-033): one surface, one owner ----
 * CAP_FB (T-031) answers "may this process draw at all".  It cannot
 * answer "which of two privileged processes is talking to the screen",
 * which is what this record answers: the first drawing call claims the
 * surface, every later drawer is refused until the holder exits.
 *
 * The state lives in the driver, but nothing in the drawing path above
 * consults it -- those primitives are shared with the text console, and a
 * console that could be refused by a user process would be a console a
 * user process could mute.  Enforcement happens in the syscall layer
 * (fb_owner_gate in kernel/syscall.c); only the record and the two
 * operations on it live here.
 *
 * Release is deliberately not here either: the holder is a pid, and a
 * pid stops meaning anything when its task dies, so sys_exit drops it
 * (kernel/task.c).  Every way a task dies ends in sys_exit -- the syscall
 * itself, SIGKILL, a terminating signal, the #PF/#GP default -- and pids
 * are handed out by a counter that never reuses them, so one release
 * point covers everything that can happen. */
u32 fb_owner_get(void);   /* the pid holding it; 0 = free */
bool fb_claim(u32 pid);   /* may `pid` draw?  Claims a free surface */
void fb_disown(u32 pid);  /* clear it -- only if `pid` is the holder */

/* ---- self-test hooks (ktest/t_fb.c, t_draw.c) ----
 * fb_pack()/fb_rgb_px() are pure but file-static and the channel layout is
 * private state, so these let the boot self-test drive every bit layout a
 * loader could hand us and read back what a drawing call actually stored.
 * Callers that change the layout must restore it with fb_layout_get(). */
struct fb_layout {
    u32 r_pos, r_size;
    u32 g_pos, g_size;
    u32 b_pos, b_size;
    u32 pxsz; /* bytes per stored pixel (1 = paletted 8bpp) */
};
void fb_layout_get(struct fb_layout *l);
void fb_layout_set(const struct fb_layout *l);
u32 fb_pack(u8 r, u8 g, u8 b);   /* 8-bit channels -> stored pixel */
u32 fb_rgb_px(u32 rgb);          /* 0x00RRGGBB -> stored pixel */
bool fb_read_px(u32 x, u32 y, u32 *out); /* false when off-screen/unmapped */
void fb_write_px(u32 x, u32 y, u32 raw); /* stored value, no colour conversion */

/* framebuffer text console (mirrors VGA text mode) */
void fb_console_init(void);
void fb_console_putc(char c);
/* Cursor and grid of the frame buffer console, for the boot self-test.
 * Any pointer may be NULL. */
void fb_console_get(int *row, int *col, int *rows, int *cols);
extern int fb_active;

/* console cell size in pixels (8x16 font) */
#define FB_CELL_W 8
#define FB_CELL_H 16

/* 16 named colours, each a 0x00RRGGBB value usable as any fb_* colour */
#define FB_CLR_BLACK    0x000000
#define FB_CLR_GREEN    0x008000
#define FB_CLR_NAVY     0x000080
#define FB_CLR_PURPLE   0x800080
#define FB_CLR_TEAL     0x008080
#define FB_CLR_LGREY    0xc0c0c0
#define FB_CLR_DGREY    0x808080
#define FB_CLR_RED      0xff0000
#define FB_CLR_BROWN    0xff8040
#define FB_CLR_WHITE    0xffffff
/* bright / saturated extras */
#define FB_CLR_LBLUE    0x6699ff
#define FB_CLR_LGREEN   0x66ff66
#define FB_CLR_LCYAN    0x66ffff
#define FB_CLR_LRED     0xff6666
#define FB_CLR_LMAGENTA 0xff66ff
#define FB_CLR_YELLOW   0xffff00

#ifdef __cplusplus
}
#endif
