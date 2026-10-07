/* Colour packing: fb_pack() must put each channel exactly where this mode's
 * bit layout says it goes, for every layout a VBE/GRUB loader can report.
 *
 * Why this is worth booting a kernel for: a wrong shift does not crash
 * anything, it just makes every colour slightly (or wildly) wrong on screen,
 * which is invisible in a serial log.  The layout is swapped in through the
 * self-test hooks in include/framebuffer.h and restored before returning. */
#include <sys/ktest.h>
#include <console.h>
#include <framebuffer.h>

struct layout_case {
    const char *name;
    u32 pxsz; /* bytes per pixel: 1 == paletted 8bpp */
    u32 r_pos, r_size, g_pos, g_size, b_pos, b_size;
};

static const struct layout_case layouts[] = {
    {"15bpp RGB555", 2, 10, 5, 5, 5, 0, 5},
    {"16bpp RGB565", 2, 11, 5, 5, 6, 0, 5},
    {"16bpp RGB555", 2, 10, 5, 5, 5, 0, 5},
    {"24bpp RGB", 3, 16, 8, 8, 8, 0, 8},
    {"24bpp BGR", 3, 0, 8, 8, 8, 16, 8},
    {"32bpp RGBX", 4, 16, 8, 8, 8, 0, 8},
    {"32bpp XRGB", 4, 0, 8, 8, 8, 16, 8},
    {"8bpp indexed", 1, 0, 0, 0, 0, 0, 0},
};

static const u32 colors[] = {0x000000, 0xffffff, 0xff0000, 0x00ff00, 0x0000ff,
                             0x123456, 0xa5a5a5, 0x8040c0, 0x010101, 0xfedcba};

/* pull a channel back out of a stored pixel and widen it to 8 bits the same
 * way fb_pack() narrowed it -- this is the reference the kernel matches */
static u32 unpack(u32 px, u32 pos, u32 size)
{
    if (!size) return 0;
    return ((px >> pos) & ((1u << size) - 1)) << (8 - size);
}

/* the value fb_pack() is allowed to produce for an 8-bit input channel */
static u32 narrow8(u32 v, u32 size)
{
    if (!size) return 0;
    return (v >> (8 - size)) << (8 - size);
}

static u32 channel_mask(u32 pos, u32 size)
{
    return size ? (((1u << size) - 1) << pos) : 0;
}

/* One layout: pack every probe colour, decode it again, and make sure no
 * bit outside the declared channels moved.  Returns the number of misses. */
static u32 check_layout(const struct layout_case *lc)
{
    u32 fails = 0;
    for (size_t c = 0; c < sizeof(colors) / sizeof(colors[0]); c++) {
        u32 rgb = colors[c];
        u32 r = rgb >> 16, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
        u32 px = fb_pack((u8)r, (u8)g, (u8)b);
        u32 keep = channel_mask(lc->r_pos, lc->r_size) | channel_mask(lc->g_pos, lc->g_size) |
                   channel_mask(lc->b_pos, lc->b_size);

        if (unpack(px, lc->r_pos, lc->r_size) != narrow8(r, lc->r_size)) fails++;
        if (unpack(px, lc->g_pos, lc->g_size) != narrow8(g, lc->g_size)) fails++;
        if (unpack(px, lc->b_pos, lc->b_size) != narrow8(b, lc->b_size)) fails++;
        if (px & ~keep) fails++;                      /* stray bits */
        if (lc->pxsz < 4 && px >= (1u << (8 * lc->pxsz))) fails++; /* too wide */
        if (lc->pxsz == 1 && lc->r_size == 0 && px) fails++; /* indexed: fb_pack is 0 */
    }
    return fails;
}

static void test_fb_pack_layouts(void)
{
    struct fb_layout save, got;
    fb_layout_get(&save);

    for (size_t i = 0; i < sizeof(layouts) / sizeof(layouts[0]); i++) {
        const struct layout_case *lc = &layouts[i];
        struct fb_layout l = save;
        l.pxsz = lc->pxsz;
        l.r_pos = lc->r_pos;
        l.r_size = lc->r_size;
        l.g_pos = lc->g_pos;
        l.g_size = lc->g_size;
        l.b_pos = lc->b_pos;
        l.b_size = lc->b_size;
        fb_layout_set(&l);

        u32 fails = check_layout(lc);
        if (fails) kprintf("[ktest] %s: %u colour checks wrong\n", lc->name, fails);
        K_EXPECT_EQ(fails, 0);
        K_EXPECT_EQ(fb_pack(0, 0, 0), 0); /* black is all-zero in every layout */
    }

    /* put the real hardware layout back, then read it back: this is the
     * get/set round-trip, and it leaves the later cases on the real mode */
    fb_layout_set(&save);
    fb_layout_get(&got);
    K_EXPECT_EQ(got.r_pos, save.r_pos);
    K_EXPECT_EQ(got.r_size, save.r_size);
    K_EXPECT_EQ(got.g_pos, save.g_pos);
    K_EXPECT_EQ(got.g_size, save.g_size);
    K_EXPECT_EQ(got.b_pos, save.b_pos);
    K_EXPECT_EQ(got.b_size, save.b_size);
    K_EXPECT_EQ(got.pxsz, save.pxsz);
}

/* 8bpp is paletted: fb_rgb_px() must land on the DAC's 6x6x6 cube and stay
 * inside indices 16..231 (0..15 are the fixed VGA colours, 232..255 grey). */
static void test_fb_rgb_px_8bpp(void)
{
    static const u8 lv[6] = {0, 55, 95, 135, 175, 215};
    struct fb_layout save, l;
    u32 fails = 0;

    fb_layout_get(&save);
    l = save;
    l.pxsz = 1;
    l.r_size = l.g_size = l.b_size = 0;
    l.r_pos = l.g_pos = l.b_pos = 0;
    fb_layout_set(&l);

    /* every cube entry round-trips onto its own index */
    for (int r = 0; r < 6; r++)
        for (int g = 0; g < 6; g++)
            for (int b = 0; b < 6; b++) {
                u32 rgb = ((u32)lv[r] << 16) | ((u32)lv[g] << 8) | lv[b];
                if (fb_rgb_px(rgb) != (u32)(16 + 36 * r + 6 * g + b)) fails++;
            }
    if (fails) kprintf("[ktest] 8bpp cube: %u entries mis-quantised\n", fails);
    K_EXPECT_EQ(fails, 0);

    /* arbitrary colours must pick the *nearest* level of each channel */
    fails = 0;
    for (size_t c = 0; c < sizeof(colors) / sizeof(colors[0]); c++) {
        u32 px = fb_rgb_px(colors[c]);
        if (px < 16 || px > 231) {
            fails++;
            continue;
        }
        u32 rest = px - 16;
        u32 want[3] = {rest / 36, (rest % 36) / 6, rest % 6};
        u32 have[3] = {colors[c] >> 16, (colors[c] >> 8) & 0xff, colors[c] & 0xff};
        for (int ch = 0; ch < 3; ch++) {
            int best = 0, bestd = 0x7fffffff;
            for (int i = 0; i < 6; i++) {
                int d = (int)lv[i] - (int)have[ch];
                if (d < 0) d = -d;
                if (d < bestd) { bestd = d; best = i; }
            }
            if (want[ch] != (u32)best) fails++;
        }
    }
    if (fails) kprintf("[ktest] 8bpp nearest-level: %u colours wrong\n", fails);
    K_EXPECT_EQ(fails, 0);

    fb_layout_set(&save);
}

/* restoring the real layout is what keeps the console we are printing to
 * readable for the rest of the boot */
static void test_fb_layout_restored(void)
{
    struct fb_layout now;
    fb_layout_get(&now);
    K_EXPECT(now.r_size == 5 || now.r_size == 8 || now.r_size == 0);
    K_EXPECT(now.pxsz >= 1 && now.pxsz <= 4);
}

/* ---- the frame buffer console (T-011 fbc_esc_process, T-012 clipping) --
 * Everything the escape parser and the scroll path do is invisible in a
 * serial log: a clamp that fails writes a cell off the end of the buffer, a
 * scroll that promotes the wrong row smears the screen.  Both are only
 * observable through fb_console_get() and fb_read_px().
 *
 * Sequences are kept at 7 body bytes: ANSI_SEQ_MAX is 8 and the tokenizer
 * drops bytes past that, so a longer CSI would be silently truncated (which
 * is itself what the unterminated case below pins down).
 *
 * These cases never clear the screen.  ESC[2J would throw the boot log away
 * (so the "ESC[2J rehomes the cursor" half of T-011 is left out on purpose),
 * and the clear side of it cannot be separated from the rehome side.  All
 * writes land on the blank rows under the log and are blanked again with
 * spaces before the case returns. */

static void fbc_str(const char *s)
{
    while (*s) fb_console_putc(*s++);
}

/* a decimal CSI parameter, printed straight into the console */
static void fbc_putn(int n)
{
    char tmp[12];
    int i = 0;
    if (n <= 0) {
        fb_console_putc('0');
        return;
    }
    while (n > 0) {
        tmp[i++] = (char)('0' + n % 10);
        n /= 10;
    }
    while (i > 0) fb_console_putc(tmp[--i]);
}

static void fbc_state(int *row, int *col, int *rows, int *cols)
{
    fb_console_get(row, col, rows, cols);
}

/* one text cell, row-major; false if any pixel of it is off-screen */
static bool cell_read(int row, int col, u32 *out)
{
    u32 x0 = (u32)col * FB_CELL_W, y0 = (u32)row * FB_CELL_H;
    for (u32 dy = 0; dy < FB_CELL_H; dy++)
        for (u32 dx = 0; dx < FB_CELL_W; dx++)
            if (!fb_read_px(x0 + dx, y0 + dy, &out[dy * FB_CELL_W + dx])) return false;
    return true;
}

/* a cursor position as a CSI, 0-based in, 1-based on the wire; the guards
 * below keep the body inside the 7-byte tokenizer budget */
static void fbc_move(int row0, int col0)
{
    fbc_str("\033[");
    fbc_putn(row0 + 1);
    fbc_str(";");
    fbc_putn(col0 + 1);
    fbc_str("H");
}

/* erase one text row with spaces -- never with ESC[2J, which would take the
 * boot log with it; the wrap at the end of the row must not scroll either,
 * so the caller only passes rows above the last one */
static void fbc_blank_row(int row0, int cols)
{
    fbc_move(row0, 0);
    for (int i = 0; i < cols; i++) fb_console_putc(' ');
}

static void test_fb_console_clipping(void)
{
    int row = 0, col = 0, rows = 0, cols = 0;
    int c_row = 0, c_col = 0, k_row = 0, k_col = 0;
    int r0 = 0;

    fbc_state(&row, &col, &rows, &cols);
    if (!fb_active || rows <= 0 || cols <= 0) {
        ktest_skip("no frame buffer console");
        return;
    }
    if (rows > 99 || cols > 999) {
        /* fbc_move() would build a CSI body longer than ANSI_SEQ_MAX-1 and
         * the tokenizer would drop its terminator */
        ktest_skip("grid beyond the 7-byte CSI budget");
        return;
    }

    /* Rows 0..c_row hold the boot log and this case must not disturb them:
     * no screen clear, no cursor parked on a logged cell (fbc_cursor() paints
     * an 8x2 block and erases it with the background, which would eat the
     * bottom of the glyph underneath).  Everything happens on the blank rows
     * underneath and is blanked again with spaces before returning. */
    c_row = row;
    c_col = col;
    r0 = c_row + 1; /* first row this case may write to */
    if (r0 + 3 >= rows) {
        ktest_skip("no blank rows below the log");
        return;
    }

    /* out-of-range coordinates are clamped inside the grid: a row of 999
     * lands on the last text row, never past it (and that row is blank) */
    fbc_str("\033[999H");
    fbc_state(&row, &col, NULL, NULL);
    K_EXPECT_EQ(row, rows - 1);
    K_EXPECT_EQ(col, 0);

    /* a column of 999 lands on the last cell of the row it was given */
    fbc_str("\033[");
    fbc_putn(r0 + 1);
    fbc_str(";999H");
    fbc_state(&row, &col, NULL, NULL);
    K_EXPECT_EQ(row, r0);
    K_EXPECT_EQ(col, cols - 1);

    /* a column of 0 is one before the first cell: clamped back to 0, not to
     * -1 (the row clamp shares that code; reaching row 0 would mean parking
     * the cursor on the log, so only the column side is driven here) */
    fbc_str("\033[");
    fbc_putn(r0 + 1);
    fbc_str(";0H");
    fbc_state(&row, &col, NULL, NULL);
    K_EXPECT_EQ(row, r0);
    K_EXPECT_EQ(col, 0);

    /* a row parameter with no column leaves the column at 0 */
    fbc_str("\033[");
    fbc_putn(r0 + 2);
    fbc_str("H");
    fbc_state(&row, &col, NULL, NULL);
    K_EXPECT_EQ(row, r0 + 1);
    K_EXPECT_EQ(col, 0);

    /* exactly `cols` characters fill the row and wrap to the next one */
    fbc_move(r0, 0);
    for (int i = 0; i < cols; i++) fb_console_putc('.');
    fbc_state(&row, &col, NULL, NULL);
    K_EXPECT_EQ(col, 0);
    K_EXPECT_EQ(row, r0 + 1);

    /* a run longer than two rows wraps inside the grid instead of running
     * off the bottom: scrolling there would push the boot log away, so the
     * row it ends on is checked against the last one as well */
    fbc_move(r0, 0);
    for (u32 i = 0; i < (u32)cols * 2u + 5u; i++) fb_console_putc('x');
    fbc_state(&row, &col, NULL, NULL);
    K_EXPECT(row >= 0 && row < rows);
    K_EXPECT(col >= 0 && col < cols);
    K_EXPECT(row < rows - 1); /* short of the last row: nothing scrolled */

    /* an unterminated sequence changes no state at all -- and closing it
     * later must not move the cursor either */
    fbc_state(&k_row, &k_col, NULL, NULL);
    fbc_str("\033[");
    fbc_state(&row, &col, NULL, NULL);
    K_EXPECT_EQ(row, k_row);
    K_EXPECT_EQ(col, k_col);
    fbc_str("m"); /* ESC[m: reset colour, no cursor movement */
    fbc_state(&row, &col, NULL, NULL);
    K_EXPECT_EQ(row, k_row);
    K_EXPECT_EQ(col, k_col);

    /* hand the console back exactly as found: the three rows written above
     * go back to blank (spaces, so the screen is never cleared) and the
     * cursor returns to where the log left it */
    fbc_blank_row(r0, cols);
    fbc_blank_row(r0 + 1, cols);
    fbc_blank_row(r0 + 2, cols);
    fbc_move(c_row, c_col);
}

static void test_fb_console_scroll(void)
{
    static u32 before0[FB_CELL_W * FB_CELL_H];
    static u32 before1[FB_CELL_W * FB_CELL_H];
    static u32 after[FB_CELL_W * FB_CELL_H];
    int row = 0, col = 0, rows = 0, cols = 0;
    int c_row = 0, c_col = 0, r0 = 0;
    u32 diff0 = 0, diff1 = 0, glyphs = 0, dirt = 0;

    fbc_state(&row, &col, &rows, &cols);
    if (!fb_active || rows < 6 || cols < 2) {
        ktest_skip("frame buffer console too small");
        return;
    }
    if (rows > 99 || cols > 999) {
        ktest_skip("grid beyond the 7-byte CSI budget");
        return;
    }
    c_row = row;
    c_col = col;

    /* The marks go into the blank rows at the bottom: rows 0..c_row are the
     * boot log and must survive untouched, so the "row shows what the row
     * below it held" invariant is measured on our own rows instead of on
     * rows 0 and 1 (which would mean writing into the log).  The one thing
     * this case cannot avoid is the scroll itself: fbc_scroll() drops the top
     * row of the screen, so exactly one log line leaves with it. */
    r0 = rows - 4; /* two mark rows, one row for 'z', one row to park on */
    if (r0 <= c_row) {
        ktest_skip("log reaches the bottom rows");
        return;
    }

    /* mark (r0,0) and (r0+1,0); the cursor moves away, so both stay clean */
    fbc_move(r0, 0);
    fb_console_putc('@');
    fbc_move(r0 + 1, 0);
    fb_console_putc('#');
    K_ASSERT(cell_read(r0, 0, before0));
    K_ASSERT(cell_read(r0 + 1, 0, before1));

    /* the two marks must differ, or the promotion check below proves nothing */
    for (u32 i = 0; i < FB_CELL_W * FB_CELL_H; i++)
        if (before0[i] != before1[i]) glyphs++;
    K_EXPECT(glyphs > 0);

    /* exactly one scroll: a glyph on the last text row, then one newline */
    fbc_move(rows - 1, 0);
    fb_console_putc('z');
    fb_console_putc('\n');

    fbc_state(&row, &col, NULL, NULL);
    K_EXPECT_EQ(row, rows - 1); /* still on the last row, not off it */
    K_EXPECT_EQ(col, 0);

    /* every row now holds what the row below it held */
    K_ASSERT(cell_read(r0 - 1, 0, after));
    for (u32 i = 0; i < FB_CELL_W * FB_CELL_H; i++)
        if (after[i] != before0[i]) diff0++;
    K_ASSERT(cell_read(r0, 0, after));
    for (u32 i = 0; i < FB_CELL_W * FB_CELL_H; i++)
        if (after[i] != before1[i]) diff1++;
    if (diff0) kprintf("[ktest] scroll: %u px of the promoted row differ\n", diff0);
    if (diff1) kprintf("[ktest] scroll: %u px of the row after it differ\n", diff1);
    K_EXPECT_EQ(diff0, 0);
    K_EXPECT_EQ(diff1, 0);

    /* the glyph written on the last row travelled up with the scroll: proof
     * it happened, rather than the compared rows matching already */
    K_ASSERT(cell_read(rows - 2, 0, after));
    for (u32 i = 0; i < FB_CELL_W * FB_CELL_H; i++)
        if (after[i] != 0) dirt++;
    K_EXPECT(dirt > 0);

    /* the vacated bottom row is cleared to the background (read away from
     * column 0, where the software cursor block sits) */
    K_ASSERT(cell_read(rows - 1, cols - 1, after));
    for (u32 i = 0; i < FB_CELL_W * FB_CELL_H; i++)
        K_EXPECT_EQ(after[i], 0); /* black is 0 in every colour layout */

    /* blank our marks with spaces -- no screen clear, so the log (now one
     * row higher) stays exactly where the scroll put it -- and park the
     * cursor right behind the log tail */
    fbc_move(r0 - 1, 0);
    fb_console_putc(' ');
    fbc_move(r0, 0);
    fb_console_putc(' ');
    fbc_move(rows - 2, 0);
    fb_console_putc(' ');
    fbc_move(c_row > 0 ? c_row - 1 : 0, c_col);
}

/* ---- T-033: the surface is held by exactly one pid ----
 *
 * fb_claim()/fb_disown() are the entire policy behind the owner gate in
 * kernel/syscall.c.  The drawing primitives themselves never consult
 * them -- the console shares those and must never be refused by a user
 * process -- so this small state machine is the whole of what "two
 * privileged processes cannot overwrite each other" rests on, and it is
 * worth stating as a unit rather than only through two live processes.
 *
 * The first assertion is load-bearing: this runs at boot, before
 * anything has drawn, and an owner left standing here would refuse
 * systest's very first fb.fill -- a failure that would show up a long
 * way from its cause.  The last one says the test gave the surface back
 * in the state it found it. */
static void test_fb_owner_is_one_pid_at_a_time(void)
{
    K_ASSERT_EQ(fb_owner_get(), 0); /* nobody has drawn yet */

    /* the first drawer claims it, and keeps it across its own calls */
    K_ASSERT(fb_claim(101));
    K_ASSERT_EQ(fb_owner_get(), 101);
    K_ASSERT(fb_claim(101));
    K_ASSERT_EQ(fb_owner_get(), 101);

    /* a second process is refused, and the refusal takes nothing --
     * otherwise the one merely trying would end up holding the screen */
    K_ASSERT(!fb_claim(102));
    K_ASSERT_EQ(fb_owner_get(), 101);

    /* nor may a non-holder release it: this is exactly the call a
     * losing process's exit would make, and it must clear nothing */
    fb_disown(102);
    K_ASSERT_EQ(fb_owner_get(), 101);

    /* the holder leaving frees the surface for whoever comes next */
    fb_disown(101);
    K_ASSERT_EQ(fb_owner_get(), 0);
    K_ASSERT(fb_claim(102));
    K_ASSERT_EQ(fb_owner_get(), 102);
    fb_disown(102);

    K_ASSERT_EQ(fb_owner_get(), 0); /* nobody leaves holding it */
}

KTEST("fb", KTEST_LATE, test_fb_pack_layouts);
KTEST("fb", KTEST_LATE, test_fb_rgb_px_8bpp);
KTEST("fb", KTEST_LATE, test_fb_layout_restored);
KTEST("fb", KTEST_LATE, test_fb_console_clipping);
KTEST("fb", KTEST_LATE, test_fb_console_scroll);
KTEST("fb", KTEST_LATE, test_fb_owner_is_one_pid_at_a_time);
