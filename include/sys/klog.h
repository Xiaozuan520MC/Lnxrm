// SPDX-License-Identifier: Apache-2.0

/* Kernel log levels + crash ring buffer (PP2 observability, GAP section 5.3).
 *
 * Two halves of one idea:
 *   - the CONSOLE filter (klog_setlevel) decides what reaches VGA/fb/serial
 *     right now -- quiet a noisy boot without losing the ability to see it;
 *   - the RING (kernel/core/klog.c) keeps every formatted message
 *     regardless of the filter, so a quiet run still has a full transcript
 *     when the machine dies: panic() dumps the tail before the halt, and
 *     ktest can assert on log content by sequence number instead of
 *     scraping the console.
 *
 * print.c feeds every kprintf()/klogf() message through klog_record();
 * the console half is a threshold, not a channel switch: levels at or
 * below klog_getlevel() print, everything is recorded. */
#pragma once
#include <types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Severity, low = severe (Linux console_loglevel convention, compressed
 * to the four levels this kernel actually uses). */
enum { LOG_ERR = 0, LOG_WARN = 1, LOG_INFO = 2, LOG_DEBUG = 3 };

/* Parse `loglevel=<0..3>` from bootinfo.cmdline (QEMU -append, GRUB
 * `linux` line); call after boot_read_cmdline().  Before that call the
 * default LOG_INFO stands, so early boot always prints. */
void klog_init(void);

void klog_setlevel(int lvl); /* clamped into 0..LOG_DEBUG */
int klog_getlevel(void);
int klog_shown(int lvl);     /* would `lvl` reach the console right now? */

/* Level-tagged printf: always recorded, printed only when klog_shown(lvl). */
void klogf(int lvl, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* Ring queries.  Records are numbered by append order (seq); once
 * KLOG_SLOTS wrapped, the oldest sequence numbers answer "gone" (-1). */
u32 klog_count(void); /* records appended so far == the next seq */
int klog_fetch(u32 seq, int *lvl, char *buf, size_t n); /* 0 ok, -1 gone */

/* Print up to `max` newest records -- panic()'s post-mortem tail.
 * Deliberately lock-free and unrecorded: panic may run while print_lock
 * is held (T-002) and must still reach its halt, and the tail must not
 * feed itself back into the ring it is reading. */
void klog_dump(int max);

/* print.c hands each formatted message here; klog.c owns the ring. */
void klog_record(int lvl, const char *msg);

#ifdef __cplusplus
}
#endif
