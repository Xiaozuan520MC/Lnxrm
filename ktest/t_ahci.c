/* C49: an interrupt that is routed, unmasked and armed still has to *arrive*.
 *
 * The route prints a line at boot; this case makes the drive do work and
 * counts the interrupts the completion handler retired.  A machine with no
 * AHCI (IDE-only, `MACHINE=pc`) or one whose INTx line could not be routed
 * skips instead of failing: neither is a statement about the mechanism. */
#include <sys/ktest.h>
#include <pci.h>

static void test_a_command_is_retired_by_its_interrupt(void)
{
    int r = ahci_irq_selftest();

    if (r == -2) {
        ktest_skip("no AHCI interrupt line routed on this machine");
        return;
    }
    if (r == -1) {
        ktest_skip("no AHCI disk to command");
        return;
    }
    /* 0 == the command completed with the port armed and no ISR run */
    K_EXPECT(r > 0);
}

KTEST("ahci", KTEST_LATE, test_a_command_is_retired_by_its_interrupt);
