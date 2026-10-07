/* Assertion infrastructure (T-010): the counters that console.h's
 * ASSERT()/WARN()/BUG_ON() macros feed, and the note functions behind them.
 *
 * This lives in kernel/ rather than ktest/ on purpose: it is runtime
 * behaviour, not test code.  `make KTEST=0` drops the whole ktest/ suite
 * (runner and cases), but BUG_ON() in production code -- syscall.c has one
 * -- must still record and print, so the macros' targets belong to every
 * build.  The case scope belongs to the runner: ktest/ktest.c resets
 * selftest_case_failed when a case starts, and ktest_summary() reads both
 * flags for the verdict (never reached in a KTEST=0 build, where nothing
 * reads them back -- the increment and the line are all that remain).
 *
 * Boot runs single-CPU with interrupts off at every stage, so during the
 * self-test the counters need no lock.  Outside it the same counters are
 * hit by ASSERT()/WARN()/BUG_ON() from anywhere (including interrupt
 * context), hence the relaxed atomics -- a lost increment on a warning is
 * not worth a lock. */
#include <console.h>

/* T-010: the global counters the ASSERT/WARN/BUG_ON macros feed. */
unsigned selftest_pass, selftest_fail, selftest_warn;
/* Written by the note functions below; reset by ktest_run() per case. */
bool selftest_case_failed;
/* A BUG_ON fired: ktest_summary() treats it as fatal even under
 * `ktest=continue`. */
bool selftest_bug_seen;

static void note(const char *tag, const char *file, int line, const char *expr, const char *fmt,
                 __builtin_va_list ap)
{
    char msg[160];
    msg[0] = 0;
    if (fmt && *fmt) vsnprintf(msg, sizeof(msg), fmt, ap);
    kprintf("%s %s:%d: %s%s%s\n", tag, file, line, expr, msg[0] ? ": " : "", msg);
}

void selftest_note_fail(const char *file, int line, const char *expr, const char *fmt, ...)
{
    __builtin_va_list ap;
    __atomic_add_fetch(&selftest_fail, 1u, __ATOMIC_RELAXED);
    selftest_case_failed = true; /* no-op between cases; ktest_run() resets it */
    __builtin_va_start(ap, fmt);
    note("[selftest] FAIL", file, line, expr, fmt, ap);
    __builtin_va_end(ap);
}

void selftest_note_warn(const char *file, int line, const char *expr, const char *fmt, ...)
{
    __builtin_va_list ap;
    __atomic_add_fetch(&selftest_warn, 1u, __ATOMIC_RELAXED);
    __builtin_va_start(ap, fmt);
    note("[selftest] WARN", file, line, expr, fmt, ap);
    __builtin_va_end(ap);
}

/* A violated bug: record it as a failure and remember that no amount of
 * `ktest=continue` may talk the machine back into a working state. */
void selftest_note_bug(const char *file, int line, const char *expr)
{
    __atomic_add_fetch(&selftest_fail, 1u, __ATOMIC_RELAXED);
    selftest_bug_seen = true;
    selftest_case_failed = true;
    kprintf("[selftest] BUG %s:%d: %s\n", file, line, expr);
}
