/* T-030: process privilege tiers.  Every task carries a struct cred:
 *
 *   CRED_USER  a plain process; no privileges of its own
 *   CRED_ROOT  administrative user (init starts here, fork inherits it);
 *              can do anything a user can, but still cannot damage kxld
 *              state or the kxld system files the kernel needs to boot
 *   CRED_KXLD  the highest tier: protects the kernel and those system
 *              files from everyone below, root included.  Nothing can
 *              climb into it, and a kxld task never enters the syscall
 *              path (syscall_entry BUG_ONs on that).
 *
 * Ordering is by power: USER < ROOT < KXLD, so `kind >= CRED_ROOT`
 * reads as "at least root".  The values are the ABI's (ps prints them
 * straight through), so kernel and user space cannot drift apart. */
#pragma once
#include <types.h>

enum cred_kind {
    CRED_USER = LNXRM_CRED_USER,
    CRED_ROOT = LNXRM_CRED_ROOT,
    CRED_KXLD = LNXRM_CRED_KXLD,
};

/* uid 0 = privileged (root and kxld alike); a plain user is 1000. */
#define CRED_UID_PRIV 0u
#define CRED_UID_USER 1000u

/* Capability bits (T-031), checked as a mask against one CAP_*.
 * root and kxld own every capability by construction (`kind >=
 * CRED_ROOT` -- the enum ascends with power, so this really is "at
 * least root"); a plain user holds only the bits in cred->caps, and
 * dropping a root task to the user tier (sys_setuid) clears them all. */
#define CAP_KILL       (1ULL << 0) /* signal tasks outside one's own uid */
#define CAP_SYS_ADMIN  (1ULL << 1) /* ps / diskinfo: enumerate tasks, disks */
#define CAP_FB         (1ULL << 2) /* draw on the framebuffer */

struct cred {
    enum cred_kind kind;
    u32 uid;
    u64 caps; /* CAP_* mask; 0 for a default user */
};

/* Does `c` hold the capability mask `cap`? */
static inline bool cred_capable(const struct cred *c, u64 cap)
{
    if (!c) return false;
    if (c->kind >= CRED_ROOT) return true; /* root and kxld: all caps */
    return (c->caps & cap) != 0;
}

/* T-032: how many user pages may `c` have mapped at once?  0 = no ceiling.
 *
 * The tier answers, not a number stored beside it: sys_setuid() changes
 * `kind` in place, so a per-task copy of the quota could be made to
 * disagree with the tier it was derived from -- two pieces of bookkeeping
 * for one fact is exactly what made the block cache's victim choice wrong
 * (C24).  root and kxld already hold every capability, so a ceiling they
 * could lift would only add a second thing to keep straight.
 *
 * The ceiling exists because this kernel has no reclaim and no OOM
 * killer: a process that maps until the buddy runs dry leaves every later
 * allocation -- page tables, task stacks, file buffers -- failing forever.
 * 64 MiB is a quarter of the machine and an absurd amount for the
 * programs that live here; root is exempt because it is the tier that
 * could fix the problem it caused (and the tier tests run as). */
#define CRED_USER_MEM_QUOTA_PAGES ((64ULL << 20) / PAGE_SIZE)

static inline u64 cred_mem_quota(const struct cred *c)
{
    if (c && c->kind >= CRED_ROOT) return 0; /* root / kxld: unlimited */
    return CRED_USER_MEM_QUOTA_PAGES;        /* user -- or no cred at all */
}
