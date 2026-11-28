// SPDX-License-Identifier: Apache-2.0

/* /bin/poweroff -- ask the platform to switch off.
 * shutdown(LNXRM_RB_POWER_OFF) writes SLP_TYP=5|SLP_EN to the ACPI PM1a_CNT
 * port from the FADT; QEMU honours it and exits with status 0 instead of
 * resetting.  On real firmware this triggers a soft power-off (ACPI S5).
 * CAP_SYS_ADMIN gated; success never returns. */
#include "ulib.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    long r = kshutdown(LNXRM_RB_POWER_OFF);
    if (r == LNXRM_EACCES)
        xputs("poweroff: permission denied\n");
    else if (r == LNXRM_EINVAL)
        xputs("poweroff: invalid command\n");
    else {
        xputs("poweroff: failed (");
        xprinti(r);
        xputs(")\n");
    }
    return 1;
}
