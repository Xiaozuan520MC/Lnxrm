/* Drawing primitives must clip: an off-screen rectangle or glyph is allowed
 * to be silly, it is not allowed to write a single byte.  Out-of-range
 * coordinates reach these functions from the fb_* syscalls (24-28), i.e.
 * from user space, so "zero writes" is a promise the GAP_ANALYSIS makes
 * about all four primitives.
 *
 * The probes are the four corners: if any stray write happens anywhere it
 * will show up in at least one of them (the rects are anchored at 0 or at
 * the far corner), and everything written on purpose is restored from a
 * raw snapshot so the console underneath stays readable. */
#include <sys/ktest.h>
#include <console.h>
#include <framebuffer.h>

struct corners {
    u32 tl, tr, bl, br;
};

static void snap(struct corners *c)
{
    fb_read_px(0, 0, &c->tl);
    fb_read_px(fb_width - 1, 0, &c->tr);
    fb_read_px(0, fb_height - 1, &c->bl);
    fb_read_px(fb_width - 1, fb_height - 1, &c->br);
}

static bool same(const struct corners *a, const struct corners *b)
{
    return a->tl == b->tl && a->tr == b->tr && a->bl == b->bl && a->br == b->br;
}

static void test_fill_rect_clipping(void)
{
    struct corners before, after;
    u32 probe;
    if (!fb_read_px(0, 0, &probe)) {
        ktest_skip("no framebuffer mapped");
        return;
    }
    snap(&before);

    u32 hot = 0xff00ff;
    /* entirely off-screen: right/below the visible area, one pixel past it */
    fb_fill_rect(fb_width, 0, 32, 32, hot);
    fb_fill_rect(0, fb_height, 32, 32, hot);
    fb_fill_rect(fb_width + 7, fb_height + 9, 64, 64, hot);
    fb_fill_rect(fb_width - 1, fb_height + 1, 8, 8, hot); /* col on, row off */
    fb_fill_rect(fb_width + 1, fb_height - 1, 8, 8, hot); /* row on, col off */
    /* x+w / y+h wrap around: the loop bound must not be fooled */
    fb_fill_rect(0xffffffffu, 0, 16, 16, hot);
    fb_fill_rect(0, 0xffffffffu, 16, 16, hot);
    fb_fill_rect(fb_width, fb_height, 0xffffffffu, 0xffffffffu, hot);
    /* degenerate rectangles paint nothing */
    fb_fill_rect(0, 0, 0, 64, hot);
    fb_fill_rect(0, 0, 64, 0, hot);

    snap(&after);
    K_EXPECT(same(&before, &after));

    /* ...but a rect that starts inside and runs off the bottom-right corner
     * must paint exactly the pixels that are on screen. */
    u32 p00, p10, p01, p11, l = 0, t = 0;
    fb_read_px(fb_width - 3, fb_height - 1, &l); /* one pixel left of it */
    fb_read_px(fb_width - 1, fb_height - 3, &t); /* one pixel above it */
    fb_read_px(fb_width - 2, fb_height - 2, &p00);
    fb_read_px(fb_width - 1, fb_height - 2, &p10);
    fb_read_px(fb_width - 2, fb_height - 1, &p01);
    fb_read_px(fb_width - 1, fb_height - 1, &p11);

    fb_fill_rect(fb_width - 2, fb_height - 2, 100, 100, hot);

    u32 painted;
    K_EXPECT(fb_read_px(fb_width - 1, fb_height - 1, &painted));
    K_EXPECT_EQ(painted, fb_rgb_px(hot)); /* the visible corner was written */
    K_EXPECT(fb_read_px(fb_width - 3, fb_height - 1, &probe));
    K_EXPECT_EQ(probe, l); /* one pixel left of the rect: untouched */
    K_EXPECT(fb_read_px(fb_width - 1, fb_height - 3, &probe));
    K_EXPECT_EQ(probe, t); /* one pixel above the rect: untouched */

    /* put the four pixels the rect legally painted back exactly as found */
    fb_write_px(fb_width - 2, fb_height - 2, p00);
    fb_write_px(fb_width - 1, fb_height - 2, p10);
    fb_write_px(fb_width - 2, fb_height - 1, p01);
    fb_write_px(fb_width - 1, fb_height - 1, p11);
    snap(&after);
    K_EXPECT(same(&before, &after));
}

static void test_draw_char_clipping(void)
{
    struct corners before, after;
    u32 probe;
    if (!fb_read_px(0, 0, &probe)) {
        ktest_skip("no framebuffer mapped");
        return;
    }
    snap(&before);

    u32 hot = 0x00ffff;
    fb_draw_char(fb_width, fb_height, 'A', hot, hot);
    fb_draw_char(fb_width + 8, 0, 'A', hot, hot);
    fb_draw_char(0, fb_height, 'A', hot, hot);
    snap(&after);
    K_EXPECT(same(&before, &after));

    /* a glyph straddling the corner: only its on-screen pixel may move */
    u32 corner, left, above;
    fb_read_px(fb_width - 1, fb_height - 1, &corner);
    fb_read_px(fb_width - 2, fb_height - 1, &left);
    fb_read_px(fb_width - 1, fb_height - 2, &above);

    fb_draw_char(fb_width - 1, fb_height - 1, 'M', hot, hot);

    K_EXPECT(fb_read_px(fb_width - 1, fb_height - 1, &probe));
    K_EXPECT_EQ(probe, fb_rgb_px(hot)); /* fg == bg: any write is visible */
    K_EXPECT(fb_read_px(fb_width - 2, fb_height - 1, &probe));
    K_EXPECT_EQ(probe, left);
    K_EXPECT(fb_read_px(fb_width - 1, fb_height - 2, &probe));
    K_EXPECT_EQ(probe, above);

    fb_write_px(fb_width - 1, fb_height - 1, corner);
    snap(&after);
    K_EXPECT(same(&before, &after));
}

KTEST("draw", KTEST_LATE, test_fill_rect_clipping);
KTEST("draw", KTEST_LATE, test_draw_char_clipping);
