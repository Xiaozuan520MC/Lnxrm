/* The T-031 capability gates, proved against synthetic creds.
 *
 * On a live boot the interesting half of these gates never runs: every
 * task starts as root (kernel_spawn, then fork inheritance), so
 * cred_capable() answers "yes" from `kind >= CRED_ROOT` and the refusal
 * path would stay untested until something drops a cred.  These cases
 * build the creds by hand and assert both halves -- allow and refuse --
 * on every boot, before user space exists to be one of them.
 *
 * Pure functions on a struct cred, no allocator: KTEST_EARLY. */
#include <sys/ktest.h>
#include <sys/cred.h>
#include <sys/protect.h>
#include <sys/sched.h> /* struct task, for the kxld kill rules */
#include <console.h>   /* memset */

static struct cred mk_user(u64 caps)
{
    struct cred c;
    c.kind = CRED_USER;
    c.uid = CRED_UID_USER;
    c.caps = caps;
    return c;
}

static struct cred mk_root(void)
{
    struct cred c;
    c.kind = CRED_ROOT;
    c.uid = CRED_UID_PRIV;
    c.caps = ~0ULL; /* root's mask is ignored: kind answers for it */
    return c;
}

static void test_user_without_caps_is_refused_everywhere(void)
{
    struct cred u = mk_user(0);
    K_ASSERT_EQ(cap_check(&u, CAP_KILL), LNXRM_EACCES);
    K_ASSERT_EQ(cap_check(&u, CAP_SYS_ADMIN), LNXRM_EACCES);
    K_ASSERT_EQ(cap_check(&u, CAP_FB), LNXRM_EACCES);
    /* no cred at all is never a privilege */
    K_ASSERT_EQ(cap_check(NULL, CAP_FB), LNXRM_EACCES);
}

static void test_a_granted_bit_grants_only_that_bit(void)
{
    struct cred u = mk_user(CAP_FB);
    K_ASSERT_EQ(cap_check(&u, CAP_FB), 0);
    K_ASSERT_EQ(cap_check(&u, CAP_SYS_ADMIN), LNXRM_EACCES);
    K_ASSERT_EQ(cap_check(&u, CAP_KILL), LNXRM_EACCES);
}

static void test_root_and_kxld_hold_every_cap(void)
{
    struct cred r = mk_root();
    struct cred k = mk_root();
    k.kind = CRED_KXLD;
    K_ASSERT_EQ(cap_check(&r, CAP_KILL), 0);
    K_ASSERT_EQ(cap_check(&r, CAP_SYS_ADMIN), 0);
    K_ASSERT_EQ(cap_check(&r, CAP_FB), 0);
    K_ASSERT_EQ(cap_check(&k, CAP_KILL), 0);
    K_ASSERT_EQ(cap_check(&k, CAP_SYS_ADMIN), 0);
    K_ASSERT_EQ(cap_check(&k, CAP_FB), 0);
}

static void test_setuid_drop_takes_the_tier_and_the_caps(void)
{
    struct cred r = mk_root();
    K_ASSERT_EQ(cred_setuid(&r, CRED_UID_USER), 0);
    K_ASSERT_EQ((u64)r.kind, (u64)CRED_USER);
    K_ASSERT_EQ(r.uid, CRED_UID_USER);
    K_ASSERT_EQ(r.caps, 0); /* the mask goes with the tier, not after it */

    /* ... and everything the task used to do is now refused */
    K_ASSERT_EQ(cap_check(&r, CAP_SYS_ADMIN), LNXRM_EACCES);
    K_ASSERT_EQ(cap_check(&r, CAP_FB), LNXRM_EACCES);
    K_ASSERT_EQ(cap_check(&r, CAP_KILL), LNXRM_EACCES);
}

static void test_setuid_drop_is_one_way(void)
{
    struct cred r = mk_root();
    K_ASSERT_EQ(cred_setuid(&r, CRED_UID_USER), 0);

    /* naming uid 0 must not hand root back */
    K_ASSERT_EQ(cred_setuid(&r, CRED_UID_PRIV), LNXRM_EACCES);
    K_ASSERT_EQ(r.uid, CRED_UID_USER); /* and the failed call changed nothing */
    K_ASSERT_EQ((u64)r.kind, (u64)CRED_USER);

    /* neither may a user walk sideways into someone else's uid */
    K_ASSERT_EQ(cred_setuid(&r, 1001), LNXRM_EACCES);
    K_ASSERT_EQ(r.uid, CRED_UID_USER);

    /* asking for the uid you already hold is always fine (and is the
     * no-op every task would otherwise need privileges for) */
    K_ASSERT_EQ(cred_setuid(&r, CRED_UID_USER), 0);
}

static void test_root_may_set_any_uid(void)
{
    struct cred r = mk_root();
    K_ASSERT_EQ(cred_setuid(&r, 0), 0); /* already root: no-op */
    K_ASSERT_EQ((u64)r.kind, (u64)CRED_ROOT);

    K_ASSERT_EQ(cred_setuid(&r, 42), 0);
    K_ASSERT_EQ(r.uid, 42);
    K_ASSERT_EQ((u64)r.kind, (u64)CRED_USER); /* non-zero uid = plain user */
    K_ASSERT_EQ(cred_setuid(&r, CRED_UID_PRIV), LNXRM_EACCES); /* one way */
    K_ASSERT_EQ(cred_setuid(NULL, 0), LNXRM_EACCES);           /* no cred */
}

static void test_kxld_protect_kill_rules(void)
{
    struct task t;

    K_ASSERT(kxld_protect_kill(NULL)); /* no target = no permission */

    memset(&t, 0, sizeof(t));
    t.pid = 1; /* init: untouchable even for root */
    t.cred.kind = CRED_ROOT;
    K_ASSERT(kxld_protect_kill(&t));

    t.pid = 7; /* a kxld task: untouchable for the same reason */
    t.cred.kind = CRED_KXLD;
    K_ASSERT(kxld_protect_kill(&t));

    t.cred.kind = CRED_ROOT; /* an ordinary root task: the regular gate */
    K_ASSERT(!kxld_protect_kill(&t));
    t.cred.kind = CRED_USER;
    K_ASSERT(!kxld_protect_kill(&t));
}

/* T-032: the brk ceiling comes from the tier, and from nowhere else.
 *
 * sys_brk() calls cred_mem_quota() on every growth attempt, so this is
 * the whole of "who may map how much": a plain user is capped at 64 MiB
 * of address space (image + stack + heap counted together), root and kxld
 * are not, and a missing cred gets the cap rather than the exemption.
 * Holding the number here also pins the policy: changing the 64 MiB
 * means changing this expectation.  The last assertion is the C24 point --
 * setuid() moves the tier in place, so the ceiling follows it, and there
 * is no second piece of bookkeeping left to disagree with it. */
static void test_mem_quota_follows_the_tier(void)
{
    struct cred u = mk_user(0), r = mk_root(), k = mk_root();
    k.kind = CRED_KXLD;

    K_EXPECT_EQ(CRED_USER_MEM_QUOTA_PAGES, (u64)((64ULL << 20) / PAGE_SIZE));
    K_ASSERT_EQ(cred_mem_quota(&u), CRED_USER_MEM_QUOTA_PAGES);
    K_EXPECT_EQ(cred_mem_quota(&r), (u64)0); /* root: no ceiling */
    K_EXPECT_EQ(cred_mem_quota(&k), (u64)0); /* kxld: no ceiling */
    K_EXPECT_EQ(cred_mem_quota(NULL), CRED_USER_MEM_QUOTA_PAGES); /* fail closed */

    K_ASSERT_EQ(cred_setuid(&r, CRED_UID_USER), 0);
    K_ASSERT_EQ((u64)r.kind, (u64)CRED_USER);
    K_EXPECT_EQ(cred_mem_quota(&r), CRED_USER_MEM_QUOTA_PAGES);
}

KTEST("cred", KTEST_EARLY, test_user_without_caps_is_refused_everywhere);
KTEST("cred", KTEST_EARLY, test_a_granted_bit_grants_only_that_bit);
KTEST("cred", KTEST_EARLY, test_root_and_kxld_hold_every_cap);
KTEST("cred", KTEST_EARLY, test_setuid_drop_takes_the_tier_and_the_caps);
KTEST("cred", KTEST_EARLY, test_setuid_drop_is_one_way);
KTEST("cred", KTEST_EARLY, test_root_may_set_any_uid);
KTEST("cred", KTEST_EARLY, test_kxld_protect_kill_rules);
KTEST("cred", KTEST_EARLY, test_mem_quota_follows_the_tier);
