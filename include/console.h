#pragma once
#include <types.h>

#ifdef __cplusplus
extern "C" {
#endif

void kprintf(const char *fmt, ...);
void kvprintf(const char *fmt, __builtin_va_list ap);
void console_init(void);
void console_putc(char c); /* dual: VGA + COM1 */

/* Shared console serialization (print.c owns the lock). Both kprintf
 * and console_write take it irqsave; see print.c for the rationale. */
void console_out_lock(u64 *flags);
void console_out_unlock(u64 flags);

void panic(const char *fmt, ...) __attribute__((noreturn));

/* ---- assertion infrastructure (T-010, live in kernel/selftest.c) ----
 * Three macros, one set of counters, one machine-readable verdict:
 *
 *   ASSERT(cond, fmt, ...)  a check the kernel must pass; a hit that fails
 *                           counts as a self-test failure.
 *   WARN(cond, fmt, ...)    something noticed but safe to continue past;
 *                           counts as a warning and never fails the run.
 *   BUG_ON(cond)            an invariant whose violation means the kernel
 *                           state cannot be trusted: fails, and the summary
 *                           stops the machine even under `ktest=continue`.
 *
 * Every failure prints file:line and the failing expression, so an injected
 * bug is greppable (`[selftest] FAIL`, `[ktest] FAIL`); ktest_summary()
 * reports `[selftest] N/M pass` when everything held. */
extern unsigned selftest_pass; /* checks that passed */
extern unsigned selftest_fail; /* checks that failed */
extern unsigned selftest_warn; /* warnings raised */
/* Set by the note_* functions below; the runner in ktest/ resets the case
 * flag when a case starts and ktest_summary() reads both for the verdict. */
extern bool selftest_case_failed; /* a check failed inside the running case */
extern bool selftest_bug_seen;    /* a BUG_ON fired: always fatal */

void selftest_note_fail(const char *file, int line, const char *expr, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
void selftest_note_warn(const char *file, int line, const char *expr, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));
void selftest_note_bug(const char *file, int line, const char *expr);

#define ASSERT(cond, fmt, ...)                                                \
    do {                                                                      \
        if (cond)                                                             \
            __atomic_add_fetch(&selftest_pass, 1u, __ATOMIC_RELAXED);         \
        else                                                                  \
            selftest_note_fail(__FILE__, __LINE__, #cond, fmt, ##__VA_ARGS__); \
    } while (0)

#define WARN(cond, fmt, ...)                                                  \
    do {                                                                      \
        if (!(cond))                                                          \
            selftest_note_warn(__FILE__, __LINE__, #cond, fmt, ##__VA_ARGS__); \
    } while (0)

#define BUG_ON(cond)                                          \
    do {                                                      \
        if (cond) selftest_note_bug(__FILE__, __LINE__, #cond); \
    } while (0)

size_t vsnprintf(char *buf, size_t size, const char *fmt, __builtin_va_list ap);

/* ring buffer for replaying boot messages in framebuffer console */
int ring_get_count(void);
char ring_get_char(int i);

/* ---- ANSI (CSI) escape tokenizer (lib/.. ansi.c) ----
 * Both console backends (VGA text in print.c, framebuffer in framebuffer.c)
 * consume character streams that may contain sequences like "ESC [ 3 2 m".
 * Only the tokenizing state machine lives here; interpreting the sequence is
 * device specific. */

#define ANSI_SEQ_MAX 8

struct ansi_seq {
    int phase; /* 0 = text, 1 = got ESC, 2 = got ESC[ */
    int len;
    char buf[ANSI_SEQ_MAX]; /* params + final byte, NUL terminated */
};

enum {
    ANSI_TEXT = -1,    /* ordinary character: render it */
    ANSI_CONSUMED = 0, /* part of a sequence: swallow it */
    ANSI_DONE = 1,     /* complete sequence in s->buf */
};

/* Feed one character. On ANSI_DONE, s->buf holds the bytes after "ESC[",
 * including the terminating letter (NUL terminated). */
int ansi_feed(struct ansi_seq *s, char c);

/* ANSI colour code (30-37 foreground / 40-47 background) to palette index. */
int ansi_color_index(int code);

/* string (lib/string.c) */
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strcasecmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *d, const char *s);
char *strncpy(char *d, const char *s, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
int memcmp(const void *a, const void *b, size_t n);

#ifdef __cplusplus
}
#endif
