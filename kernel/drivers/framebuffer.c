/* lnxrm -- VBE linear framebuffer driver + graphical console.
 * Supports 8 / 15 / 16 / 24 / 32 bits per pixel.  Every colour handed to
 * the drawing primitives is a true-colour 0x00RRGGBB value; fb_rgb_px()
 * turns it into a stored pixel (packed channels, or the nearest DAC cube
 * entry on 8bpp indexed modes).
 *
 * Drawing is double buffered by default: fb_ptr (what every fb_* routine
 * writes) is an offscreen back surface in WB RAM, the visible LFB is the
 * front surface, and fb_flip() copies what was drawn since the last
 * present -- the dirty region -- onto the screen, so the screen shows a
 * whole frame or the previous one, never a half-drawn one.  See FB_DB_*
 * in framebuffer.h for the window the back surface lives in. */
#include <framebuffer.h>
#include <console.h>
#include <boot.h>
#include <mm/mm.h>
#include <sys/cpu.h>
#include <sys/spinlock.h>
#include <io.h>
#include <font8x16.h>

extern struct boot_info bootinfo;

static volatile u8 *fb_ptr;   /* what every fb_* routine draws into */
static volatile u8 *fb_front; /* the visible LFB */
static volatile u8 *fb_back;  /* the offscreen draw surface; NULL = single buffered */

/* fb_put_pixel is internal to this file; the other four are exported and
 * serve both the text console and the fb_* syscalls (24-27).  They all
 * draw into the back surface -- fb_flip (syscall 32) is what shows it. */
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

/* Move the visible text area up by one text row.
 *
 * That is (fbc_rows-1) * pitch * FB_CELL_H bytes -- 8.1 MiB at 1920x1080x32
 * -- moved on *every* line that reaches the bottom of the screen, i.e.
 * thousands of times while a boot log runs.  Two things decide whether the
 * user watches the screen slide or watches it repaint itself from the top
 * down, and the copy order is not one of them:
 *
 *  - the unit of the copy.  lib/string.c's memcpy is a byte-at-a-time loop,
 *    so this became 8.3 million separate loads against an aperture that
 *    cannot be cached -- by far the bulk of the cost.  Linux's
 *    cfb_copyarea() exists for exactly this reason and walks a framebuffer
 *    32 bits at a time; this walks it 64 bits at a time.
 *  - the cache type of the LFB: at UC each store is additionally its own
 *    PCIe write (see fb_lfb_pte_flags()).
 *
 * One forward copy over the whole region is correct on the overlap: src =
 * dst + row_bytes with dst < src, so the byte that would overwrite src[i]
 * (at dst[i + row_bytes]) has not been reached when src[i] is read.  A
 * backward copy would smear row content downward. */
static void fb_copy_fwd(volatile void *dst, const volatile void *src, size_t n)
{
    volatile u8 *d = dst;
    const volatile u8 *s = src;

    /* Wide moves only when both ends share an alignment; they normally do
     * (src - dst is a multiple of 8 for any sane pitch), but a padded pitch
     * could break it and a mismatched pair would be UB in C. */
    if (((uptr)d & 7) == ((uptr)s & 7)) {
        while (n && ((uptr)d & 7)) {
            *d++ = *s++;
            n--;
        }
        volatile u64 *d64 = (volatile u64 *)(void *)d;
        const volatile u64 *s64 = (const volatile u64 *)(const void *)s;
        while (n >= 8) {
            *d64++ = *s64++;
            n -= 8;
        }
        /* The byte pointers must follow the wide loop, not the prologue:
         * d64/s64 moved on while d/s stood still, and the tail below would
         * then re-copy already-copied bytes from the wrong offset and leave
         * the last (n % 8) bytes of the row -- the box's right edge --
         * stale.  (2026-10-21) */
        d = (volatile u8 *)(void *)d64;
        s = (const volatile u8 *)(const void *)s64;
    }
    while (n--)
        *d++ = *s++;
}

/* ---- double buffering ----
 * Everything below draws into fb_ptr, which is the back surface whenever
 * one could be mapped (fb_back_init()).  Presenting is deliberately not
 * the caller's job for ordinary output:
 *
 *  - The console has no frames, it has a stream of characters.  Presenting
 *    per character would push a region to the screen thousands of times
 *    over a boot log, so a console write marks its region dirty and lets
 *    the batch boundary (fb_console_flush) decide when the screen is
 *    worth updating.
 *  - A drawing program does have frames, and calls fb_flip() (or the
 *    fb_flip syscall) to land one as one.
 *
 * Dirty tracking is a *region*, not a flag: fb_mark_dirty(x,y,w,h) unions
 * the box each write covered into (fb_dx0..fb_dx1, fb_dy0..fb_dy1), and a
 * present copies exactly that box, row by row -- a char cell costs an
 * 8x16 copy instead of the whole surface.  The box is sound because
 * fb_console_init() clears the whole surface before the first flip (front
 * and back then agree everywhere) and every write afterwards marks its
 * box; fbc_scroll()'s raw fb_copy_fwd() is the one write that bypasses
 * fb_store_px(), so it marks its rows itself.
 *
 * Two rules keep a mark from being lost to a present running on the other
 * CPU.  With the whole-surface flag this was merely a tear -- the next
 * store anywhere repainted everything -- but a lost *region* would sit in
 * the back surface until something marked that region again, so both
 * rules are structural:
 *
 *  - Mark after the store, never before (under fb_box_lock).  A present
 *    may then skip a write that is not marked yet (one more frame for
 *    it), but it can never consume the mark of a write that has not
 *    happened.
 *  - The present consumes the box (flag down, box emptied) under
 *    fb_box_lock and copies the pixels under console_out_lock.  Every
 *    back-surface writer holds console_out_lock for its whole write --
 *    a console batch from the first character to fb_console_flush, a
 *    drawing syscall for the whole primitive -- so the copy reads one
 *    consistent frame.  A scroll or cursor redraw landing mid-copy on
 *    the other CPU is what used to stack two frames on one screen
 *    during a fast scroll; holding the writer's own lock across the
 *    copy is what closes that window, and a mark cannot slip in either:
 *    it needs console_out_lock too, so it lands before the copy (its
 *    pixels are in it) or after (a fresh box, presented next).
 *
 * fb_box_lock is held for about a dozen stores on each side (irqsave:
 * handlers print); console_out_lock spans the copy itself, which is a
 * WB->WC blast over the dirty box -- at most a couple of milliseconds
 * with the aperture in WC (fb_memtype_program), and every spinner on the
 * other CPU is a console batch, already bounded by the same budget.
 *
 * Presenting is split between three callers:
 *
 *  - Once the timer runs (everything after lapic_timer_start(), which is
 *    where the steady state lives), the BSP's tick presents: at most one
 *    copy every FB_PRESENT_TICKS ticks, no matter how much was drawn in
 *    between, and always in the timer handler -- never in whatever
 *    context printed the byte.  A burst of kprintfs becomes one present,
 *    not one per line.  The batch gate is shut for good after the first
 *    tick (see fb_present_wanted): it used to reopen for the rest of any
 *    tick in which a present had landed, which made a fast-scrolling
 *    console present once per 64-byte chunk -- each copy as long as the
 *    whole frame budget, so output ran at a crawl and the screen lagged
 *    behind it.
 *  - Before the first tick (fb_init() through kernel_spawn, which
 *    includes all of ktest stage 3), jiffies never advances and no tick
 *    is coming, so the console presents itself at its batch boundary:
 *    kvprintf and console_write emit their output under console_out_lock
 *    and call fb_console_flush() at console_out_unlock, so each printed
 *    message lands on screen as one frame.  Coalescing on the batch, not
 *    on a character count, is what makes the boot log advance as it is
 *    printed: the whole boot log is ~3 KB, so the old once-per-1024-
 *    characters clock showed three frames for the entire boot.
 *  - fb_present_due() keeps that character count as a backstop for
 *    output that never crosses the console lock; it cannot fire inside
 *    one batch (kvprintf formats into 1024 bytes, console_write chunks
 *    64).  The paths that halt before the timer ever starts (panic, the
 *    kernel_spawn failure loop) flip the surface themselves for the
 *    same reason. */
#define FB_PRESENT_TICKS  2    /* HZ=100 -> a dirty box shows within 20 ms */
#define FB_PRESENT_CHARS 1024  /* unlocked-output backstop, see above */

static volatile u8 fb_dirty;    /* a present is owed: the box below is live */
static u64 fb_last_present;     /* jiffies of the last present */
static u32 fb_chars_since;      /* chars printed since it (pre-timer clock) */

/* The dirty box: pixels written since the last present, in pixels, x1/y1
 * exclusive.  Guarded by fb_box_lock together with fb_dirty; only the
 * present consumes it (snapshot + empty), marks only grow it. */
static u32 fb_dx0, fb_dy0, fb_dx1, fb_dy1;
static spinlock_t fb_box_lock = SPINLOCK_INIT;

/* Record the box a write just covered; fb_present_due() does the rest.
 * Over-marking is harmless -- an extra row copies bytes that already
 * match -- so the box grows in u64 and is clamped to the visible area,
 * and only a write with no visible pixel in it (fully off-screen) marks
 * nothing.  Single buffered there is nothing to track: every write
 * already showed.  A mark while nothing is pending starts a fresh box:
 * the zeros left behind by a consume are not a live box to union into
 * (unioning would anchor every later box at the origin). */
static void fb_mark_dirty(u32 x, u32 y, u32 w, u32 h)
{
    if (!fb_back) return;
    u64 x0 = x, y0 = y, x1 = (u64)x + w, y1 = (u64)y + h;
    if (x1 > fb_width) x1 = fb_width;
    if (y1 > fb_height) y1 = fb_height;
    if (x0 >= x1 || y0 >= y1) return;
    u64 fl;
    spin_lock_irqsave(&fb_box_lock, &fl);
    if (!fb_dirty) {
        fb_dx0 = (u32)x0;
        fb_dy0 = (u32)y0;
        fb_dx1 = (u32)x1;
        fb_dy1 = (u32)y1;
        fb_dirty = 1;
    } else {
        if (x0 < fb_dx0) fb_dx0 = (u32)x0;
        if (y0 < fb_dy0) fb_dy0 = (u32)y0;
        if (x1 > fb_dx1) fb_dx1 = (u32)x1;
        if (y1 > fb_dy1) fb_dy1 = (u32)y1;
    }
    spin_unlock_irqrestore(&fb_box_lock, fl);
}

/* Publish what was drawn: snapshot the dirty box, consume it, then copy
 * exactly that box from the back surface into the visible LFB, row by
 * row.  This is the whole of double buffering -- a half-drawn frame is
 * simply never copied, because the copy only happens here, and what is
 * copied is only what was marked (front and back agree outside the box:
 * fb_console_init() synced them at boot and every write has marked its
 * box since).  No-op when single buffered (fb_ptr is the LFB then and
 * every draw already showed) or when nothing is dirty.
 *
 * The copy runs with console_out_lock HELD: every back-surface writer
 * takes that lock for its whole write, so the surface cannot change
 * under the copy (the tear window the old lockless copy left open --
 * a scroll racing the present = two frames stacked on screen).  Callers
 * that do not already hold it take it here; the console batch paths
 * (fb_console_flush, fb_present_due) already do and call the _held form
 * directly -- console_out_lock is not reentrant. */
static void fb_present_held(void)
{
    if (!fb_back || !fb_front) return;
    u32 x0, y0, x1, y1;
    u64 fl;
    spin_lock_irqsave(&fb_box_lock, &fl);
    if (!fb_dirty) {
        spin_unlock_irqrestore(&fb_box_lock, fl);
        return;
    }
    x0 = fb_dx0;
    y0 = fb_dy0;
    x1 = fb_dx1;
    y1 = fb_dy1;
    fb_dx0 = fb_dy0 = fb_dx1 = fb_dy1 = 0; /* empty: consumed */
    fb_dirty = 0;
    spin_unlock_irqrestore(&fb_box_lock, fl);

    size_t row_bytes = (size_t)(x1 - x0) * fb_pxsz;
    for (u32 y = y0; y < y1; y++)
        fb_copy_fwd((volatile void *)(fb_front + (u64)y * fb_pitch + (u64)x0 * fb_pxsz),
                    (const volatile void *)(fb_back + (u64)y * fb_pitch + (u64)x0 * fb_pxsz),
                    row_bytes);
    /* WC stores drain into write-combining buffers and can sit there after
     * the last one retires; without a fence this function could return --
     * and its caller read the LFB back (ktest's fb_read_front_px) -- while
     * the tail of the frame is still in flight. */
    __asm__ volatile("mfence" ::: "memory");
    fb_chars_since = 0;
    fb_last_present = jiffies;
}

void fb_flip(void)
{
    if (!fb_back || !fb_front) return;
    u64 fl;
    console_out_lock(&fl);
    fb_present_held();
    /* Unlock still runs fb_console_flush(), but the box is consumed and
     * marks need this very lock, so the gate reads empty: no recursion. */
    console_out_unlock(fl);
}

/* Present without taking console_out_lock.  panic() only: its caller may
 * already hold the lock (a BUG inside the console path -- taking it again
 * would spin forever with interrupts off, the T-002 self-deadlock), and if
 * some other CPU holds it the frame may tear -- on a machine that is dying
 * anyway, a torn last frame beats none. */
void fb_flip_unlocked(void)
{
    if (!fb_back || !fb_front) return;
    fb_present_held();
}

int fb_double_buffered(void) { return fb_back != NULL; }

/* The gate on every console-initiated present: the printer owns the
 * screen only while jiffies is still 0 (before the first tick no tick is
 * coming and the boot log must advance as it prints).  From the first
 * tick on the gate stays shut -- fb_tick_flush() owns presenting then,
 * and a copy must not land in whatever context printed the byte (an ISR,
 * another CPU, an IRQ-off section).  The old test (jiffies ==
 * fb_last_present) reopened the gate for the rest of any tick in which a
 * present had landed: a burst of output then presented once per batch
 * (console_write chunks of 64 bytes), each copy a full-frame budget. */
static bool fb_present_wanted(void)
{ return fb_back && fb_dirty && jiffies == 0; }

/* End of a console write batch -- console_out_unlock() calls this with
 * console_out_lock held, so kvprintf and each 64-byte console_write chunk
 * end with it.  Before the first tick this is the present clock: one
 * printed message = one frame, and a message waiting out a silent stretch
 * (a disk read) is already on screen instead of stuck behind the next
 * character.  No-op once ticks own presenting (see fb_present_wanted). */
void fb_console_flush(void)
{
    if (fb_present_wanted()) fb_present_held();
}

/* Backstop for output that does not cross the console lock (panic and the
 * init replay flip themselves).  Called after every character; the counter
 * resets on each present, and a batch is always shorter than the count,
 * so in the locked paths fb_console_flush() presents and resets first. */
static void fb_present_due(void)
{
    if (!fb_present_wanted()) return;
    if (++fb_chars_since < FB_PRESENT_CHARS) return;
    fb_present_held();
}

/* The BSP timer handler: show what was drawn, at most one copy every
 * FB_PRESENT_TICKS ticks however much was written in between -- a burst of
 * kprintfs becomes one present, not one per line.  It is a load and a
 * branch when nothing drew, which is most ticks.  console_out_lock is
 * taken for the copy: BSP-only (only the BSP advances jiffies), and the
 * BSP can never deadlock on it -- a CPU holding it runs with interrupts
 * off, so this handler cannot be the holder. */
void fb_tick_flush(void)
{
    if (!fb_back || !fb_dirty) return;
    if (jiffies - fb_last_present < FB_PRESENT_TICKS) return;
    u64 fl;
    console_out_lock(&fl);
    fb_present_held();
    console_out_unlock(fl);
}

static void fbc_scroll(void)
{
    u32 row_bytes = fb_pitch * FB_CELL_H;
    if (fbc_rows < 2) return;
    fb_copy_fwd((volatile void *)fb_ptr, (volatile void *)((volatile u8 *)fb_ptr + row_bytes),
                (size_t)row_bytes * (fbc_rows - 1));
    /* the raw copy above bypasses fb_store_px(), so it marks its rows
     * itself -- store first, mark after, like every other write */
    fb_mark_dirty(0, 0, fb_width, (u32)(fbc_rows - 1) * FB_CELL_H);
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
        fb_present_due();
        return;
    }
    if (r == ANSI_DONE) {
        fbc_esc_process(fbc_ansi.buf, fbc_ansi.len);
        fbc_cursor(1); /* redraw at wherever the sequence left the cursor */
        /* This is where double buffering pays for itself on the console:
         * ESC[H + ESC[2J used to blank the screen in front of the reader
         * and repaint it afterwards; now the whole sequence lands as one
         * present. */
        fb_present_due();
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
    fb_present_due();
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

    /* Everything the console replayed is in the back surface; show it in
     * one piece rather than trickling it out through the present clock. */
    fb_flip();
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

/* ---- LFB cache type (see mm.h's cache-type note) ---- */

#define MSR_IA32_PAT          0x00000277
#define MSR_IA32_MTRR_CAP     0x000000FE
#define MSR_IA32_MTRR_DEFTYPE 0x000002FF
#define MSR_IA32_MTRR_BASE    0x00000200 /* +2i base (type in bits 2:0), +2i+1 mask */
#define MTRR_PHYSMASK         0x000FFFFFFFFFF000ULL
/* Memory-type encoding, SDM Table 11-10: UC=00h, WC=01h, WT=04h, WP=05h,
 * WB=06h, UC-=07h (02h/03h reserved).  MTRRs never encode UC-, PAT does --
 * the two share this table. */
#define PAT_TYPE_WC 0x01

static const char *const memtype_name[8] = {"UC", "WC", "rsvd", "rsvd", "WT", "WP", "WB", "UC-"};

/* How many variable MTRR ranges this kernel is willing to mirror onto the
 * APs.  Shipping CPUs carry 8-10 (QEMU: 8); past MTRR_MAX_VAR the snapshot
 * in fb_memtype_program() could not reproduce the BSP's state faithfully,
 * so nothing gets reprogrammed at all -- a uniformly firmware-typed LFB
 * beats a half-synced one. */
#define MTRR_MAX_VAR 16

/* The BSP's MTRR state as fb_memtype_program() left it, for every AP to
 * adopt.  MTRRs are per logical processor: an AP comes up on its own reset
 * state, which the platform may or may not have kept in step with the
 * BSP's (QEMU gives its APs the BSP's DEFTYPE -- observed -- but ranges
 * firmware touched only on the BSP need not be there at all).  Left
 * unsynced, the BSP would type the LFB UC under firmware's range while an
 * AP, with PAT alone, types it WC: one PTE, two types, lost pixels --
 * exactly the mixing this section exists to prevent.  Fixed MTRRs are not
 * mirrored: they reach only below 1 MiB (fb_mtrr_covering), where the only
 * resident is the AP trampoline, which fetches instructions under any type
 * and has already run by the time an AP gets here. */
static struct {
    bool valid; /* a snapshot exists; APs adopt it */
    u64 deftype;
    u64 base[MTRR_MAX_VAR];
    u64 mask[MTRR_MAX_VAR];
} s_mtrr_bsp;
static bool s_bsp_seen; /* the first caller -- the BSP -- has been and gone */

/* Give THIS logical processor a WC slot in IA32_PAT, if firmware left
 * none.  The reset layout has WC at PAT[2], but firmware may have
 * rewritten the MSR (QEMU resets it to 0x0007040600070406 -- UC- where
 * Intel puts WC), and without a WC slot the LFB can only be mapped UC:
 * every present then costs one uncached PCIe write per store, which is
 * the difference between a 1-2 ms frame and a 10-20 ms one.
 *
 * PAT[7] (index = PG_PAT|PG_PCD|PG_PWT) is the slot to claim:
 *  - nothing in this kernel selects it: every early page table entry is
 *    0x83 (PAT bits clear -> slot 0), the APIC windows use PAT[2]
 *    (PG_PCD alone, apic.c), the AHCI ABAR and the UC fallback use
 *    PAT[3] (PG_PCD|PG_PWT), and only this file ever sets PG_PAT;
 *  - it is UC on both reset layouts, so no firmware mapping that still
 *    survives can be relying on it being something else.
 *
 * The MSR is per logical processor, so every core runs this: the BSP in
 * fb_init() before the LFB PTE picks its slot (fb_lfb_pte_flags reads
 * the MSR right after), every AP at the top of ap_main() before it can
 * touch the screen.  Programming only the BSP would leave one CPU
 * storing WC and another UC to the same PTE -- exactly the mixing the
 * old read-only note warned about.  The MTRR half of the combination is
 * fb_memtype_program()'s business; this function only ever owns IA32_PAT. */
static void fb_pat_wc_program(void)
{
    if (!bootinfo.fb.ok || !bootinfo.fb.phys_addr) return;
    u64 pat = rdmsr(MSR_IA32_PAT);
    for (int i = 0; i < 8; i++)
        if (((pat >> (8 * i)) & 0xFF) == PAT_TYPE_WC) return; /* one is enough */
    pat = (pat & ~(0xFFULL << 56)) | ((u64)PAT_TYPE_WC << 56); /* PAT[7] := WC */
    wrmsr(MSR_IA32_PAT, pat);
}

/* MTRR type of a variable range covering every byte of [pa, pa+len), or -1
 * if none does; *r_idx gets that range's index when one does.  Fixed ranges
 * reach only below 1 MiB, so they cannot cover an aperture and are not
 * consulted.  Read-only, deliberately: retyping a range the firmware owns
 * also retypes everything else inside it -- an aperture's neighbours are
 * usually register blocks that must stay UC. */
static int fb_mtrr_covering(u64 pa, u64 len, u32 *r_idx)
{
    u64 cap = rdmsr(MSR_IA32_MTRR_CAP);
    for (u32 i = 0; i < (u32)(cap & 0xFF); i++) {
        u64 mask = rdmsr(MSR_IA32_MTRR_BASE + 2 * i + 1) & MTRR_PHYSMASK;
        if (!mask) continue; /* disabled range */
        u64 base = rdmsr(MSR_IA32_MTRR_BASE + 2 * i) & MTRR_PHYSMASK;
        if ((pa & mask) != (base & mask)) continue;
        if (((pa + len - 1) & mask) != (base & mask)) continue;
        *r_idx = i;
        return (int)(rdmsr(MSR_IA32_MTRR_BASE + 2 * i) & 7);
    }
    return -1;
}

/* Ask for a WC variable range over the aperture -- but only when every
 * step of it is provably safe:
 *
 *  - MTRRs on, WC supported, and no range covering the aperture: what the
 *    aperture gets is whatever the default type says, so a range of ours
 *    retypes nothing firmware had asked for;
 *  - the default type is UC or WP (with WB/WT/WC the PAT slot already
 *    comes through, so no range is needed);
 *  - the aperture rounds up to a pow2 block it is aligned to, no larger
 *    than FB_MAX_BYTES: VBE hands out the BAR's base address, and a PCI
 *    BAR is pow2-sized and at least as large as the aperture it holds, so
 *    the block stays inside the BAR and cannot reach a neighbour;
 *  - that block intersects no enabled firmware range (a partial overlap
 *    would mean shrinking or splitting theirs -- not ours to do), and one
 *    variable range is free.
 *
 * QEMU fails the first condition on purpose: firmware marks the whole
 * upper 1 GiB UC (mtrr[0] = 0xc0000000 / 1 GiB / UC), the LFB sits inside
 * it, and a 1 GiB pow2 block is not something we may carve up -- so this
 * returns quietly and fb_lfb_pte_flags() reports the UC for what it is.
 * Machines whose firmware left the aperture to a UC default but covered it
 * with no range get their WC this way.  Written only by the BSP, before
 * SMP starts: one CPU, no concurrent reader of these MSRs. */
static void fb_mtrr_aperture_wc(u64 pa, u64 len)
{
    u64 cap = rdmsr(MSR_IA32_MTRR_CAP);
    u64 def = rdmsr(MSR_IA32_MTRR_DEFTYPE);
    if (!(def & (1ULL << 11))) return; /* variable MTRRs off: PAT alone */
    if (!(cap & (1ULL << 10))) return; /* no WC in MTRR_CAP */
    u32 n = (u32)(cap & 0xFF);
    u32 idx;
    if (fb_mtrr_covering(pa, len, &idx) >= 0) return; /* firmware already types it */
    u32 dflt = (u32)(def & 0xFF);
    if (dflt > 7 || (dflt != 0 && dflt != 5)) return; /* PAT WC gets through */

    u64 size = 1;
    while (size < len) size <<= 1;
    while (size <= FB_MAX_BYTES && (pa & (size - 1))) size <<= 1;
    if (size < len || size > FB_MAX_BYTES || (pa & (size - 1))) {
        kprintf("[fb] aperture %llx+%lu not pow2-aligned within %lu MiB - no WC range\n",
                (unsigned long long)pa, (unsigned long)len,
                (unsigned long)(FB_MAX_BYTES >> 20));
        return;
    }

    u32 slot = 0xFFFFFFFF;
    for (u32 i = 0; i < n; i++) {
        u64 m = rdmsr(MSR_IA32_MTRR_BASE + 2 * i + 1);
        u64 b = rdmsr(MSR_IA32_MTRR_BASE + 2 * i);
        if (!(m & MTRR_PHYSMASK)) { /* disabled range: the one to claim */
            if (slot == 0xFFFFFFFF) slot = i;
            continue;
        }
        u64 start = b & MTRR_PHYSMASK;
        u64 rsize = (((~m) & MTRR_PHYSMASK) | 0xFFF) + 1;
        if (pa < start + rsize && start < pa + size) {
            kprintf("[fb] MTRR[%u] %s partially overlaps the aperture block - no WC range\n",
                    i, memtype_name[b & 7]);
            return;
        }
    }
    if (slot == 0xFFFFFFFF) {
        kprintf("[fb] no free MTRR range for the aperture (default %s) - LFB stays on it\n",
                memtype_name[dflt]);
        return;
    }
    /* Base first, then the mask with its V bit: the range is inactive
     * (mask 0 -> default type) until the second write lands it. */
    wrmsr(MSR_IA32_MTRR_BASE + 2 * slot, (pa & MTRR_PHYSMASK) | PAT_TYPE_WC);
    wrmsr(MSR_IA32_MTRR_BASE + 2 * slot + 1,
          ((~(size - 1)) & MTRR_PHYSMASK) | (1ULL << 11)); /* V: valid */
    kprintf("[fb] MTRR[%u] WC %llx+%lu KiB (default %s, nothing covered the aperture)\n",
            slot, (unsigned long long)pa, (unsigned long)(size >> 10),
            memtype_name[dflt]);
}

/* Program this core's LFB memory type, and keep every CPU in step:
 *
 *  - IA32_PAT gets a WC slot (per logical processor, so each core runs
 *    this -- see fb_pat_wc_program());
 *  - the first caller -- always the BSP, from fb_init() via
 *    fb_lfb_pte_flags(), long before SMP starts -- may add a WC range
 *    over the aperture (fb_mtrr_aperture_wc) and then snapshots the MTRRs
 *    for whoever comes after;
 *  - every later caller (an AP, at the top of ap_main() before it can
 *    touch the screen) replays that snapshot, so all CPUs resolve the
 *    LFB's PTE to the same type: an AP's own reset ranges may lack what
 *    firmware wrote on the BSP, and without the replay one CPU would
 *    store UC (the BSP, under firmware's range) and another WC (an AP,
 *    PAT alone) to the same PTE.
 *
 * Nothing here invents a memory type: DEFTYPE is only ever written back
 * as the BSP had it, and the fixed MTRRs are left alone (see s_mtrr_bsp). */
void fb_memtype_program(void)
{
    if (!bootinfo.fb.ok || !bootinfo.fb.phys_addr) return;
    u64 cap = rdmsr(MSR_IA32_MTRR_CAP);
    u32 n = (u32)(cap & 0xFF);

    if (s_bsp_seen) { /* AP */
        if (!s_mtrr_bsp.valid) return; /* the BSP reprogrammed nothing */
        fb_pat_wc_program();
        /* Variable ranges off first, this core's reset state dropped,
         * the BSP's ranges replayed, and DEFTYPE last -- exactly as the
         * BSP has it.  Nothing here runs with interrupts on (APs reach
         * ap_main() from the trampoline and have not sti'd yet), and this
         * core touches no memory during the window but its own stack. */
        wrmsr(MSR_IA32_MTRR_DEFTYPE, s_mtrr_bsp.deftype & ~(1ULL << 11));
        for (u32 i = 0; i < n; i++)
            wrmsr(MSR_IA32_MTRR_BASE + 2 * i + 1, 0);
        for (u32 i = 0; i < n; i++) {
            wrmsr(MSR_IA32_MTRR_BASE + 2 * i, s_mtrr_bsp.base[i]);
            wrmsr(MSR_IA32_MTRR_BASE + 2 * i + 1, s_mtrr_bsp.mask[i]);
        }
        wrmsr(MSR_IA32_MTRR_DEFTYPE, s_mtrr_bsp.deftype);
        return;
    }
    s_bsp_seen = true;

    if (n > MTRR_MAX_VAR) {
        /* Printed here, not on the APs: they call at the top of ap_main(),
         * before idt_init() has run on that core, which is no place to
         * print from.  PAT is left alone too, so every CPU stays on the
         * firmware type. */
        kprintf("[fb] %u MTRR ranges > %u - LFB stays on the firmware type\n",
                n, (unsigned)MTRR_MAX_VAR);
        return;
    }
    fb_pat_wc_program();
    if (fb_pitch && fb_height) /* geometry is set: this is the fb_init() call */
        fb_mtrr_aperture_wc(bootinfo.fb.phys_addr, (u64)fb_pitch * fb_height);
    s_mtrr_bsp.deftype = rdmsr(MSR_IA32_MTRR_DEFTYPE);
    for (u32 i = 0; i < n; i++) {
        s_mtrr_bsp.base[i] = rdmsr(MSR_IA32_MTRR_BASE + 2 * i);
        s_mtrr_bsp.mask[i] = rdmsr(MSR_IA32_MTRR_BASE + 2 * i + 1);
    }
    s_mtrr_bsp.valid = true;
}

/* PTE cache bits for the LFB, plus a line saying what came of it.
 *
 * This used to be a flat UC (PG_PCD|PG_PWT = PAT[3]), justified by "WC would
 * mean reprogramming IA32_PAT".  It does not have to: a PTE reaches all
 * eight PAT slots (index = PG_PAT*4 + PG_PCD*2 + PG_PWT), and
 * fb_memtype_program() runs first, on this CPU, so the read below is
 * guaranteed to find a WC slot (PAT[7] on firmware that rewrote the MSR,
 * PAT[2] on the Intel reset layout) -- and so that every other CPU agrees
 * about the MTRR half of the combination.
 *
 * The combination decides, and the MTRR half may still be firmware's: a
 * UC range over the aperture beats any PAT slot (architecture, not a
 * preference), so the line below reports what actually came out of it
 * instead of promising WC. */
static u64 fb_lfb_pte_flags(u64 pa, u64 len)
{
    fb_memtype_program(); /* WC slot + MTRR sync, before either is read */
    u64 pat = rdmsr(MSR_IA32_PAT);
    int idx = -1;
    for (int i = 0; i < 8; i++)
        if (((pat >> (8 * i)) & 0xFF) == PAT_TYPE_WC) {
            idx = i;
            break;
        }
    if (idx < 0) {
        /* Unreachable while fb_pat_wc_program() precedes the read (it
         * writes PAT[7] when nothing holds WC); kept as the honest
         * fallback for a future caller that skips it. */
        kprintf("[fb] IA32_PAT=%016llx holds no WC slot\n", (unsigned long long)pat);
        kprintf("[fb] LFB cache: -> UC (IA32_PAT would have to be rewritten)\n");
        return PG_PCD | PG_PWT;
    }

    int type = -1;  /* -1: MTRRs disabled, the PAT slot decides alone */
    bool covered = false;
    u32 range = 0;
    u64 def = rdmsr(MSR_IA32_MTRR_DEFTYPE);
    if (def & (1ULL << 11)) {
        type = fb_mtrr_covering(pa, len, &range);
        covered = (type >= 0);
        if (!covered) type = (int)(def & 0xFF); /* nothing covers: default type */
        if (type > 7) type = 0;                 /* reserved encoding: assume UC */
    }
    /* PAT=WC against that type: WC/WT/WB let it through, everything else
     * wins over it -- UC and WP per mm.h's table, and the reserved type
     * codes get the same safe reading (MTRRs never encode UC- themselves,
     * but a firmware range may carry a reserved field). */
    int uc = (type >= 0) && type != 1 && type != 4 && type != 6;
    const char *verdict = uc ? "UC (firmware MTRR, scrolling stays slow)"
                             : "WC (write-combining)";
    if (type < 0)
        kprintf("[fb] LFB cache: IA32_PAT[%d]=WC MTRR=disabled -> %s\n", idx, verdict);
    else if (covered)
        kprintf("[fb] LFB cache: IA32_PAT[%d]=WC MTRR[%u] %s @%llx -> %s\n",
                idx, range, memtype_name[type],
                (unsigned long long)(rdmsr(MSR_IA32_MTRR_BASE + 2 * range) & MTRR_PHYSMASK),
                verdict);
    else
        kprintf("[fb] LFB cache: IA32_PAT[%d]=WC MTRR default %s -> %s\n",
                idx, memtype_name[type], verdict);

    u64 flags = 0;
    if (idx & 1) flags |= PG_PWT;
    if (idx & 2) flags |= PG_PCD;
    if (idx & 4) flags |= PG_PAT;
    return flags;
}

/* Map an offscreen back surface for the running mode and start drawing
 * into it (fb_ptr).  Leaves single buffering in place -- with a saying-so
 * on the console -- when the mode does not fit the window or RAM is short;
 * fb_flip() and fb_present_due() are no-ops then, and every other path is
 * unchanged because they all go through fb_ptr either way.
 *
 * The surface is built from single 4 KiB buddy frames mapped contiguously:
 * the buddy's largest block is 4 MiB (MAX_ORDER 10) but a 1080p surface is
 * ~7.9 MiB, and page-at-a-time mapping also survives any fragmentation. */
static void fb_back_init(void)
{
    u64 size = (u64)fb_pitch * fb_height;
    if (!size || size > FB_DB_MAX_BYTES) {
        /* !size cannot happen (fb_init() rejected zero-pitch and
         * zero-height modes first) -- but fb_ptr would become an
         * unmapped address if it ever did, and every draw from there is
         * a fault, so the guard earns its keep. */
        kprintf("[fb] surface %lu B outside the %lu B window - single buffered\n",
                (unsigned long)size, (unsigned long)FB_DB_MAX_BYTES);
        return;
    }
    u64 need = ALIGN_UP(size, PAGE_SIZE);
    for (u64 off = 0; off < need; off += PAGE_SIZE) {
        u64 pa = pmm_alloc();
        if (!pa) {
            /* Failing this early means RAM is essentially gone already.
             * The frames mapped so far stay where they are (nothing writes
             * through them -- fb_back stays NULL, so fb_ptr remains the
             * LFB): unwinding a boot-time allocation that is at most a few
             * MiB is not worth a rollback path here. */
            kprintf("[fb] back surface wants %lu KiB - RAM too low, single buffered\n",
                    (unsigned long)(need >> 10));
            return;
        }
        /* WB, the same memory type as the frames' own high-half alias.
         * Two cacheable mappings of one frame are coherent on x86, and
         * drawing into WB RAM is the point: the slow aperture gets one
         * wide copy per flip instead of one uncached store per pixel. */
        vmm_map_kernel_page(FB_DB_VMA + off, pa, PG_P | PG_W);
    }
    fb_back = (volatile u8 *)FB_DB_VMA;
    fb_ptr = fb_back;
    kprintf("[fb] double buffered: back %p (%lu KiB, WB RAM) -> LFB on flip\n",
            (void *)fb_back, (unsigned long)(need >> 10));
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

    /* Map the framebuffer into kernel VA using 4 KiB pages, as
     * write-combining if the PAT and the firmware's MTRRs allow it (see
     * fb_lfb_pte_flags() -- it prints what it settled on). */
    u64 pa = fb->phys_addr;
    u64 size = (u64)fb_pitch * fb_height;
    u64 pte = fb_lfb_pte_flags(pa, size);
    for (u64 off = 0; off < size; off += PAGE_SIZE)
        vmm_map_kernel_page(FB_VMA + off, pa + off, pte);

    fb_front = (volatile u8 *)FB_VMA;
    fb_ptr = fb_front;

    kprintf("[fb] mapped at %p (phys=0x%lx), %ux%u %u bpp, pitch=%u\n", (void *)fb_ptr, pa,
            fb_width, fb_height, fb_bpp, fb_pitch);


    /* palette: DAC programming for 8bpp, packed pixels otherwise */
    fb_setup_palette();

    /* Start drawing offscreen; a no-op that logs why when it cannot. */
    fb_back_init();

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

/* one pixel, pre-converted by fb_rgb_px(), clipped to the visible area.
 * A bare store: the caller marks what it wrote (see fb_mark_dirty -- the
 * store must land before its mark). */
static inline void fb_store_px(u32 x, u32 y, u32 px)
{
    if (x >= fb_width || y >= fb_height || !fb_ptr) return;
    fb_store(fb_ptr + (u64)y * fb_pitch + (u64)x * fb_pxsz, px);
}

static void fb_put_pixel(u32 x, u32 y, u32 color)
{
    fb_store_px(x, y, fb_rgb_px(color));
    fb_mark_dirty(x, y, 1, 1); /* no-op when clipped off-screen */
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

/* one raw stored pixel out of `base` (the back surface or the LFB); both
 * surfaces have the same geometry, so only the base differs */
static bool fb_px_read(volatile u8 *base, u32 x, u32 y, u32 *out)
{
    if (!base || x >= fb_width || y >= fb_height) return false;
    volatile u8 *p = base + (u64)y * fb_pitch + (u64)x * fb_pxsz;
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

bool fb_read_px(u32 x, u32 y, u32 *out) { return fb_px_read(fb_ptr, x, y, out); }

bool fb_read_front_px(u32 x, u32 y, u32 *out) { return fb_px_read(fb_front, x, y, out); }

/* ktest only (see framebuffer.h): one raw store into the back surface
 * *without* marking it.  A present copies exactly the marked region, and
 * that is only observable through a write that does not mark. */
void fb_poke_back_px(u32 x, u32 y, u32 raw)
{
    if (!fb_back || x >= fb_width || y >= fb_height) return;
    fb_store(fb_back + (u64)y * fb_pitch + (u64)x * fb_pxsz, raw);
}

void fb_write_px(u32 x, u32 y, u32 raw)
{
    if (!fb_ptr || x >= fb_width || y >= fb_height) return;
    fb_store(fb_ptr + (u64)y * fb_pitch + (u64)x * fb_pxsz, raw);
    fb_mark_dirty(x, y, 1, 1);
}

void fb_clear(u32 color)
{
    if (!fb_ptr) return;
    u32 px = fb_rgb_px(color);
    if (fb_pxsz == 1)
        memset((void *)fb_ptr, (int)px, (size_t)fb_pitch * fb_height);
    else
        for (u32 y = 0; y < fb_height; y++) {
            volatile u8 *row = fb_ptr + (u64)y * fb_pitch;
            for (u32 x = 0; x < fb_width; x++) fb_store(row + (u64)x * fb_pxsz, px);
        }
    fb_mark_dirty(0, 0, fb_width, fb_height); /* the whole surface changed */
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
    fb_mark_dirty(x, y, w, h); /* clamped; a fully off-screen rect marks nothing */
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
    /* one mark for the whole glyph box (writes clipped per pixel above) */
    fb_mark_dirty(x, y, FB_CELL_W, FB_CELL_H);
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
