// SPDX-License-Identifier: Apache-2.0

/* /bin/halt -- stop the CPU, do not reset or power off.
 * shutdown(LNXRM_RB_HALT) runs cli; hlt forever.  Useful when the machine
 * has no ACPI power-off path and you just want it to stop cleanly.
 * CAP_SYS_ADMIN gated; success never returns. */
#include "ulib.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    long r = kshutdown(LNXRM_RB_HALT);
    if (r == LNXRM_EACCES)
        xputs("halt: permission denied\n");
    else if (r == LNXRM_EINVAL)
        xputs("halt: invalid command\n");
    else {
        xputs("halt: failed (");
        xprinti(r);
        xputs(")\n");
    }
    return 1;
}
