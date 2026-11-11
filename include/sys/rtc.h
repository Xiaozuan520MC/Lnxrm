#pragma once
#include <types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The wall clock behind gettimeofday(46): CMOS RTC reading first, TSC
 * advancement after that.  Everything here reports UTC -- this kernel has
 * no time zone database and no DST rule, and the CMOS on QEMU and on real
 * PC hardware is set to UTC by default. */

/* One raw reading of the RTC registers, as calendar fields.  `ok` is false
 * when the registers would not settle (an RTC still mid-update past the
 * wait budget, or hardware that answers the same byte twice with the index
 * between) -- the fields are then meaningless and must not be used. */
struct rtc_utc {
    int year; /* 4-digit, century byte resolved */
    int mon;  /* 1..12 */
    int day;  /* 1..31 */
    int hour; /* 0..23, 12-hour mode already converted */
    int min;
    int sec;
    bool ok;
};

/* Read the RTC right now.  Costs a UIP wait (normally nothing: QEMU keeps
 * the update-in-progress flag clear) and two port accesses per register.
 * Called by wall_clock() exactly once and by the boot self-test; nothing
 * on a hot path ever goes through here. */
bool rtc_utc_now(struct rtc_utc *out);

/* Days since 1970-01-01 for a civil date (proleptic Gregorian, Howard
 * Hinnant's era-correct form -- no trial division by 4/100/400).  Exposed
 * so the self-test can check the conversion against a date it computes
 * the long way round. */
i64 rtc_days_from_civil(int y, int m, int d);

/* Calendar fields for `sec` epoch seconds, in the lnxrm_timeval shape:
 * seconds since 1970-01-01 UTC plus microseconds in [0, 1000000).  First
 * call samples the RTC (and may cost the UIP wait); every call after that
 * is arithmetic on the calibrated TSC, so it can never wait and never go
 * backwards.  Answers 0, or a negative LNXRM error when no RTC reading
 * has ever been usable -- reporting a fabricated 1970 to a program that
 * asked what time it is would be worse than an error. */
int wall_clock(i64 *sec, i64 *usec);

/* "YYYY-MM-DDTHH:MM:SSZ" for an epoch second -- a fixed 20 bytes plus the
 * NUL.  The log line the self-test prints with it is how a human checks at
 * a glance that the guest clock agrees with the host. */
void rtc_format(i64 epoch_sec, char *buf, size_t bufsz);

#ifdef __cplusplus
}
#endif
