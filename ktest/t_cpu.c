/* CPU-hardware self-tests: the irqsave spinlock contract, the TSC, the
 * kernel text-range predicate and the CR0.WP / CR4.SMEP mitigations.  The
 * EARLY cases work with interrupts off and no allocator -- exactly the
 * state start_kernel() is in -- plus one KTEST_MM case that runs after
 * cpu_init() has had its say about SMEP. */
#include <sys/ktest.h>
#include <sys/spinlock.h>
#include <sys/cpu.h>
#include <console.h>

/* declared in kernel/main.c, used by vfs.c to validate function pointers */
extern bool kern_text_ptr(u64 p);

static spinlock_t test_lock = SPINLOCK_INIT;

static u64 read_rflags(void)
{
    u64 f;
    __asm__ volatile("pushfq; pop %0" : "=r"(f));
    return f;
}

#define IF_BIT 0x200UL

static void test_spinlock_irqsave(void)
{
    K_EXPECT_EQ(test_lock.locked, 0);

    u64 if_before = read_rflags() & IF_BIT;
    u64 flags;

    spin_lock_irqsave(&test_lock, &flags);
    K_EXPECT_EQ(test_lock.locked, 1);
    /* Interrupts must be masked the moment the lock is taken: a lock held
     * with IF=1 self-deadlocks the instant the same CPU re-enters from IRQ. */
    K_EXPECT_EQ(read_rflags() & IF_BIT, 0);
    /* ... and the saved flags must be the pre-lock state, not "now". */
    K_EXPECT_EQ(flags & IF_BIT, if_before);
    spin_unlock_irqrestore(&test_lock, flags);
    K_EXPECT_EQ(test_lock.locked, 0);
    /* unlock restores exactly what lock saved */
    K_EXPECT_EQ(read_rflags() & IF_BIT, if_before);

    /* lock -> check -> unlock must be able to run twice (no one-shot state) */
    spin_lock_irqsave(&test_lock, &flags);
    K_EXPECT_EQ(test_lock.locked, 1);
    spin_unlock_irqrestore(&test_lock, flags);
    K_EXPECT_EQ(test_lock.locked, 0);
}

static void test_tsc_advances(void)
{
    u64 a = rdtsc();
    u64 b = rdtsc();
    K_EXPECT(b >= a); /* one CPU's TSC never runs backwards */

    volatile u64 sink = 0;
    for (int i = 0; i < 10000; i++) sink += (u64)i;
    K_EXPECT(rdtsc() > a);
    K_EXPECT(sink > 0);
}

static void test_kernel_text_range(void)
{
    extern char __kernel_start[], __kernel_end[];

    K_EXPECT(kern_text_ptr((u64)&test_kernel_text_range));
    K_EXPECT(kern_text_ptr((u64)__kernel_start));
    K_EXPECT(kern_text_ptr((u64)__kernel_end - 1));
    K_EXPECT(!kern_text_ptr((u64)__kernel_end));
    K_EXPECT(!kern_text_ptr(0));
    K_EXPECT(!kern_text_ptr(0x1000));
    /* below the link address and far above the image: both must be rejected,
     * otherwise the VFS would accept a "function pointer" from either */
    K_EXPECT(!kern_text_ptr(0xffffffff80000000UL));
    K_EXPECT(!kern_text_ptr(0xffffffffc0000000UL));
}

/* CR0.WP has to be on from the very first C instruction: setup.asm and
 * entry64.S's _start32 both set it together with CR0.PG, before
 * start_kernel() ever runs.  This case is the boot path's receipt. */
static void test_cr0_wp_on(void)
{
    u64 cr0;
    __asm__ volatile("mov %%cr0,%0" : "=r"(cr0));
    K_EXPECT(cr0 & CR0_WP);
}

/* cpu_init() (BSP) / cpu_protect_init() (AP) turn SMEP on behind a CPUID
 * gate; KTEST_MM runs after cpu_init(), so before any user page is executed
 * the bit must be live whenever the CPU offers it. */
static void test_cr4_smep_on(void)
{
    u64 cr4;
    __asm__ volatile("mov %%cr4,%0" : "=r"(cr4));
    if (!cpu_has_smep()) {
        ktest_skip("CPU does not advertise SMEP");
        return;
    }
    K_EXPECT(cr4 & CR4_SMEP);
}

/* Same contract for SMAP, plus the one invariant the uaccess helpers hang
 * off: g_smap must track the actual CR4 bit (g_smap=1 with CR4.SMAP=0 would
 * #UD on STAC; g_smap=0 with CR4.SMAP=1 would #PF every un-windowed user
 * access).  And copy_from_user() succeeding below the first SMAP'd access
 * is itself the STAC proof -- without it that memcpy #PFs the kernel. */
static void test_cr4_smap_on(void)
{
    u64 cr4;
    __asm__ volatile("mov %%cr4,%0" : "=r"(cr4));
    if (!cpu_has_smap()) {
        ktest_skip("CPU does not advertise SMAP");
        return;
    }
    K_EXPECT(cr4 & CR4_SMAP);
    K_EXPECT(g_smap);
}

/* The IDT, gate by gate.  isr_common() dispatches whatever vector the CPU
 * delivers; a gate that is missing, mistyped, IST-carrying or open to
 * ring 3 sends the fault it should catch somewhere else instead.  This
 * walks all 256 entries as sidt reads them back and pins the whole shape
 * idt_init() promises: present 64-bit interrupt gates on kernel CS, IST 0
 * everywhere (no fault stack exists -- GAP 3.4), DPL 3 on exactly the two
 * user-entry vectors, and the stub every vector must target. (2026-10-20) */
struct idt_gate16 {
    u16 off_lo;
    u16 sel;
    u8 ist;
    u8 type;
    u16 off_mid;
    u32 off_hi;
    u32 zero;
} __attribute__((packed));
struct idtr16 {
    u16 limit;
    u64 base;
} __attribute__((packed));

extern u64 isr_stub_table[]; /* entry64.S: stubs for vectors 0..48 */

static void test_idt_gate_table(void)
{
    struct idtr16 idtr;
    __asm__ volatile("sidt %0" : "=m"(idtr));

    /* 256 gates x 16 bytes - 1: a shorter limit means delivery of a high
     * vector would run off the end of the table */
    K_EXPECT_EQ(idtr.limit, 256 * 16 - 1);
    K_EXPECT(idtr.base != 0);

    const struct idt_gate16 *g = (const struct idt_gate16 *)idtr.base;
    u32 bad_present = 0, bad_type = 0, bad_sel = 0, bad_ist = 0;
    u32 bad_canon = 0, bad_stub = 0, bad_dpl = 0;
    u64 ipi_off[3] = {0, 0, 0};

    for (int v = 0; v < 256; v++) {
        u64 off = (u64)g[v].off_lo | ((u64)g[v].off_mid << 16) | ((u64)g[v].off_hi << 32);

        if (!(g[v].type & 0x80)) { bad_present++; continue; } /* P must be set */
        if ((g[v].type & 0x1F) != 0x0E) bad_type++;           /* 64-bit interrupt gate */
        if (g[v].zero != 0) bad_type++;                        /* packed pad dword */
        if (g[v].sel != 0x08) bad_sel++;                       /* kernel CS */
        if (g[v].ist != 0) bad_ist++;                          /* no IST configured anywhere */
        if ((i64)off >> 47 != -1) bad_canon++;                 /* kernel-half target */

        /* DPL: ring 3 only on int 0x30 (48) and int 0x80 (128) */
        u32 dpl = (g[v].type >> 5) & 3;
        u32 dpl_want = (v == 48 || v == 128) ? 3 : 0;
        if (dpl != dpl_want) bad_dpl++;

        /* which stub this gate must target */
        u64 want;
        if (v <= 48) want = isr_stub_table[v];
        else if (v == 128) want = isr_stub_table[48]; /* 0x80 lands on the syscall stub */
        else if (v >= 0xF0 && v <= 0xF2) continue;    /* IPI: own stubs, checked below */
        else want = isr_stub_table[19];               /* spurious vectors -> catch-all */
        if (off != want) bad_stub++;
    }

    /* the three IPI gates must exist and override the catch-all */
    for (int i = 0; i < 3; i++) {
        int v = 0xF0 + i;
        ipi_off[i] = (u64)g[v].off_lo | ((u64)g[v].off_mid << 16) | ((u64)g[v].off_hi << 32);
        K_EXPECT(ipi_off[i] != isr_stub_table[19]);
        K_EXPECT((g[v].type & 0x80) != 0);
    }
    K_EXPECT(ipi_off[0] != ipi_off[1] && ipi_off[1] != ipi_off[2]);

    K_EXPECT_EQ(bad_present, 0);
    K_EXPECT_EQ(bad_type, 0);
    K_EXPECT_EQ(bad_sel, 0);
    K_EXPECT_EQ(bad_ist, 0);
    K_EXPECT_EQ(bad_canon, 0);
    K_EXPECT_EQ(bad_stub, 0);
    K_EXPECT_EQ(bad_dpl, 0);

    /* the four vectors worth naming out loud: #DE and #PF are ring 0
     * only, the two syscall entries are ring 3 */
    K_EXPECT_EQ(g[0].type & 0x60, 0);    /* #DE  */
    K_EXPECT_EQ(g[14].type & 0x60, 0);   /* #PF  */
    K_EXPECT_EQ(g[48].type & 0x60, 0x60);  /* int 0x30 */
    K_EXPECT_EQ(g[128].type & 0x60, 0x60); /* int 0x80 */
}

KTEST("lock", KTEST_EARLY, test_spinlock_irqsave);
KTEST("cpu", KTEST_EARLY, test_tsc_advances);
KTEST("cpu", KTEST_EARLY, test_kernel_text_range);
KTEST("cpu", KTEST_EARLY, test_cr0_wp_on);
KTEST("cpu", KTEST_EARLY, test_idt_gate_table);
KTEST("cpu", KTEST_MM, test_cr4_smep_on);
KTEST("cpu", KTEST_MM, test_cr4_smap_on);
