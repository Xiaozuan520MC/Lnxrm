// SPDX-License-Identifier: Apache-2.0

/* PP2 pins (GAP section 5.3): the crash ring records exactly what the
 * console filter hides, the level knob clamps and round-trips, and
 * backtrace() walks a known call chain -- frames[3] must be the return
 * slot of the third wrapper, i.e. the address __builtin_return_address(0)
 * reports from inside it.
 *
 * The first two cases move the global console level, so every mutation
 * finishes and the original level is restored BEFORE any K_EXPECT runs:
 * a ktest failure prints through kprintf (an INFO message), and a check
 * that fired while the level was clamped low would swallow its own report
 * line -- worse, an early-returning K_ASSERT would leave the console
 * quiet for every boot marker downstream of this stage. */
#include <sys/klog.h>
#include <console.h>
#include <sys/ktest.h>

/* Scan [from, now) for a record whose text matches `want` exactly and
 * hand back its level.  Searching by text rather than by sequence number
 * keeps the check honest if an interrupt printed between the caller's
 * klog_count() snapshot and its klogf(): that message would have stolen
 * the seq, never the content. */
static int find_record(u32 from, const char *want, int *lvl_out)
{
    for (u32 s = from; s < klog_count(); s++) {
        char buf[160];
        int lvl;
        if (klog_fetch(s, &lvl, buf, sizeof buf) != 0) continue;
        if (strcmp(buf, want)) continue;
        if (lvl_out) *lvl_out = lvl;
        return 1;
    }
    return 0;
}

static void test_klog_ring_keeps_filtered_messages(void)
{
    int saved = klog_getlevel();
    u32 from = klog_count();

    klog_setlevel(LOG_WARN); /* the console now hides INFO and DEBUG */
    klogf(LOG_INFO, "[ktest] klog below-threshold record\n");
    klog_setlevel(saved); /* restore before anything can report */

    int lvl = -1;
    K_EXPECT(find_record(from, "[ktest] klog below-threshold record\n", &lvl));
    K_EXPECT_EQ(lvl, LOG_INFO);
}

static void test_klog_level_clamps_and_roundtrips(void)
{
    int saved = klog_getlevel();

    /* Sample everything while the level moves ... */
    klog_setlevel(LOG_DEBUG);
    int v_top = klog_getlevel();
    int s_dbg = klog_shown(LOG_DEBUG);
    klog_setlevel(99); /* clamps to the top level */
    int v_hi = klog_getlevel();
    klog_setlevel(-3); /* clamps to the bottom level */
    int v_lo = klog_getlevel();
    int s_info = klog_shown(LOG_INFO);
    int s_err = klog_shown(LOG_ERR);
    klog_setlevel(saved);
    int v_rest = klog_getlevel();
    int s_rest = klog_shown(LOG_INFO);

    /* ... then assert, with the console level provably back where it was. */
    K_EXPECT_EQ(v_top, LOG_DEBUG);
    K_EXPECT_EQ(s_dbg, 1);
    K_EXPECT_EQ(v_hi, LOG_DEBUG);
    K_EXPECT_EQ(v_lo, LOG_ERR);
    K_EXPECT_EQ(s_info, 0);
    K_EXPECT_EQ(s_err, 1);
    K_EXPECT_EQ(v_rest, saved);
    K_EXPECT_EQ(s_rest, saved >= LOG_INFO);
}

#define BT_MAX 16

static uptr bt_frames[BT_MAX];
static int bt_depth; /* raw backtrace() result, taken at the leaf */
static uptr bt_ret;  /* bt_outer's own return slot: the address under test */

/* Each wrapper does arithmetic on the result AFTER the call: that keeps
 * the call out of tail position, so -Os cannot elide the very frame the
 * walk is supposed to see. */
__attribute__((noinline)) static int bt_leaf(uptr *f, int m)
{
    int n = backtrace(f, m);
    bt_depth = n;
    return n + 1;
}

__attribute__((noinline)) static int bt_mid(uptr *f, int m)
{
    int n = bt_leaf(f, m);
    return n + 1;
}

__attribute__((noinline)) static int bt_outer(uptr *f, int m)
{
    bt_ret = (uptr)__builtin_return_address(0);
    int n = bt_mid(f, m);
    return n + 1;
}

static void test_backtrace_walks_known_chain(void)
{
    extern char __text_start[], __text_end[];

    int n = bt_outer(bt_frames, BT_MAX);

    /* Layout: this case -> bt_outer -> bt_mid -> bt_leaf -> backtrace, so
     * frames[0] is the call site inside bt_leaf, [1] inside bt_mid, [2]
     * inside bt_outer, [3] inside THIS case -- captured above. */
    K_EXPECT_EQ(n, bt_depth + 3); /* three wrappers each add 1 */
    K_EXPECT(bt_depth >= 4);
    if (bt_depth >= 4) K_EXPECT_EQ(bt_frames[3], bt_ret);

    int all_text = 1;
    for (int i = 0; i < bt_depth && i < BT_MAX; i++)
        if (bt_frames[i] < (uptr)__text_start || bt_frames[i] >= (uptr)__text_end)
            all_text = 0;
    K_EXPECT(all_text);
}

KTEST("klog", KTEST_LATE, test_klog_ring_keeps_filtered_messages);
KTEST("klog", KTEST_LATE, test_klog_level_clamps_and_roundtrips);
KTEST("klog", KTEST_LATE, test_backtrace_walks_known_chain);
