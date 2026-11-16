/* /bin/reboot -- reset the machine.
 * Asks the kernel for reboot(48): it flushes the disk cache and pulses
 * the reset lines (CAP_SYS_ADMIN gate -- lsh runs as root, so the shell
 * gets through).  The kernel's success path never returns, so anything
 * printed below means the call was REFUSED, not that a reset failed. */
#include "ulib.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    long r = kreboot();
    if (r == LNXRM_EACCES)
        xputs("reboot: permission denied\n");
    else {
        xputs("reboot: failed (");
        xprinti(r);
        xputs(")\n");
    }
    return 1;
}
