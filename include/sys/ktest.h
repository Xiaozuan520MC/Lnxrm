/* Kernel self-test framework: the kernel asserts its own invariants while it
 * boots, so "I believe pmm hands back aligned frames" becomes "pmm proved it
 * on this machine, on this boot".
 *
 * HOW IT WORKS
 *   Every test case is one `struct ktest_case` dropped into the `.ktest`
 *   section (KEEP'd by arch/kernel.ld, so --gc-sections cannot throw it
 *   away).  The runner walks that section and calls the cases whose stage
 *   matches.  Nothing is registered at runtime and nothing needs a central
 *   list: adding a test means adding one function and one KTEST() line.
 *
 * ADDING A TEST
 *     static void test_pmm_alignment(void)          // static, void, no args
 *     {
 *         u64 pa = pmm_alloc_order(0);
 *         K_ASSERT(pa != 0);                        // failure aborts the case
 *         K_EXPECT_EQ(pa & 0xfff, 0);               // failure is recorded only
 *     }
 *     KTEST("mm", KTEST_MM, test_pmm_alignment);
 *
 * STAGES  (start_kernel runs them in this order)
 *   KTEST_EARLY  right after console_init(): no allocator yet, stack only.
 *   KTEST_MM     after pmm/vmm/kheap/cpu are up.
 *   KTEST_LATE   after the root filesystem and the scheduler, before the
 *                first user process is spawned.
 *
 * REPORTING
 *   Every failed check prints one greppable line:
 *       [ktest] FAIL <suite>.<name> <file>:<line>: <expr> [got=.. want=..]
 *   Checks that are not inside a case (or a bare ASSERT() in kernel code)
 *   print as [selftest] FAIL <file>:<line>: ...; see console.h for the
 *   ASSERT/WARN/BUG_ON triple and the selftest_pass/fail/warn counters they
 *   share with the macros above.
 *   Each stage ends with a count line, and ktest_summary() prints the two
 *   machine-readable verdicts the smoke test looks for:
 *       [ktest] selftest PASS (N cases, M checks)
 *       [selftest] N/M pass
 *   A failure prints [selftest] FAILED and stops the kernel (panic) before
 *   any user process is spawned.  `ktest=continue` boots on past a failure
 *   -- but never past a BUG_ON; `ktest=off` skips the whole framework.
 */
#pragma once
#include <types.h>

#ifdef __cplusplus
extern "C" {
#endif

enum ktest_stage {
    KTEST_EARLY = 0,
    KTEST_MM = 1,
    KTEST_LATE = 2,
    KTEST_STAGE_MAX
};

struct ktest_case {
    const char *suite;
    const char *name;
    u8 stage;
    void (*fn)(void);
};

/* Register `fn` (a `static void fn(void)` defined above this line). */
#define KTEST(suite, stage, fn)                                        \
    static const struct ktest_case __ktest_case_##fn                   \
        __attribute__((used, section(".ktest"))) = {suite, #fn, (u8)(stage), fn}

/* One failed check: record it, print it, hand back `ok` so K_ASSERT can bail. */
bool ktest_check(bool ok, const char *file, int line, const char *expr);
bool ktest_check_eq(u64 got, u64 want, const char *file, int line, const char *gexpr,
                    const char *wexpr);
/* Mark the running case as skipped (not a failure) and give a reason. */
void ktest_skip(const char *why);

/* Run every registered case of one stage. */
void ktest_run(enum ktest_stage stage);
/* Print the overall verdict (call once, after the last stage). */
void ktest_summary(void);

/* A failing check the case cannot continue past: returns from the caller. */
#define K_ASSERT(cond)                                                       \
    do {                                                                     \
        if (!ktest_check(!!(cond), __FILE__, __LINE__, #cond)) return;       \
    } while (0)

#define K_ASSERT_EQ(got, want)                                               \
    do {                                                                     \
        if (!ktest_check_eq((u64)(got), (u64)(want), __FILE__, __LINE__, #got, #want)) \
            return;                                                          \
    } while (0)

/* A failing check worth recording but not worth stopping the case for. */
#define K_EXPECT(cond) ((void)ktest_check(!!(cond), __FILE__, __LINE__, #cond))
#define K_EXPECT_EQ(got, want) \
    ((void)ktest_check_eq((u64)(got), (u64)(want), __FILE__, __LINE__, #got, #want))

#ifdef __cplusplus
}
#endif
