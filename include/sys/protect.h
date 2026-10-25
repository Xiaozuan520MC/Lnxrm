/* kxld protection layer (T-031): rules that nothing below kxld may
 * break, enforced on the syscall paths that could damage the system.
 *
 * Everything here is a pure function of its arguments -- no `current`,
 * no globals -- because ktest drives these gates with synthetic creds:
 * "a plain user with no caps is refused" has to be provable during
 * boot, before any user process exists to be one. */
#pragma once
#include <types.h>
#include <sys/cred.h>

struct task;

/* true  = the target sits under kxld protection; the operation must be
 *          refused (callers answer EACCES) no matter who asks;
 * false = no kxld rule applies -- the caller's regular gate (same uid
 *         or CAP_KILL) still runs. */
bool kxld_protect_kill(const struct task *tgt);

/* The capability gate behind one system call: 0 when `c` holds `cap`,
 * LNXRM_EACCES otherwise.  Callers print their own denial receipt (which
 * syscall, which pid) the way sys_kill does. */
long cap_check(const struct cred *c, u64 cap);

/* setuid(2) core: retarget `c` at `uid`, returning 0 or LNXRM_EACCES.
 *
 *  - uid already held            -> 0, no change (any task may ask)
 *  - root -> any uid             -> 0, and below uid 0 the tier falls to
 *    CRED_USER with every capability cleared: the drop is one-way, a
 *    user task cannot name uid 0 and get root back
 *  - plain user -> any other uid -> LNXRM_EACCES (no climbing, no lateral
 *    moves into another uid)
 *
 * kxld never reaches the syscall path (syscall_entry BUG_ONs), so it is
 * not a case here. */
long cred_setuid(struct cred *c, u32 uid);
