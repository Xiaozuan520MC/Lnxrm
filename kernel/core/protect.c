/* kxld protection layer -- see include/sys/protect.h. */
#include <sys/protect.h>
#include <sys/sched.h>
#include <sys/cred.h>

bool kxld_protect_kill(const struct task *tgt)
{
    if (!tgt) return true;
    /* pid 1: init anchors the system -- ancestor of every user task and
     * reaper of orphans.  Killing it would strand the whole user space,
     * so even root is refused. */
    if (tgt->pid == 1) return true;
    /* The per-CPU idles (kxld tier) guard kernel integrity itself. */
    if (tgt->cred.kind == CRED_KXLD) return true;
    return false;
}

long cap_check(const struct cred *c, u64 cap)
{
    return cred_capable(c, cap) ? 0 : LNXRM_EACCES;
}

long cred_setuid(struct cred *c, u32 uid)
{
    if (!c) return LNXRM_EACCES;
    if (c->uid == uid) return 0; /* asking for what you already are */

    /* Only root may re-aim a cred, and naming anything but uid 0 takes
     * the tier down with it: caps are not a field a user task could
     * hand back to itself, they follow `kind`, and `kind` for a
     * non-zero uid is CRED_USER by definition. */
    if (c->kind < CRED_ROOT) return LNXRM_EACCES;

    c->uid = uid;
    c->kind = (uid == CRED_UID_PRIV) ? CRED_ROOT : CRED_USER;
    c->caps = 0; /* root re-derives every cap from kind; a user holds none */
    return 0;
}
