/* EARLY self-tests: the contracts between assembly/ABI headers and the C
 * code that consumes them, plus the boot memory map the allocator is built
 * on.  Getting any of these wrong is silent corruption, so they run first. */
#include <sys/ktest.h>
#include <boot.h>
#include <sys/sched.h> /* struct intr_frame, struct cpu_ctx */
#include <console.h>

/* entry64.S pushes (ascending addresses): r15..r8, rbp, rdi, rsi, rdx, rcx,
 * rax, rbx, then intno, err, then the CPU frame rip, cs, rflags, ussp, usss.
 * If the struct drifts from that order every ISR reads the wrong registers
 * and iretq pops garbage -- assert the offsets the assembly promises. */
static void test_intr_frame_layout(void)
{
    K_EXPECT_EQ(sizeof(struct intr_frame), 22 * 8);
    K_EXPECT_EQ(offsetof(struct intr_frame, r15), 0 * 8);
    K_EXPECT_EQ(offsetof(struct intr_frame, r14), 1 * 8);
    K_EXPECT_EQ(offsetof(struct intr_frame, r13), 2 * 8);
    K_EXPECT_EQ(offsetof(struct intr_frame, r12), 3 * 8);
    K_EXPECT_EQ(offsetof(struct intr_frame, r11), 4 * 8);
    K_EXPECT_EQ(offsetof(struct intr_frame, r10), 5 * 8);
    K_EXPECT_EQ(offsetof(struct intr_frame, r9), 6 * 8);
    K_EXPECT_EQ(offsetof(struct intr_frame, r8), 7 * 8);
    K_EXPECT_EQ(offsetof(struct intr_frame, rbp), 8 * 8);
    K_EXPECT_EQ(offsetof(struct intr_frame, rdi), 9 * 8);
    K_EXPECT_EQ(offsetof(struct intr_frame, rsi), 10 * 8);
    K_EXPECT_EQ(offsetof(struct intr_frame, rdx), 11 * 8);
    K_EXPECT_EQ(offsetof(struct intr_frame, rcx), 12 * 8);
    K_EXPECT_EQ(offsetof(struct intr_frame, rax), 13 * 8);
    K_EXPECT_EQ(offsetof(struct intr_frame, rbx), 14 * 8);
    K_EXPECT_EQ(offsetof(struct intr_frame, intno), 15 * 8);
    K_EXPECT_EQ(offsetof(struct intr_frame, err), 16 * 8);
    K_EXPECT_EQ(offsetof(struct intr_frame, rip), 17 * 8);
    K_EXPECT_EQ(offsetof(struct intr_frame, cs), 18 * 8);
    K_EXPECT_EQ(offsetof(struct intr_frame, rflags), 19 * 8);
    K_EXPECT_EQ(offsetof(struct intr_frame, ussp), 20 * 8);
    K_EXPECT_EQ(offsetof(struct intr_frame, usss), 21 * 8);

    /* swtch() does `mov [rdi], rsp` / `mov rsp, [rsi]`: one pointer, nothing
     * else.  A context struct that grew would silently break context switch. */
    K_EXPECT_EQ(sizeof(struct cpu_ctx), 8);
}

/* The .def promises "entry i is exactly i, with no holes".  The header only
 * checks that numbers are <= LAST at compile time, so holes and renumbering
 * are still possible -- and they would break every user binary. */
static void test_syscall_table_contiguous(void)
{
    static const struct {
        const char *name;
        int nr;
    } tbl[] = {
#define LNXRM_SYS(name, nr) {#name, nr},
#include "abi/lnxrm_syscalls.def"
#undef LNXRM_SYS
    };
    const int n = (int)(sizeof(tbl) / sizeof(tbl[0]));

    K_EXPECT_EQ(n, LNXRM_SYSCALL_LAST - LNXRM_SYSCALL_FIRST + 1);
    for (int i = 0; i < n; i++) {
        K_EXPECT_EQ(tbl[i].nr, LNXRM_SYSCALL_FIRST + i); /* sorted, gap-free */
        K_EXPECT(tbl[i].name[0] != 0);
    }
    K_EXPECT_EQ(tbl[n - 1].nr, LNXRM_SYSCALL_LAST);
    K_EXPECT_EQ(SYS_read, 0);
    K_EXPECT_EQ(SYS_rename, 30);
}

/* The e820 map is the only description of RAM the kernel ever gets: pmm picks
 * its window straight from it, so entries must be sane before pmm_init(). */
static void test_boot_e820_map(void)
{
    K_EXPECT(bootinfo.map_len >= 1);
    K_EXPECT(bootinfo.map_len <= 64);

    u64 usable = 0;
    for (int i = 0; i < bootinfo.map_len; i++) {
        const struct e820_entry *e = &bootinfo.map[i];
        K_EXPECT(e->size != 0);              /* zero-length entries were filtered */
        K_EXPECT(e->type >= 1 && e->type <= 5); /* 1..5 only, 0 is invalid */
        K_EXPECT(e->addr + e->size >= e->addr); /* must not wrap */
        if (e->type == E820_RAM) usable += e->size;
    }
    K_EXPECT(usable >= 0x100000UL); /* at least 1 MiB of usable RAM */

    /* Overlapping entries would let the allocator hand out memory someone
     * else owns; a well-formed BIOS/GRUB map never overlaps. */
    for (int i = 0; i < bootinfo.map_len; i++) {
        const struct e820_entry *a = &bootinfo.map[i];
        for (int j = i + 1; j < bootinfo.map_len; j++) {
            const struct e820_entry *b = &bootinfo.map[j];
            K_EXPECT(a->addr >= b->addr + b->size || b->addr >= a->addr + a->size);
        }
    }
}

/* boot_read_cmdline() must always leave a terminated string: strlen() and
 * every parser downstream run off the end of bootinfo otherwise. */
static void test_cmdline_terminated(void)
{
    K_EXPECT_EQ(bootinfo.cmdline[sizeof(bootinfo.cmdline) - 1], 0);
    K_EXPECT(strlen(bootinfo.cmdline) < sizeof(bootinfo.cmdline));
}

KTEST("abi", KTEST_EARLY, test_intr_frame_layout);
KTEST("abi", KTEST_EARLY, test_syscall_table_contiguous);
KTEST("boot", KTEST_EARLY, test_boot_e820_map);
KTEST("boot", KTEST_EARLY, test_cmdline_terminated);
