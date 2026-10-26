/* mouse 37-38 surface: the aux ring driven through the real dispatch
 * (syscall_probe = gates + pointer checks + error codes).  KTEST_LATE
 * runs after ps2_mouse_init() but headless boots deliver no aux bytes,
 * so the ring starts empty -- asserted loudly.  ms_feed_byte() is then
 * the sole producer, exercising the exact path the ISR rides: 9-bit
 * sign extension, overflow zeroing, header sanity, resync counting,
 * FIFO order and the 63-event capacity with honest drops.
 *
 * Deliberately NOT probed here: the demoted tier's -13 (setuid mutates
 * the caller's identity, which t_syscall refuses on principle -- the
 * user-tier gate answers are user/systest's); the inited flag (a real
 * reset probe decides it, and only monotonicity-style facts hold).
 * (2026-10-26) */
#include <sys/ktest.h>
#include <console.h>
#include <mm/mm.h>
#include <sys/sched.h>
#include <sys/cpu.h>
#include <sys/vfs.h>
#include <abi/lnxrm_abi.h>

#define MS_VA   (USER_BASE + 0xE000) /* nobody else in ktest/ maps this */
#define MST_VA  (MS_VA + 0x40)       /* mouse_state receiver */
#define MINFO_VA (MS_VA + 0x80)      /* mouse_info receiver */
#define BAD_VA  (USER_BASE + 0xC000) /* kept unmapped for EFAULT probes */

static void uput(u64 va, const void *src, size_t n)
{
    u64 ac = smap_enter();
    memcpy((void *)va, src, n);
    smap_leave(ac);
}

static void uget(void *dst, u64 va, size_t n)
{
    u64 ac = smap_enter();
    memcpy(dst, (const void *)va, n);
    smap_leave(ac);
}

/* one synthetic 3-byte packet, flags first */
static void ms_packet(u8 flags, u8 dx, u8 dy)
{
    ms_feed_byte(flags);
    ms_feed_byte(dx);
    ms_feed_byte(dy);
}

static void inspect_info(struct lnxrm_mouse_info *m, u64 va)
{
    K_EXPECT_EQ(syscall_probe(SYS_mouse_info, va, 0, 0), 0);
    uget(m, va, sizeof(*m));
}

static void test_mouse_syscall_surface(void)
{
    struct lnxrm_mouse_info s0 = {0};
    u64 swpa = pmm_alloc();
    u8 page[256];

    K_ASSERT(swpa != 0);
    K_ASSERT_EQ(vmm_map_user(current->pml4, MS_VA, swpa, true, true, false), 0);
    ms_stats(&s0);
    K_ASSERT_EQ(ms_pending(), 0);

    /* the ABI struct: counters, zero tail, and the EFAULTs the copy takes
     * (validation-then-copy before any event moves; NULL is refused by the
     * call itself, and mouse_get refuses a bad receiver even when empty) */
    inspect_info(&s0, MINFO_VA);
    K_EXPECT_EQ((long)s0.capacity, 63);
    K_EXPECT(s0.pending <= s0.capacity);
    K_EXPECT(s0.moved_total <= s0.packets_total);
    K_EXPECT(s0.dropped <= s0.packets_total);
    K_EXPECT_EQ(s0.reserved[0], 0);
    K_EXPECT_EQ(syscall_probe(SYS_mouse_info, 0, 0, 0), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_mouse_info, 0x8000000000000000ULL, 0, 0),
                LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_mouse_info, (u64)page, 0, 0), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_mouse_info, BAD_VA, 0, 0), LNXRM_EFAULT);

    struct lnxrm_mouse_state st = {0};
    uput(MST_VA, &st, sizeof(st));
    K_EXPECT_EQ(syscall_probe(SYS_mouse_get, MST_VA, 0, 0), LNXRM_EAGAIN);
    K_EXPECT_EQ(syscall_probe(SYS_mouse_get, (u64)page, 0, 0), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_mouse_get, MST_VA + 0xFFF, 0, 0),
                LNXRM_EFAULT); /* second page not mapped */
    K_EXPECT_EQ(syscall_probe(SYS_mouse_get, BAD_VA, 0, 0), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_mouse_get, 0, 0, 0), LNXRM_EFAULT);

    /* the standard packet: LB down, dx=+3, dy=-2 -- flags 0x29 carry the
     * 0x08 sanity bit, the Y sign, and the button bit */
    ms_packet(0x29, 0x03, 0xFE);
    K_EXPECT_EQ(syscall_probe(SYS_mouse_get, MST_VA, 0, 0), 0);
    uget(&st, MST_VA, sizeof(st));
    K_EXPECT_EQ(st.dx, 3);
    K_EXPECT_EQ(st.dy, -2);
    K_EXPECT_EQ(st.buttons, 1u);
    K_EXPECT_EQ(st.reserved, 0u);
    K_EXPECT_EQ(syscall_probe(SYS_mouse_get, MST_VA, 0, 0), LNXRM_EAGAIN);
    K_EXPECT_EQ(syscall_probe(SYS_mouse_get, 0xdeadbeef, 0, 0), LNXRM_EFAULT);

    /* no buttons, dx=-1 (9-bit sign, 0xFF low byte), dy=0 */
    ms_packet(0x18, 0xFF, 0x00);
    K_EXPECT_EQ(syscall_probe(SYS_mouse_get, MST_VA, 0, 0), 0);
    uget(&st, MST_VA, sizeof(st));
    K_EXPECT_EQ(st.dx, -1);
    K_EXPECT_EQ(st.dy, 0);
    K_EXPECT_EQ(st.buttons, 0u);

    /* overflow: both deltas forced to 0, never garbage buttons */
    ms_packet(0xC8, 0x7F, 0x81); /* Xov | Yov */
    K_EXPECT_EQ(syscall_probe(SYS_mouse_get, MST_VA, 0, 0), 0);
    uget(&st, MST_VA, sizeof(st));
    K_EXPECT_EQ(st.dx, 0);
    K_EXPECT_EQ(st.dy, 0);
    K_EXPECT_EQ(st.buttons, 0u);

    /* a flags byte without the sanity bit is a dropped stream byte */
    ms_feed_byte(0x05);
    ms_packet(0x08, 0x02, 0x00); /* next packet re-syncs normally */
    K_EXPECT_EQ(syscall_probe(SYS_mouse_get, MST_VA, 0, 0), 0);
    uget(&st, MST_VA, sizeof(st));
    K_EXPECT_EQ(st.dx, 2);
    K_EXPECT_EQ(st.dy, 0);

    /* accounting: 4 accepted events (the resync byte never became one),
     * 3 of them moved, 1 resync, empty ring, nothing dropped */
    struct lnxrm_mouse_info s1;
    ms_stats(&s1);
    K_EXPECT_EQ((long)s1.packets_total, (long)s0.packets_total + 4);
    K_EXPECT_EQ((long)s1.moved_total, (long)s0.moved_total + 3);
    K_EXPECT_EQ((long)s1.resyncs, (long)s0.resyncs + 1);
    K_EXPECT_EQ((long)s1.dropped, (long)s0.dropped);
    K_EXPECT_EQ((long)ms_pending(), 0);

    /* teardown: exactly the frame we mapped, and no phantom event left */
    K_EXPECT_EQ(vmm_unmap_user(current->pml4, MS_VA), swpa);
    pmm_free(swpa);
    K_EXPECT_EQ(vmm_translate_in(current->pml4, MS_VA), 0);
    K_EXPECT_EQ((long)ms_pending(), 0);
}

static void test_mouse_ring_fill_and_drop(void)
{
    struct lnxrm_mouse_info s0, s1;

    ms_stats(&s0);
    K_ASSERT_EQ((long)ms_pending(), 0);
    K_ASSERT_EQ((long)s0.capacity, 63);

    /* 63 events exactly fill the ring; the 64th is a loud, counted drop */
    for (u8 i = 1; i <= 63; i++) ms_packet(0x08, i, 0x00);
    K_EXPECT_EQ((long)ms_pending(), 63);
    ms_packet(0x08, 0x40, 0x00);
    ms_stats(&s1);
    K_EXPECT_EQ((long)s1.dropped, (long)s0.dropped + 1);
    K_EXPECT_EQ((long)s1.pending, 63);
    K_EXPECT_EQ((long)s1.packets_total, (long)s0.packets_total + 63);

    /* and every one of them comes back in order through mouse_get */
    struct lnxrm_mouse_state st = {0};
    for (int i = 1; i <= 63; i++) {
        K_EXPECT_EQ(mouse_pop(&st), 0);
        K_EXPECT_EQ(st.dx, i);
        K_EXPECT_EQ(st.buttons, 0u);
    }
    K_EXPECT_EQ(mouse_pop(&st), -1);
    K_EXPECT_EQ((long)ms_pending(), 0);
}

KTEST("mouse", KTEST_LATE, test_mouse_syscall_surface);
KTEST("mouse", KTEST_LATE, test_mouse_ring_fill_and_drop);
