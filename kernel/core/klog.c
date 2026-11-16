/* Crash ring buffer + console log level (PP2, GAP section 5.3).
 *
 * The ring is fixed-size BSS: 96 records of 160 bytes, enough to carry
 * several boot phases, and it survives everything short of a reset --
 * which is the point, since panic() prints its tail before `hlt` and
 * nothing can be read back from a halted machine afterwards.  Each record
 * is one formatted kprintf()/klogf() message (longer messages truncate at
 * KLOG_MSG_MAX; the console still gets the full text).  Raw console
 * traffic (shell output through console_write) is not a kernel message
 * and is not recorded.
 *
 * LOCKING: klog_lock guards only the ring and is never held while taking
 * another lock, so the print_lock -> klog_lock order on the kprintf path
 * cannot cycle.  On the panic path the lock is taken with trylock only:
 * a lost record or a skipped tail beats a spin that never reaches the
 * halt (the console output itself does not depend on the ring). */
#include <sys/klog.h>
#include <console.h> /* vsnprintf, console_putc, in_panic, strncmp */
#include <sys/spinlock.h>
#include <boot.h> /* bootinfo.cmdline */

#define KLOG_MSG_MAX 160 /* per-record text cap, incl. the NUL */
#define KLOG_SLOTS 96    /* ~15 KiB of .bss: the last 96 messages survive */

struct klog_rec {
    u32 seq; /* append order; a slot is valid only when seq matches */
    u8 lvl;
    char msg[KLOG_MSG_MAX];
};

static struct klog_rec klog_ring[KLOG_SLOTS];
static spinlock_t klog_lock = SPINLOCK_INIT;
static volatile u32 klog_total; /* records appended == the next seq */
static volatile int klog_level = LOG_INFO;

/* Take the ring lock.  While panicking never block: the caller is on its
 * way to `hlt` with interrupts off, and any wait there is forever. */
static bool klog_lock_now(u64 *flags)
{
    if (in_panic()) return spin_trylock_irqsave(&klog_lock, flags);
    spin_lock_irqsave(&klog_lock, flags);
    return true;
}

void klog_init(void)
{
    const char *s = bootinfo.cmdline;
    while (*s) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (!strncmp(s, "loglevel=", 9)) {
            /* one digit, clamped; a non-digit keeps the default */
            if (s[9] >= '0' && s[9] <= '9') klog_setlevel(s[9] - '0');
            return;
        }
        while (*s && *s != ' ') s++;
    }
}

void klog_setlevel(int lvl)
{
    if (lvl < LOG_ERR) lvl = LOG_ERR;
    if (lvl > LOG_DEBUG) lvl = LOG_DEBUG;
    klog_level = lvl;
}

int klog_getlevel(void) { return klog_level; }

int klog_shown(int lvl) { return lvl <= klog_level; }

u32 klog_count(void) { return klog_total; }

void klog_record(int lvl, const char *msg)
{
    u64 flags;
    if (!klog_lock_now(&flags)) return;

    struct klog_rec *r = &klog_ring[klog_total % KLOG_SLOTS];
    r->seq = klog_total;
    r->lvl = (u8)lvl;
    int i = 0;
    for (; i < KLOG_MSG_MAX - 1 && msg[i]; i++) r->msg[i] = msg[i];
    r->msg[i] = 0;
    klog_total++;

    spin_unlock_irqrestore(&klog_lock, flags);
}

int klog_fetch(u32 seq, int *lvl, char *buf, size_t n)
{
    u64 flags;
    if (!klog_lock_now(&flags)) return -1;

    struct klog_rec *r = &klog_ring[seq % KLOG_SLOTS];
    int rc = (seq < klog_total && r->seq == seq) ? 0 : -1;
    if (rc == 0) {
        if (lvl) *lvl = r->lvl;
        if (buf && n) {
            int i = 0;
            for (; i < (int)n - 1 && r->msg[i]; i++) buf[i] = r->msg[i];
            buf[i] = 0;
        }
    }

    spin_unlock_irqrestore(&klog_lock, flags);
    return rc;
}

static char lvl_char(int lvl)
{
    static const char names[] = {'E', 'W', 'I', 'D'};
    if (lvl < LOG_ERR || lvl > LOG_DEBUG) return '?';
    return names[lvl];
}

/* vsnprintf needs a va_list, so the tail's one-off formatting goes through
 * a variadic wrapper (the shared format buffer lives in print.c behind
 * print_lock; this is a stack buffer on purpose -- klog_dump runs from
 * panic() where print_lock may already be held). */
__attribute__((format(printf, 3, 4)))
static void klog_line(char *buf, size_t n, const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(buf, n, fmt, ap);
    __builtin_va_end(ap);
}

void klog_dump(int max)
{
    /* Snapshot: records appended while we print belong to whoever comes
     * next, not to this tail. */
    u32 total = klog_total;
    u32 first = (total > (u32)max) ? total - (u32)max : 0;

    for (u32 s = first; s < total; s++) {
        char buf[KLOG_MSG_MAX];
        int lvl;
        if (klog_fetch(s, &lvl, buf, sizeof buf) != 0) continue; /* evicted */

        char line[KLOG_MSG_MAX + 24];
        klog_line(line, sizeof line, "[klog] #%u %c %s", s, lvl_char(lvl), buf);
        /* Raw emit (no print_lock, no klog_record): the tail must not
         * feed itself back into the ring, and panic may hold print_lock. */
        for (char *p = line; *p; p++) console_putc(*p);
        size_t len = strlen(line);
        if (len == 0 || line[len - 1] != '\n') console_putc('\n');
    }
}
