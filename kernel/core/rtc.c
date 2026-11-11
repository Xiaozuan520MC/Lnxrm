/* CMOS real-time clock: the one wall-clock source this machine has.
 *
 * Three layers, deliberately in this order:
 *
 *   1. rtc_utc_now()     raw register reads -- port I/O, a wait for the
 *                        RTC's own update to finish, BCD decoding, the
 *                        century byte.  Slow, and it can fail.
 *   2. wall_clock()      samples (1) ONCE, on the first call, and keeps
 *                        the reading together with the TSC at that
 *                        moment.  Every later call is arithmetic on the
 *                        calibrated TSC, so it never touches a port,
 *                        never waits, and cannot step backwards -- two
 *                        properties a clock is useless without.
 *   3. rtc_format()      turns an epoch second back into a calendar line
 *                        for the log, which is how a human confirms at a
 *                        glance that the guest agrees with the host.
 *
 * UTC, with no time zone and no DST: the CMOS is set that way by default
 * on QEMU and on PC hardware, and this kernel has no rule to apply even
 * if it were not.  gettimeofday()'s timezone argument therefore reports
 * zeros, exactly as Linux's does.
 *
 * Why sample once instead of reading the RTC per call: a per-call read
 * would cost a UIP wait (up to ~1 s if a call lands inside the RTC's
 * update window) and a port access from whichever CPU asked, and the RTC
 * only ever moves forward anyway -- it has a one-second resolution, so
 * reading it again can only subtract precision the TSC already gives for
 * free.  The sample and the TSC that anchors it are taken under one
 * one-shot state machine (see wall_clock), because two CPUs racing their
 * first gettimeofday would otherwise publish two different bases. */
#include <types.h>
#include <io.h>
#include <string.h>
#include <sys/rtc.h>
#include <sys/cpu.h> /* rdtsc, tsc_per_ms, jiffies, HZ */

#define CMOS_INDEX 0x70
#define CMOS_DATA  0x71

/* Register map (MC146818 layout, what QEMU and every PC implement). */
#define RTC_REG_A 0x0A
#define RTC_REG_B 0x0B
#define RTC_SEC   0x00
#define RTC_MIN   0x02
#define RTC_HOUR  0x04
#define RTC_DAY   0x07
#define RTC_MONTH 0x08
#define RTC_YEAR  0x09
#define RTC_CENT  0x32 /* century, when the CMOS provides one */

/* Bit fields. */
#define RTC_UIP (1u << 7) /* reg A: an update is in progress */
#define RTC_DM  (1u << 2) /* reg B: 1 = binary, 0 = BCD */
#define RTC_MIL (1u << 7) /* reg B: 1 = 24-hour, 0 = 12-hour + AM/PM */

/* How long rtc_utc_now() waits for UIP to clear before giving up.  Each
 * iteration is one port read (microseconds), so this budget is tens of
 * milliseconds -- an eternity for a flag QEMU normally keeps clear, and
 * short enough that broken hardware answers `ok = false` instead of
 * wedging the first gettimeofday forever. */
#define UIP_WAIT_LOOPS 100000

static u8 cmos_read(u8 reg)
{
    outb(CMOS_INDEX, reg); /* bit 7 stays clear: NMI remains enabled */
    return inb(CMOS_DATA);
}

static int bcd2bin(u8 v) { return (v >> 4) * 10 + (v & 0x0F); }

/* Days since 1970-01-01 for a civil date.  This is Howard Hinnant's
 * days_from_civil: era-based rather than trial-divided, so it stays exact
 * on both sides of a leap-year century (2000 was leap, 1900 was not) with
 * no special case for either.  Exposed because the self-test checks it
 * against dates it can derive by counting. */
i64 rtc_days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    i64 era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);              /* [0, 399] */
    unsigned doy = (153 * (unsigned)(m + (m > 2 ? -3 : 9)) + 2) / 5 + (unsigned)d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;  /* [0, 146096] */
    return era * 146097 + (i64)doe - 719468;
}

bool rtc_utc_now(struct rtc_utc *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));

    /* Wait out an in-progress update: reading the registers mid-update
     * can hand back a mixed old/new set (new seconds with the old
     * minutes), which is exactly the once-in-a-blue-moon time a clock
     * reports 06:59:59 as 07:59:00.  The budget turns a stuck UIP flag
     * into `ok = false` rather than an unbounded spin. */
    bool settled = false;
    for (int i = 0; i < UIP_WAIT_LOOPS; i++) {
        if (!(cmos_read(RTC_REG_A) & RTC_UIP)) {
            settled = true;
            break;
        }
    }
    if (!settled) return false;

    u8 b = cmos_read(RTC_REG_B);
    bool binary = (b & RTC_DM) != 0;
    bool h24 = (b & RTC_MIL) != 0;

    u8 s = cmos_read(RTC_SEC);
    u8 mi = cmos_read(RTC_MIN);
    u8 h = cmos_read(RTC_HOUR);
    u8 d = cmos_read(RTC_DAY);
    u8 mo = cmos_read(RTC_MONTH);
    u8 y2 = cmos_read(RTC_YEAR);

    int sec = binary ? s : bcd2bin(s);
    int min = binary ? mi : bcd2bin(mi);
    /* 12-hour mode parks PM in bit 7 of the hour byte (a BCD digit never
     * uses it), and 12 o'clock is 0 in 24-hour terms. */
    bool pm = (h & 0x80) != 0;
    int hour = binary ? (h & 0x7F) : bcd2bin((u8)(h & 0x7F));
    if (!h24) {
        if (hour == 12) hour = 0;
        if (pm) hour += 12;
    }
    int day = binary ? d : bcd2bin(d);
    int mon = binary ? mo : bcd2bin(mo);
    int y = binary ? y2 : bcd2bin(y2);

    /* Century: the dedicated byte when the CMOS has one (QEMU writes the
     * host's century there), otherwise the pre-2000 convention a clock
     * without one still follows -- two-digit years in [70,99] mean the
     * 1900s, everything else the 2000s. */
    int cent = binary ? cmos_read(RTC_CENT) : bcd2bin(cmos_read(RTC_CENT));
    if (cent >= 19 && cent <= 22)
        y += cent * 100;
    else
        y += (y >= 70) ? 1900 : 2000;

    /* Ranges: a reading outside them is mis-decoded hardware, not a date
     * any caller should be handed (and rtc_days_from_civil would happily
     * turn it into a plausible-looking wrong epoch). */
    if (sec > 59 || min > 59 || hour > 23 || day < 1 || day > 31 ||
        mon < 1 || mon > 12 || y < 1980 || y > 2099)
        return false;

    out->year = y;
    out->mon = mon;
    out->day = day;
    out->hour = hour;
    out->min = min;
    out->sec = sec;
    out->ok = true;
    return true;
}

/* ---- the sampled clock ---------------------------------------------- */

/* 0 = never sampled, 1 = a CPU is sampling right now, 2 = a base exists,
 * 3 = the RTC has never answered and never will. */
static volatile u32 clock_state;
static i64 base_epoch_usec;
static u64 base_tsc;
static u64 base_jiffies;
static i64 last_epoch_usec; /* monotonic floor, so no answer ever repeats
                             * an earlier one going backwards */

/* Publish `v` as the new floor if it is larger.  Two CPUs calling
 * gettimeofday at once must not both read a stale floor and each report
 * itself the latest: the CAS loop makes the max win exactly once. */
static void floor_raise(i64 v)
{
    i64 cur = __atomic_load_n(&last_epoch_usec, __ATOMIC_RELAXED);
    while (v > cur) {
        if (__atomic_compare_exchange_n(&last_epoch_usec, &cur, v, false,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            return;
        /* `cur` was reloaded by the failed compare; try again with it. */
    }
}

int wall_clock(i64 *sec, i64 *usec)
{
    u32 st = __atomic_load_n(&clock_state, __ATOMIC_ACQUIRE);

    if (st == 0) {
        /* One CPU wins this and samples; the others spin until it is done.
         * A spin-then-recheck is enough: the loser's next load sees 2 or 3
         * and never enters here again (the state never goes back to 0). */
        u32 expect = 0;
        if (__atomic_compare_exchange_n(&clock_state, &expect, 1, false,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            struct rtc_utc r;
            if (rtc_utc_now(&r)) {
                i64 e = rtc_days_from_civil(r.year, r.mon, r.day) * 86400 +
                        r.hour * 3600 + r.min * 60 + r.sec;
                base_epoch_usec = e * 1000000;
                base_tsc = rdtsc();
                base_jiffies = jiffies;
                last_epoch_usec = base_epoch_usec;
                __atomic_store_n(&clock_state, 2, __ATOMIC_RELEASE);
            } else {
                __atomic_store_n(&clock_state, 3, __ATOMIC_RELEASE);
            }
        } else {
            while ((st = __atomic_load_n(&clock_state, __ATOMIC_ACQUIRE)) == 1)
                __asm__ volatile("pause");
        }
        st = __atomic_load_n(&clock_state, __ATOMIC_ACQUIRE);
    }
    if (st != 2) return -LNXRM_EIO; /* no usable RTC reading, ever */

    /* Advance the sample.  TSC first (microsecond-class resolution); the
     * calibrated rate is 0 only before lapic_timer_calibrate() has run, in
     * which case jiffies -- the same clock nanosleep waits on -- carries
     * the seconds at HZ resolution until the TSC is known. */
    i64 delta_usec;
    u64 per_ms = tsc_per_ms();
    if (per_ms) {
        u64 d = rdtsc() - base_tsc;
        delta_usec = (i64)(d / per_ms) * 1000 + (i64)((d % per_ms) * 1000 / per_ms);
    } else {
        delta_usec = (i64)(jiffies - base_jiffies) * (1000000 / HZ);
    }

    i64 now = base_epoch_usec + delta_usec;
    floor_raise(now); /* now >= the floor, always */
    now = __atomic_load_n(&last_epoch_usec, __ATOMIC_RELAXED);

    if (sec) *sec = now / 1000000;
    if (usec) {
        i64 u = now % 1000000;
        if (u < 0) u += 1000000; /* negative epochs would borrow; we have none */
        *usec = u;
    }
    return 0;
}

/* civil_from_days: the inverse of rtc_days_from_civil (same era form). */
void rtc_format(i64 epoch_sec, char *buf, size_t bufsz)
{
    if (!buf || bufsz == 0) return;
    if (epoch_sec < 0) epoch_sec = 0; /* no dates before 1970 here */

    i64 z = epoch_sec / 86400 + 719468;
    i64 era = (z >= 0 ? z : z - 146096) / 146097;
    u64 doe = (u64)(z - era * 146097);                        /* [0, 146096] */
    u64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    i64 y = (i64)yoe + era * 400;
    u64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);        /* [0, 365] */
    u64 mp = (5 * doy + 2) / 153;                             /* [0, 11] */
    u64 d = doy - (153 * mp + 2) / 5 + 1;                     /* [1, 31] */
    u64 m = mp + (mp < 10 ? 3 : -9);                          /* [1, 12] */
    y += (m <= 2);

    i64 tod = epoch_sec % 86400;
    if (tod < 0) tod += 86400;

    /* Manual formatting: this runs on the log path and on the self-test,
     * where a dependency on kprintf's formatter would buy nothing. */
    char tmp[24];
    int n = 0;
    tmp[n++] = (char)('0' + (int)(y / 1000) % 10);
    tmp[n++] = (char)('0' + (int)(y / 100) % 10);
    tmp[n++] = (char)('0' + (int)(y / 10) % 10);
    tmp[n++] = (char)('0' + (int)y % 10);
    tmp[n++] = '-';
    tmp[n++] = (char)('0' + (int)(m / 10));
    tmp[n++] = (char)('0' + (int)(m % 10));
    tmp[n++] = '-';
    tmp[n++] = (char)('0' + (int)(d / 10));
    tmp[n++] = (char)('0' + (int)(d % 10));
    tmp[n++] = 'T';
    tmp[n++] = (char)('0' + (int)(tod / 3600) / 10);
    tmp[n++] = (char)('0' + (int)(tod / 3600) % 10);
    tmp[n++] = ':';
    tmp[n++] = (char)('0' + (int)(tod / 60 % 60) / 10);
    tmp[n++] = (char)('0' + (int)(tod / 60 % 60) % 10);
    tmp[n++] = ':';
    tmp[n++] = (char)('0' + (int)(tod % 60) / 10);
    tmp[n++] = (char)('0' + (int)(tod % 60) % 10);
    tmp[n++] = 'Z';
    tmp[n] = 0;

    size_t i;
    for (i = 0; tmp[i] && i + 1 < bufsz; i++) buf[i] = tmp[i];
    buf[i] = 0;
}
