// SPDX-License-Identifier: Apache-2.0

/* /bin/help -- list available commands. */
#include "ulib.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    xputs("cat mkdir clear echo help kill ls mv ps lsh touch rm reboot cd pwd\n");
    return 0;
}
