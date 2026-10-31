/* /bin/help -- list available commands. */
#include "ulib.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    xputs("cat mkdir clear echo help kill ls mv ps rn sh touch rm\n");
    return 0;
}
