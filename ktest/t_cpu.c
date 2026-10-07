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

KTEST("lock", KTEST_EARLY, test_spinlock_irqsave);
KTEST("cpu", KTEST_EARLY, test_tsc_advances);
KTEST("cpu", KTEST_EARLY, test_kernel_text_range);
KTEST("cpu", KTEST_EARLY, test_cr0_wp_on);
KTEST("cpu", KTEST_MM, test_cr4_smep_on);
KTEST("cpu", KTEST_MM, test_cr4_smap_on);
