/* Self-test runner: walks the linker-collected `.ktest` section, runs the
 * cases registered for the requested stage and reports a verdict.
 *
 * The counters and the note functions behind console.h's ASSERT()/WARN()/
 * BUG_ON() live in kernel/selftest.c -- production code asserts too, so
 * they must survive `make KTEST=0`.  This file is the optional half: the
 * harness (runner, checks, verdict), compiled only when ktest/ is. */
#include <sys/ktest.h>
#include <console.h>
#include <boot.h>

/* Declared by arch/kernel.ld around KEEP(*(.ktest)). */
extern const struct ktest_case __ktest_start[], __ktest_end[];

static const char *const stage_name[KTEST_STAGE_MAX] = {"early", "mm", "late"};

/* Harness-private state.  The pieces the note functions touch
 * (selftest_case_failed, selftest_bug_seen) live beside the counters in
 * kernel/selftest.c so they exist in every build. */
static struct {
    bool parsed;
    bool enabled;
    bool continue_on_fail; /* `ktest=continue`: report but keep booting */

    const char *suite; /* case currently running */
    const char *name;
    bool case_skipped;

    unsigned cases_run, cases_skipped, cases_failed;
} kt;

/* Does the kernel command line contain `key`?  No strstr() in this libc. */
static bool cmdline_has(const char *key)
{
    const char *s = bootinfo.cmdline;
    size_t n = strlen(key);
    for (; *s; s++)
        if (!strncmp(s, key, n)) return true;
    return false;
}

static void ktest_parse_cmdline(void)
{
    if (kt.parsed) return;
    kt.parsed = true;
    kt.enabled = !cmdline_has("ktest=off");
    kt.continue_on_fail = cmdline_has("ktest=continue");
}

/* The ASSERT/WARN/BUG_ON note functions moved to kernel/selftest.c: they
 * are production infrastructure and must link even in a `make KTEST=0`
 * build.  Everything below is the harness proper. */

/* ---- case-scoped checks (include/sys/ktest.h) ---- */

bool ktest_check(bool ok, const char *file, int line, const char *expr)
{
    if (ok) {
        __atomic_add_fetch(&selftest_pass, 1u, __ATOMIC_RELAXED);
        return true;
    }
    __atomic_add_fetch(&selftest_fail, 1u, __ATOMIC_RELAXED);
    selftest_case_failed = true;
    kprintf("[ktest] FAIL %s.%s %s:%d: %s\n", kt.suite ? kt.suite : "?",
            kt.name ? kt.name : "?", file, line, expr);
    return false;
}

bool ktest_check_eq(u64 got, u64 want, const char *file, int line, const char *gexpr,
                    const char *wexpr)
{
    if (got == want) {
        __atomic_add_fetch(&selftest_pass, 1u, __ATOMIC_RELAXED);
        return true;
    }
    __atomic_add_fetch(&selftest_fail, 1u, __ATOMIC_RELAXED);
    selftest_case_failed = true;
    kprintf("[ktest] FAIL %s.%s %s:%d: %s == %s (got=0x%lx want=0x%lx)\n",
            kt.suite ? kt.suite : "?", kt.name ? kt.name : "?", file, line, gexpr, wexpr, got,
            want);
    return false;
}

void ktest_skip(const char *why)
{
    if (kt.case_skipped || selftest_case_failed) return;
    kt.case_skipped = true;
    kprintf("[ktest] SKIP %s.%s: %s\n", kt.suite ? kt.suite : "?", kt.name ? kt.name : "?",
            why);
}

/* ---- runner ---- */

void ktest_run(enum ktest_stage stage)
{
    ktest_parse_cmdline();
    if (!kt.enabled) return;
    if (stage >= KTEST_STAGE_MAX) return;

    unsigned p0 = selftest_pass, f0 = selftest_fail;
    unsigned n = 0;

    for (const struct ktest_case *c = __ktest_start; c < __ktest_end; c++) {
        if (c->stage != (u8)stage) continue;
        kt.suite = c->suite;
        kt.name = c->name;
        selftest_case_failed = false;
        kt.case_skipped = false;
        n++;
        kt.cases_run++;
        BUG_ON(c->fn == NULL); /* a null slot means the section got corrupted */
        if (c->fn) c->fn();
        /* a failure outranks a skip: a case that gave up after a failed
         * check must never be filed as "skipped" */
        if (selftest_case_failed)
            kt.cases_failed++;
        else if (kt.case_skipped)
            kt.cases_skipped++;
    }
    kt.suite = kt.name = NULL;

    kprintf("[ktest] stage %s: %u cases, %u checks, %u failed\n", stage_name[stage], n,
            selftest_pass - p0, selftest_fail - f0);
}

/* The one verdict line the smoke test greps for.  Called once, after the
 * last stage and *before* the first user process is spawned: a broken
 * kernel must not reach user space first and be discovered afterwards. */
void ktest_summary(void)
{
    ktest_parse_cmdline();

    if (!kt.enabled) {
        kprintf("[ktest] selftest disabled (ktest=off)\n");
        kprintf("[selftest] disabled\n");
        return;
    }

    if (selftest_fail) {
        kprintf("\033[1;31m[ktest] selftest FAIL (%u/%u cases, %u checks failed)\033[0m\n",
                kt.cases_failed, kt.cases_run, selftest_fail);
        kprintf("\033[1;31m[selftest] FAILED (%u failed, %u pass, %u warn)\033[0m\n",
                selftest_fail, selftest_pass, selftest_warn);
        if (!kt.continue_on_fail || selftest_bug_seen)
            panic("kernel self-test failed");
        return;
    }

    /* An empty suite is a broken build, not a passing kernel: the only way
     * to be sure the registration path still works is to have cases. */
    if (!kt.cases_run) {
        kprintf("\033[1;31m[ktest] selftest FAIL (no cases registered)\033[0m\n");
        kprintf("\033[1;31m[selftest] FAILED (0 checks)\033[0m\n");
        panic("kernel self-test found no cases");
    }

    /* Keep "(N cases, M checks)" verbatim even when cases were skipped --
     * that substring is the machine-readable contract with smoke_test.py. */
    if (kt.cases_skipped)
        kprintf("\033[1;32m[ktest] selftest PASS (%u cases, %u checks)\033[0m [%u skipped]\n",
                kt.cases_run, selftest_pass, kt.cases_skipped);
    else
        kprintf("\033[1;32m[ktest] selftest PASS (%u cases, %u checks)\033[0m\n", kt.cases_run,
                selftest_pass);

    /* N == M is what makes this line meaningful: every check that ran held. */
    kprintf("\033[1;32m[selftest] %u/%u pass\033[0m", selftest_pass,
            selftest_pass + selftest_fail);
    if (selftest_warn) kprintf(" [%u warn]", selftest_warn);
    kprintf("\n");
}
