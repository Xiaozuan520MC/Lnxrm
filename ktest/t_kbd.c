/* kbd 34-36 surface: the input stream driven through the real dispatch
 * (syscall_probe = gates + pointer checks + error codes).  KTEST_LATE
 * runs before userland exists and the harness types nothing, so the ring
 * starts empty -- asserted loudly, because a byte there would mean
 * something fed the console before any reader ever existed.  This case
 * is then the sole producer/consumer on the ring, so every count is
 * exact: FIFO order, capacity 255, drop accounting, counters.
 *
 * Deliberately NOT probed here: the demoted tier's -13 (setuid mutates
 * the caller's identity, which t_syscall refuses on principle -- the
 * user-tier gate answers are userland/systest's); the PS/2 scancode counter
 * (hardwareIRQ1 -- only monotonicity holds). (2026-10-25) */
#include <sys/ktest.h>
#include <console.h>
#include <mm/mm.h>
#include <sys/sched.h>
#include <sys/cpu.h>
#include <sys/vfs.h>
#include <abi/lnxrm_abi.h>

#define KBD_VA  (USER_BASE + 0xA000) /* nobody else in ktest/ maps this */
#define INJ_VA  (KBD_VA + 0x40)
#define KINFO_VA (KBD_VA + 0x80)
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

static void test_kbd_syscall_surface(void)
{
    struct lnxrm_kbd_info s0 = {0};
    u64 swpa = pmm_alloc();
    u8 page[256];

    K_ASSERT(swpa != 0);
    K_ASSERT_EQ(vmm_map_user(current->pml4, KBD_VA, swpa, true, true, false), 0);
    kbd_stats(&s0);
    K_ASSERT_EQ(kbd_pending(), 0);

    /* the ABI struct: counters, zero tail, and the EFAULTs the copy takes
     * (validation-then-copy, the W^X mechanism t_usercopy proves; NULL
     * is refused by the call itself) */
    K_EXPECT_EQ(syscall_probe(SYS_kbd_info, KINFO_VA, 0, 0), 0);
    struct lnxrm_kbd_info ki = {0};
    uget(&ki, KINFO_VA, sizeof(ki));
    K_EXPECT_EQ((long)ki.capacity, 255);
    K_EXPECT(ki.pending <= ki.capacity);
    K_EXPECT(ki.dropped <= ki.pushed_total);
    K_EXPECT_EQ(ki.reserved[0] | ki.reserved[1] | ki.reserved[2], 0);
    K_EXPECT_EQ(syscall_probe(SYS_kbd_info, 0, 0, 0), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_kbd_info, 0x8000000000000000ULL, 0, 0),
                LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_kbd_info, (u64)page, 0, 0), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_kbd_info, BAD_VA, 0, 0), LNXRM_EFAULT);

    /* kbd_get on an empty ring: EAGAIN, never a fake byte */
    K_EXPECT_EQ(syscall_probe(SYS_kbd_get, 0, 0, 0), LNXRM_EAGAIN);
    K_EXPECT_EQ(syscall_probe(SYS_kbd_get, 0xdeadbeef, 7, 7), LNXRM_EAGAIN);

    /* inject: pointer validation first (len==0 is a legal pointer-free
     * no-op, exactly 0..256 keys, and a page-boundary straddle is EFAULT) */
    uput(INJ_VA, (u8[]){'Q', 'w', 'e'}, 3);
    K_EXPECT_EQ(syscall_probe(SYS_kbd_inject, 0, 3, 0), LNXRM_EFAULT);
    K_EXPECT_EQ(syscall_probe(SYS_kbd_inject, KBD_VA + 0xFFF, 2, 0),
                LNXRM_EFAULT); /* second page not mapped */
    K_EXPECT_EQ(syscall_probe(SYS_kbd_inject, INJ_VA, 300, 0), LNXRM_EINVAL);
    K_EXPECT_EQ(syscall_probe(SYS_kbd_inject, 0, 0, 0), 0); /* 0 keys: no-op */
    K_EXPECT_EQ(syscall_probe(SYS_kbd_inject, INJ_VA, 3, 0), 3);

    /* exactly those three keys, FIFO, then the ring is empty again */
    K_EXPECT_EQ(syscall_probe(SYS_kbd_get, 0, 0, 0), 'Q');
    K_EXPECT_EQ(syscall_probe(SYS_kbd_get, 0, 0, 0), 'w');
    K_EXPECT_EQ(syscall_probe(SYS_kbd_get, 0, 0, 0), 'e');
    K_EXPECT_EQ(syscall_probe(SYS_kbd_get, 0, 0, 0), LNXRM_EAGAIN);

    /* byte 0xff must come back as 255, not as the errno side of the line */
    uput(INJ_VA, (u8[]){0x12, 0xff, 0x00}, 3);
    K_EXPECT_EQ(syscall_probe(SYS_kbd_inject, INJ_VA, 3, 0), 3);
    K_EXPECT_EQ(syscall_probe(SYS_kbd_get, 0, 0, 0), 0x12);
    K_EXPECT_EQ(syscall_probe(SYS_kbd_get, 0, 0, 0), 255);
    K_EXPECT_EQ(syscall_probe(SYS_kbd_get, 0, 0, 0), 0);
    K_EXPECT_EQ(syscall_probe(SYS_kbd_get, 0, 0, 0), LNXRM_EAGAIN);

    /* accounting: three accepted injections, one EFAULT sweep that moved
     * nothing, and the counters monotonic */
    struct lnxrm_kbd_info s1;
    kbd_stats(&s1);
    K_EXPECT_EQ((long)s1.pushed_total, (long)s0.pushed_total + 6);
    K_EXPECT_EQ((long)s1.dropped, (long)s0.dropped);
    K_EXPECT(s1.ps2_scans >= s0.ps2_scans);

    /* teardown: exactly the frame we mapped, and no phantom key left */
    K_EXPECT_EQ(vmm_unmap_user(current->pml4, KBD_VA), swpa);
    pmm_free(swpa);
    K_EXPECT_EQ(vmm_translate_in(current->pml4, KBD_VA), 0);
    K_EXPECT_EQ((long)kbd_pending(), 0);
}

static void test_kbd_ring_fill_and_drop(void)
{
    struct lnxrm_kbd_info s0, s1;

    kbd_stats(&s0);
    K_ASSERT_EQ(kbd_pending(), 0);
    K_ASSERT_EQ((long)s0.capacity, 255);

    /* 255 keys exactly fill the ring; the 256th is a loud, counted drop */
    for (u32 i = 1; i <= 255; i++) input_push((char)i);
    K_EXPECT_EQ((long)kbd_pending(), 255);
    input_push((char)0xF0);
    kbd_stats(&s1);
    K_EXPECT_EQ((long)s1.dropped, (long)s0.dropped + 1);
    K_EXPECT_EQ((long)s1.pending, 255);
    K_EXPECT_EQ((long)s1.pushed_total, (long)s0.pushed_total + 255);

    /* and every one of them comes back in order through kbd_get */
    for (u32 i = 1; i <= 255; i++)
        K_EXPECT_EQ(kbd_get_byte(), (long)i);
    K_EXPECT_EQ(kbd_get_byte(), LNXRM_EAGAIN);
    K_EXPECT_EQ((long)kbd_pending(), 0);
}

KTEST("kbd", KTEST_LATE, test_kbd_syscall_surface);
KTEST("kbd", KTEST_LATE, test_kbd_ring_fill_and_drop);
