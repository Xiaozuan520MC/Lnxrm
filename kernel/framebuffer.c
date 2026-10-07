/* lnxrm -- VBE linear framebuffer driver + graphical console.
 * Supports 8 / 15 / 16 / 24 / 32 bits per pixel.  Every colour handed to
 * the drawing primitives is a true-colour 0x00RRGGBB value; fb_rgb_px()
 * turns it into a stored pixel (packed channels, or the nearest DAC cube
 * entry on 8bpp indexed modes). */
#include <framebuffer.h>
#include <console.h>
#include <boot.h>
#include <mm/mm.h>
#include <io.h>
#include <font8x16.h>

extern struct boot_info bootinfo;

static volatile u8 *fb_ptr;   /* what every fb_* routine draws into */
static volatile u8 *fb_front; /* the visible LFB */

/* fb_put_pixel is internal to this file; the other four are exported and
 * serve both the text console and the fb_* syscalls (24-28). */
static void fb_put_pixel(u32 x, u32 y, u32 color);


u32 fb_pitch;
u32 fb_bpp = 8;
u32 fb_pxsz = 1; /* bytes per stored pixel */

/* channel layout of the current mode (bit positions inside the pixel) */
static u32 fb_r_pos, fb_r_size;
static u32 fb_g_pos, fb_g_size;
static u32 fb_b_pos, fb_b_size;

/* ---- VGA DAC palette (standard 16-color) ---- */
static const u8 palette16[16][3] = {
    {0x00, 0x00, 0x00}, /*  0 black       */
    {0x00, 0x00, 0xAA}, /*  1 blue        */
    {0x00, 0xAA, 0x00}, /*  2 green       */
    {0x00, 0xAA, 0xAA}, /*  3 cyan        */
    {0xAA, 0x00, 0x00}, /*  4 red         */
    {0xAA, 0x00, 0xAA}, /*  5 magenta     */
    {0xAA, 0x55, 0x00}, /*  6 brown       */
    {0xAA, 0xAA, 0xAA}, /*  7 light grey  */
    {0x55, 0x55, 0x55}, /*  8 dark grey   */
    {0x55, 0x55, 0xFF}, /*  9 light blue  */
    {0x55, 0xFF, 0x55}, /* 10 light green */
    {0x55, 0xFF, 0xFF}, /* 11 light cyan  */
    {0xFF, 0x55, 0x55}, /* 12 light red   */
    {0xFF, 0x55, 0xFF}, /* 13 light magenta*/
    {0xFF, 0xFF, 0x55}, /* 14 yellow      */
    {0xFF, 0xFF, 0xFF}, /* 15 white       */
};

/* pack an 8-bit-per-channel colour into a framebuffer pixel.
 * Exported for the boot self-test, which walks every layout. */
u32 fb_pack(u8 r, u8 g, u8 b)
{
    u32 px = 0;
    if (fb_r_size) px |= ((u32)(r >> (8 - fb_r_size)) & ((1u << fb_r_size) - 1)) << fb_r_pos;
    if (fb_g_size) px |= ((u32)(g >> (8 - fb_g_size)) & ((1u << fb_g_size) - 1)) << fb_g_pos;
    if (fb_b_size) px |= ((u32)(b >> (8 - fb_b_size)) & ((1u << fb_b_size) - 1)) << fb_b_pos;
    return px;
}

/* nearest 6x6x6 cube level (0,55,95,135,175,215) for an 8-bit channel */
static u32 fb_cube_level(u8 v)
{
    static const u8 lv[6] = {0, 55, 95, 135, 175, 215};
    u32 best = 0;
    int bestd = 0x7fffffff;
    for (u32 i = 0; i < 6; i++) {
        int d = (int)lv[i] - (int)v;
        if (d < 0) d = -d;
        if (d < bestd) {
            bestd = d;
            best = i;
        }
    }
    return best;
}

/* true-colour 0x00RRGGBB -> the value actually stored in a pixel.  Colour
 * modes pack the channels; 8bpp modes are paletted, so the RGB is
 * quantised onto the DAC's 6x6x6 cube entry. */
u32 fb_rgb_px(u32 rgb)
{
    u8 r = (u8)(rgb >> 16), g = (u8)(rgb >> 8), b = (u8)rgb;
    if (fb_pxsz == 1)
        return 16 + 36 * fb_cube_level(r) + 6 * fb_cube_level(g) + fb_cube_level(b);
    return fb_pack(r, g, b);
}

/* program the VGA DAC for 8bpp modes: 16 VGA colours, the 6x6x6 cube
 * (16..231) that fb_rgb_px() quantises onto, and a 24 step grey ramp */
static void fb_setup_palette(void)
{
    if (fb_pxsz != 1) return; /* colour modes never touch the DAC */

    /* program the first 16 entries via VGA DAC */
    outb(0x3C8, 0);
    for (int i = 0; i < 16; i++) {
        outb(0x3C9, palette16[i][0] >> 2);
        outb(0x3C9, palette16[i][1] >> 2);
        outb(0x3C9, palette16[i][2] >> 2);
    }
    /* 6x6x6 colour cube: indices 16..231 */
    outb(0x3C8, 16);
    for (int r = 0; r < 6; r++)
        for (int g = 0; g < 6; g++)
            for (int b = 0; b < 6; b++) {
                outb(0x3C9, (r ? (r * 40 + 55) : 0) >> 2);
                outb(0x3C9, (g ? (g * 40 + 55) : 0) >> 2);
                outb(0x3C9, (b ? (b * 40 + 55) : 0) >> 2);
            }
    /* 24 greys: indices 232..255 */
    outb(0x3C8, 232);
    for (int i = 0; i < 24; i++) {
        u8 v = (u8)(8 + i * 10);
        outb(0x3C9, v >> 2);
        outb(0x3C9, v >> 2);
        outb(0x3C9, v >> 2);
    }
}

/* ---- public API ---- */

/* Bochs/VBE DISPI I/O ports: only meaningful on Bochs/QEMU/VirtualBox, never
 * on real hardware (the BIOS already programmed the mode in setup.asm, or
 * the boot loader did it in GRUB's case). */
#define DISPI_INDEX 0x1CE
#define DISPI_DATA  0x1CF

/* runtime framebuffer dimensions (set by fb_init from the boot loader) */
u32 fb_width = 640;
u32 fb_height = 480;

static u16 dispi_read(u16 idx)
{
    outw(DISPI_INDEX, idx);
    return inw(DISPI_DATA);
}

static void dispi_write(u16 idx, u16 val)
{
    outw(DISPI_INDEX, idx);
    outw(DISPI_DATA, val);
}

/* Make sure the Bochs-compatible display sits in the mode the boot loader
 * described.  Returns the active scan line length in bytes (0 when the
 * DISPI device is absent, i.e. on real hardware). */
static u32 vbe_sync_mode(void)
{
    u16 id = dispi_read(0); /* Bochs=0xB0C5, VirtualBox=0xB0C8 */
    if (id != 0xB0C5 && id != 0xB0C8) return 0;

    u16 w = dispi_read(1);
    u16 h = dispi_read(2);
    u16 bpp = dispi_read(3);
    u16 en = dispi_read(4);   /* ENABLE (register 5 is the 64 KiB bank window) */
    u16 virt = dispi_read(6); /* VIRT_WIDTH in pixels */

    if (en & 0x03 && w == fb_width && h == fb_height && bpp == fb_bpp)
        return (u32)(virt ? virt : w) * (u32)bpp / 8;

    /* The BIOS/GRUB left the display in some other mode: reprogram it. */
    dispi_write(4, 0);         /* disable VBE */
    dispi_write(1, fb_width);  /* X resolution */
    dispi_write(2, fb_height); /* Y resolution */
    dispi_write(3, (u16)fb_bpp);
    dispi_write(4, 0x03);      /* enable VBE (bit 0) + LFB (bit 1) */
    virt = dispi_read(6);
    return (u32)(virt ? virt : fb_width) * (u32)fb_bpp / 8;
}

/* ---- framebuffer text console ---- */
static int fbc_cols, fbc_rows;

int fb_active = 0;
static int fbc_row, fbc_col;
static u32 fbc_fg = FB_CLR_LGREY;
static u32 fbc_bg = FB_CLR_BLACK;

/* ANSI 30-37 / 40-47 code -> 0x00RRGGBB.  ansi_color_index() itself still
 * returns a 0-7 VGA index for the serial console, which speaks indices. */
static u32 ansi_rgb(int idx)
{
    static const u32 vga[16] = {FB_CLR_BLACK,    FB_CLR_RED,      FB_CLR_GREEN,
                                FB_CLR_BROWN,    FB_CLR_NAVY,     FB_CLR_PURPLE,
                                FB_CLR_TEAL,     FB_CLR_LGREY,    FB_CLR_DGREY,
                                FB_CLR_LBLUE,    FB_CLR_LGREEN,   FB_CLR_LCYAN,
                                FB_CLR_LRED,     FB_CLR_LMAGENTA, FB_CLR_YELLOW,
                                FB_CLR_WHITE};
    return vga[idx & 15];
}

/* ANSI escape state machine (tokenizer shared with print.c) */
static struct ansi_seq fbc_ansi;

static void fbc_cursor(int show)
{
    if (!fb_front) return;
    u32 c = show ? FB_CLR_LGREY : fbc_bg;
    for (u32 dy = FB_CELL_H - 2; dy < FB_CELL_H; dy++)
        for (u32 dx = 0; dx < FB_CELL_W; dx++)
            fb_put_pixel(fbc_col * FB_CELL_W + dx, fbc_row * FB_CELL_H + dy, c);
}

static void fbc_esc_process(const char *buf, int len)
{
    char last = buf[len - 1];
    if (last == 'J') {
        if (len >= 2 && buf[0] == '2') {
            fb_clear(fbc_bg);
            fbc_row = fbc_col = 0;
        }
    } else if (last == 'H') {
        int row = 1, col = 1, num = 0, have = 0, semi = 0;
        for (int i = 0; i < len - 1; i++) {
            if (buf[i] >= '0' && buf[i] <= '9') {
                num = num * 10 + (buf[i] - '0');
                have = 1;
            } else if (buf[i] == ';') {
                if (have) {
                    row = num;
                    num = 0;
                    have = 0;
                }
                semi = 1;
            }
        }
        /* CSI n H = row n, column 1; only a ';' introduces the column */
        if (semi) {
            if (have) col = num;
        } else if (have) {
            row = num;
        }
        fbc_row = row - 1;
        fbc_col = col - 1;
        if (fbc_row < 0) fbc_row = 0;
        if (fbc_row >= fbc_rows) fbc_row = fbc_rows - 1;
        if (fbc_col < 0) fbc_col = 0;
        if (fbc_col >= fbc_cols) fbc_col = fbc_cols - 1;
    } else if (last == 'm') {
        if (len == 1) { /* ESC[m == ESC[0m: empty parameter list = reset */
            fbc_fg = FB_CLR_LGREY;
            fbc_bg = FB_CLR_BLACK;
            return;
        }
        int num = 0, have = 0;
        for (int i = 0; i < len; i++) {
            if (buf[i] >= '0' && buf[i] <= '9') {
                num = num * 10 + (buf[i] - '0');
                have = 1;
            } else if (buf[i] == ';' || i == len - 1) {
                if (have) {
                    if (num == 0) {
                        fbc_fg = FB_CLR_LGREY;
                        fbc_bg = FB_CLR_BLACK;
                    } else if (num >= 30 && num <= 37)
                        fbc_fg = ansi_rgb(ansi_color_index(num));
                    else if (num >= 40 && num <= 47)
                        fbc_bg = ansi_rgb(ansi_color_index(num - 10));
                    else if (num == 1)
                        fbc_fg = FB_CLR_WHITE;
                    num = 0;
                    have = 0;
                }
            }
        }
    }
}

static void fbc_scroll(void)
{
    u32 row_bytes = fb_pitch * FB_CELL_H;
    for (int r = 0; r < fbc_rows - 1; r++)
        memcpy((void *)fb_ptr + (u64)r * row_bytes, (void *)fb_ptr + (u64)(r + 1) * row_bytes,
               row_bytes);
    /* clear the last text row (pixel-aware: memset would only fit 8bpp) */
    fb_fill_rect(0, (u32)(fbc_rows - 1) * FB_CELL_H, fb_width, FB_CELL_H, fbc_bg);
}

void fb_console_putc(char c)
{
    if (!fb_active) return;

    int r = ansi_feed(&fbc_ansi, c);
    if (r == ANSI_CONSUMED) {
        /* hide the software cursor while an escape runs: phase 1 is the ESC
         * byte itself, later sequence bytes keep phase != 1 so this fires
         * exactly once per sequence */
        if (fbc_ansi.phase == 1) fbc_cursor(0);
        return;
    }
    if (r == ANSI_DONE) {
        fbc_esc_process(fbc_ansi.buf, fbc_ansi.len);
        fbc_cursor(1); /* redraw at wherever the sequence left the cursor */
        return;
    }

    fbc_cursor(0);

    switch (c) {
    case '\n':
        fbc_col = 0;
        if (++fbc_row >= fbc_rows) {
            fbc_scroll();
            fbc_row--;
        }
        break;
    case '\r':
        break;
    case '\t':
        fbc_col = (fbc_col / 8 + 1) * 8; /* tab stop, not a cell boundary */
        if (fbc_col >= fbc_cols) {
            fbc_col = 0;
            if (++fbc_row >= fbc_rows) {
                fbc_scroll();
                fbc_row--;
            }
        }
        break;
    case '\b':
        if (fbc_col > 0) {
            fbc_col--;
            fb_draw_char(fbc_col * FB_CELL_W, fbc_row * FB_CELL_H, ' ', fbc_fg, fbc_bg);
        }
        break;
    default:
        if (c >= 0x20 && c <= 0x7E) {
            fb_draw_char(fbc_col * FB_CELL_W, fbc_row * FB_CELL_H, c, fbc_fg, fbc_bg);
            if (++fbc_col >= fbc_cols) {
                fbc_col = 0;
                if (++fbc_row >= fbc_rows) {
                    fbc_scroll();
                    fbc_row--;
                }
            }
        }
    }

    fbc_cursor(1);
}

void fb_console_init(void)
{
    fbc_cols = fb_width / FB_CELL_W;
    fbc_rows = fb_height / FB_CELL_H;
    fbc_row = fbc_col = 0;
    fb_clear(fbc_bg);
    fb_active = 1;

    int n = ring_get_count();
    for (int i = 0; i < n; i++) fb_console_putc(ring_get_char(i));
}

/* published for the boot self-test (ktest/t_fb.c): the escape parser's
 * clamping and the scroll path are only visible through this state */
void fb_console_get(int *row, int *col, int *rows, int *cols)
{
    if (row) *row = fbc_row;
    if (col) *col = fbc_col;
    if (rows) *rows = fbc_rows;
    if (cols) *cols = fbc_cols;
}

void fb_init(void)
{
    struct fb_info *fb = &bootinfo.fb;
    if (!fb->ok || !fb->phys_addr) return;
    /* Reject obviously invalid mode data (garbage from buggy BIOSes). */
    if (fb->width == 0 || fb->height == 0 || fb->pitch == 0 || fb->width > FB_MAX_DIM ||
        fb->height > FB_MAX_DIM || (u64)fb->pitch * fb->height > FB_MAX_BYTES ||
        (fb->bpp != 8 && fb->bpp != 15 && fb->bpp != 16 && fb->bpp != 24 && fb->bpp != 32)) {
        kprintf("[fb] invalid mode %dx%d %u bpp pitch=%u - skipping fb\n", fb->width, fb->height,
                fb->bpp, fb->pitch);
        return;
    }

    fb_width = fb->width;
    fb_height = fb->height;
    fb_bpp = fb->bpp;
    fb_pxsz = (fb->bpp + 7) / 8;

    /* channel layout, with sensible defaults when the loader left it empty */
    fb_r_pos = fb->r_pos;
    fb_r_size = fb->r_size;
    fb_g_pos = fb->g_pos;
    fb_g_size = fb->g_size;
    fb_b_pos = fb->b_pos;
    fb_b_size = fb->b_size;
    if (!fb_r_size && !fb_g_size && !fb_b_size) {
        if (fb_bpp == 15) {
            fb_r_size = fb_g_size = fb_b_size = 5;
            fb_r_pos = 10;
            fb_g_pos = 5;
            fb_b_pos = 0;
        } else if (fb_bpp == 16) {
            fb_r_size = fb_b_size = 5;
            fb_g_size = 6;
            fb_r_pos = 11;
            fb_g_pos = 5;
            fb_b_pos = 0;
        } else if (fb_bpp >= 24) {
            fb_r_size = fb_g_size = fb_b_size = 8;
            fb_r_pos = 16;
            fb_g_pos = 8;
            fb_b_pos = 0;
        }
    }
    if (fb_r_size > 8 || fb_g_size > 8 || fb_b_size > 8 || fb_r_pos > 31 || fb_g_pos > 31 ||
        fb_b_pos > 31) {
        kprintf("[fb] bad channel layout r%u/%u g%u/%u b%u/%u - skipping fb\n", fb_r_pos, fb_r_size,
                fb_g_pos, fb_g_size, fb_b_pos, fb_b_size);
        return;
    }
    if (fb->pitch < fb_width * fb_pxsz) {
        kprintf("[fb] pitch %u too small for %u bpp - skipping fb\n", fb->pitch, fb_bpp);
        return;
    }

    /* Bochs/QEMU only: make sure the hardware sits in this mode.  Real
     * hardware has no DISPI device and keeps whatever the BIOS set. */
    u32 dispi_pitch = vbe_sync_mode();
    if (dispi_pitch && dispi_pitch >= fb_width * fb_pxsz && dispi_pitch != fb->pitch) {
        kprintf("[fb] pitch adjusted to %u by DISPI\n", dispi_pitch);
        fb->pitch = dispi_pitch;
    }
    fb_pitch = fb->pitch;

    /* Map the framebuffer into kernel VA using 4 KiB pages. */
    u64 pa = fb->phys_addr;
    u64 size = (u64)fb_pitch * fb_height;
    for (u64 off = 0; off < size; off += PAGE_SIZE)
        vmm_map_kernel_page(FB_VMA + off, pa + off, PG_PCD);

    fb_front = (volatile u8 *)FB_VMA;
    fb_ptr = fb_front;

    kprintf("[fb] mapped at %p (phys=0x%lx), %ux%u %u bpp, pitch=%u\n", (void *)fb_ptr, pa,
            fb_width, fb_height, fb_bpp, fb_pitch);


    /* palette: DAC programming for 8bpp, packed pixels otherwise */
    fb_setup_palette();

    /* Switch console to framebuffer output */
    fb_console_init();
}

/* store one pixel at dst, honouring the pixel width of the mode */
static inline void fb_store(volatile u8 *dst, u32 px)
{
    if (fb_pxsz == 4)
        *(volatile u32 *)dst = px;
    else if (fb_pxsz == 2)
        *(volatile u16 *)dst = (u16)px;
    else
        for (u32 i = 0; i < fb_pxsz; i++) dst[i] = (u8)(px >> (8 * i));
}

/* one pixel, pre-converted by fb_rgb_px(), clipped to the visible area */
static inline void fb_store_px(u32 x, u32 y, u32 px)
{
    if (x >= fb_width || y >= fb_height || !fb_ptr) return;
    fb_store(fb_ptr + (u64)y * fb_pitch + (u64)x * fb_pxsz, px);
}

static void fb_put_pixel(u32 x, u32 y, u32 color)
{
    fb_store_px(x, y, fb_rgb_px(color));
}

/* ---- self-test hooks (include/framebuffer.h) ---- */

void fb_layout_get(struct fb_layout *l)
{
    l->r_pos = fb_r_pos;
    l->r_size = fb_r_size;
    l->g_pos = fb_g_pos;
    l->g_size = fb_g_size;
    l->b_pos = fb_b_pos;
    l->b_size = fb_b_size;
    l->pxsz = fb_pxsz;
}

void fb_layout_set(const struct fb_layout *l)
{
    fb_r_pos = l->r_pos;
    fb_r_size = l->r_size;
    fb_g_pos = l->g_pos;
    fb_g_size = l->g_size;
    fb_b_pos = l->b_pos;
    fb_b_size = l->b_size;
    fb_pxsz = l->pxsz;
}

bool fb_read_px(u32 x, u32 y, u32 *out)
{
    if (!fb_ptr || x >= fb_width || y >= fb_height) return false;
    volatile u8 *p = fb_ptr + (u64)y * fb_pitch + (u64)x * fb_pxsz;
    u32 v = 0;
    if (fb_pxsz == 4)
        v = *(volatile u32 *)p;
    else if (fb_pxsz == 2)
        v = *(volatile u16 *)p;
    else
        for (u32 i = 0; i < fb_pxsz; i++) v |= (u32)p[i] << (8 * i);
    *out = v;
    return true;
}

void fb_write_px(u32 x, u32 y, u32 raw)
{
    if (!fb_ptr || x >= fb_width || y >= fb_height) return;
    fb_store(fb_ptr + (u64)y * fb_pitch + (u64)x * fb_pxsz, raw);
}

void fb_clear(u32 color)
{
    if (!fb_ptr) return;
    u32 px = fb_rgb_px(color);
    if (fb_pxsz == 1) {
        memset((void *)fb_ptr, (int)px, (size_t)fb_pitch * fb_height);
        return;
    }
    for (u32 y = 0; y < fb_height; y++) {
        volatile u8 *row = fb_ptr + (u64)y * fb_pitch;
        for (u32 x = 0; x < fb_width; x++) fb_store(row + (u64)x * fb_pxsz, px);
    }
}

void fb_fill_rect(u32 x, u32 y, u32 w, u32 h, u32 color)
{
    if (!fb_ptr) return;
    u32 px = fb_rgb_px(color);
    for (u32 row = y; row < y + h && row < fb_height; row++) {
        volatile u8 *base = fb_ptr + (u64)row * fb_pitch;
        for (u32 col = x; col < x + w && col < fb_width; col++)
            fb_store(base + (u64)col * fb_pxsz, px);
    }
}

void fb_draw_char(u32 x, u32 y, char ch, u32 fg, u32 bg)
{
    if (!fb_ptr) return;
    int idx = (u8)ch - 0x20;
    if (idx < 0 || idx > 94) idx = 0; /* blank for unprintable */

    u32 fgpx = fb_rgb_px(fg), bgpx = fb_rgb_px(bg);
    const u8 *glyph = font8x16[idx];
    for (int row = 0; row < 16; row++) {
        u8 bits = glyph[row];
        for (int col = 0; col < 8; col++)
            fb_store_px(x + (u32)col, y + (u32)row, (bits & (0x80 >> col)) ? fgpx : bgpx);
    }
}

void fb_puts(u32 x, u32 y, const char *s, u32 fg, u32 bg)
{
    while (*s) {
        fb_draw_char(x, y, *s, fg, bg);
        x += FB_CELL_W;
        s++;
    }
}

/* ---- ownership (T-033): one surface, one owner ----
 * A single pid: the holder, or 0 for "free".
 *
 * fb_claim is the only path that writes it, and it has to be atomic:
 * two processes drawing from different CPUs both see a free surface,
 * and a plain check-then-set would hand both of them "yes" while
 * storing whichever store landed last -- both would then believe it.
 * The CAS re-evaluates against whatever won the race, so the loser is
 * refused instead of believing it owns the screen (at most two passes:
 * either we win, or the owner changed to a pid that is not ours).
 *
 * fb_disown is CAS for the mirror-image reason: a holder exiting on one
 * CPU must not clear an owner that some other process just claimed on
 * the other, and a non-holder exiting must clear nothing at all. */
static u32 fb_owner;

u32 fb_owner_get(void) { return fb_owner; }

bool fb_claim(u32 pid)
{
    u32 seen = fb_owner;
    for (;;) {
        if (seen && seen != pid) return false;
        if (__atomic_compare_exchange_n(&fb_owner, &seen, pid, false,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            return true;
        /* `seen` now carries whatever beat us -- judge against that. */
    }
}

void fb_disown(u32 pid)
{
    u32 held = pid; /* expected: clear only if `pid` still holds it */
    (void)__atomic_compare_exchange_n(&fb_owner, &held, 0, false,
                                      __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
