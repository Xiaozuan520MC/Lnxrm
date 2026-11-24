// SPDX-License-Identifier: Apache-2.0

/* ps2-keyboard.km -- the ring3 half of the S1c keyboard channel.
 *
 * WHAT THIS FILE IS
 *   An ordinary static ELF64 that lives in /mod instead of /bin.  init
 *   discovers it by extension and spawns it as root; nothing in the kernel
 *   links against it, knows it exists, or waits for it.  That is the whole
 *   S1b shape: a driver you add by adding a FILE to the image.
 *
 * WHAT IT DOES (S1c: it is the keyboard's decoder)
 *   Opens the kernel's "kbd" channel, blocks in dipc_recv, and runs every
 *   byte through lnxrm_kbd_decode() -- the SAME function and the SAME two
 *   tables the IRQ1 path compiles, because both sides include
 *   include/abi/ps2-keyboard.h.  One source, two address spaces.  While this
 *   process holds the channel open, IRQ1 MIRRORS raw bytes instead of
 *   decoding them (dipc_push_if_listening), and every character decoded
 *   here travels back with dipc_send(55) as a [char, mods] pair: the
 *   kernel's sink (drivers/ps2-keyboard.c) drops it into the console inbuf and
 *   the unified queue exactly where its own decode would.  Exiting closes
 *   the descriptor, which hands the decode back to the kernel -- a dead
 *   module costs the takeover, never the keyboard.
 *
 * WHAT IT DELIBERATELY DOES NOT PRINT
 *   Anything on the happy path.  A driver that loaded and is doing its job
 *   has nothing to say: /bin/lsh already echoes every character it reads
 *   (lsh.c, putc_(c) in the line editor), so a second echo would put each
 *   keystroke on the console twice -- and a "ring live" banner would land
 *   in the middle of whatever the shell is printing.  The only line this
 *   file ever emits is the one for failing to open the channel (2026-11-18;
 *   the startup banner and the first-byte report were removed the same day
 *   for exactly that reason).
 */
#include "ulib.h"
#include <abi/ps2-keyboard.h>

#define RING_CHUNK 64 /* one recv's worth; well under RECV_MAX */

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    long fd = kdipc_open(LNXRM_KBD_CHANNEL, 0);
    if (fd < 0) {
        /* No channel means kbd_decode_init() never ran -- the kernel has
         * no keyboard consumer to offer.  Say so and leave; a module that
         * spins on a descriptor it does not have is worse than one that
         * is not there. */
        xputs("ps2-keyboard.km: no dipc channel \"" LNXRM_KBD_CHANNEL "\"\n");
        return 1;
    }

    /* Our OWN state, never the kernel's: a break code seen here must not
     * clear the shift bit the kernel's injector is holding.  Initialised
     * field by field rather than with {0} so -Wmissing-field-initializers
     * has nothing to complain about. */
    struct lnxrm_kbd_state st;
    st.shift = 0;
    st.caps = 0;
    st.ext = 0;

    char buf[RING_CHUNK];
    char out[RING_CHUNK * 2]; /* one [char, mods] pair per decoded byte */

    for (;;) {
        /* Blocking is the point: the kernel wakes us the moment a byte
         * lands, so a ring3 driver never has to poll -- polling would cost
         * more than the decode this process exists to do. */
        long n = kdipc_recv(fd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (n == LNXRM_EINTR) continue; /* a signal, not a dead channel */
            break;                          /* EBADF: the channel went away */
        }

        /* Decode the whole recv, then hand the results back in ONE send:
         * the sink wants [char, mods] pairs, and RING_CHUNK * 2 bytes is
         * exactly the worst case, so out[] can never overflow. */
        long on = 0;
        for (long i = 0; i < n; i++) {
            char c = lnxrm_kbd_decode(&st, (unsigned char)buf[i]);
            if (!c) continue; /* break code, bare modifier, E0 key */
            out[on++] = c;
            /* Read the modifiers AFTER the decode: the case flip changes
             * the character, never the state that produced it -- the same
             * rule drivers/ps2-keyboard.c follows on its side. */
            out[on++] = (char)((st.shift ? INPUT_MOD_SHIFT : 0) |
                               (st.caps ? INPUT_MOD_CAPS : 0));
        }
        if (on == 0) continue; /* modifiers and break codes only */

        /* A send that fails means the channel is gone (EBADF) or the
         * kernel has no sink for it (EINVAL).  Either way this module is
         * not a decoder any more: exit, close the descriptor, and the
         * kernel's IRQ1 stub takes the decode back on its next byte. */
        if (kdipc_send((int)fd, out, (size_t)on, 0) < 0) break;
    }

    kclose((int)fd);
    return 0;
}
