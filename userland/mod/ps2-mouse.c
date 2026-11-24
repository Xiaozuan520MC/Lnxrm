// SPDX-License-Identifier: Apache-2.0

/* ps2-mouse.km -- the ring3 half of the S1b mouse channel.
 *
 * WHAT THIS FILE IS
 *   An ordinary static ELF64 that lives in /mod instead of /bin, exactly
 *   like ps2-keyboard.km: init discovers it by extension and spawns it as
 *   root, and nothing in the kernel links against it, knows it exists or
 *   waits for it.  A driver you add by adding a FILE to the image.
 *
 * WHAT IT DOES (S1c: it is the mouse's reassembler)
 *   Opens the kernel's "mouse" channel, blocks in dipc_recv, and runs
 *   every byte through lnxrm_mouse_decode() -- the SAME reassembly state
 *   machine and the SAME sign conventions the IRQ12 path compiles,
 *   because both sides include include/abi/ps2-mouse.h.  One source, two
 *   address spaces.  While this process holds the channel open, IRQ12
 *   MIRRORS raw bytes instead of feeding them to ms_feed_byte()
 *   (dipc_push_if_listening), and every packet finished here travels back
 *   with dipc_send(55): the kernel's sink (drivers/ps2-mouse.c) lands it
 *   in mouse_get 37's ring and the unified queue39-41 exactly where its
 *   own reassembly would, counters included.  Exiting closes the
 *   descriptor, which hands the reassembly back to the kernel -- a dead
 *   module costs the takeover, never the mouse.
 *
 * WHAT IT DELIBERATELY DOES NOT PRINT
 *   Anything on the happy path.  There is no cursor in this kernel (the
 *   console is text/fb, the kernel keeps no on-screen pointer) and lsh has
 *   nothing to point at, so a ring3 mouse driver that reacted to every
 *   packet would be inventing a UI nobody asked for -- and one that
 *   ANNOUNCED itself would drop a line into the middle of whatever the
 *   shell is printing every time QEMU pushed a pointer event.  The only
 *   line this file emits is the one for failing to open the channel
 *   (2026-11-18; the startup banner and the first-packet report were
 *   removed the same day).
 */
#include "ulib.h"
#include <abi/ps2-mouse.h>

#define RING_CHUNK 64 /* one recv's worth; well under RECV_MAX */

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    long fd = kdipc_open(LNXRM_MOUSE_CHANNEL, 0);
    if (fd < 0) {
        /* No channel means mouse_decode_init() never ran -- the kernel
         * has no aux consumer to offer.  Say so and leave; a module that
         * spins on a descriptor it does not have is worse than one that
         * is not there. */
        xputs("ps2-mouse.km: no dipc channel \"" LNXRM_MOUSE_CHANNEL "\"\n");
        return 1;
    }

    /* Our OWN decoder, never the kernel's: a packet half-assembled here
     * must not be finished by ms_feed_byte() over there.  Initialised
     * field by field rather than left to a {0} so
     * -Wmissing-field-initializers has nothing to complain about. */
    struct lnxrm_mouse_dec dec;
    dec.pkt[0] = 0;
    dec.pkt[1] = 0;
    dec.pkt[2] = 0;
    dec.pkt[3] = 0;
    dec.n = 0;
    dec.bytes = LNXRM_MOUSE_PKT_MIN;

    char buf[RING_CHUNK];
    int at_boundary = 0; /* the previous byte completed a packet */
    int evidence = 0;    /* boundary resyncs seen, for framing */

    for (;;) {
        /* Blocking is the point: the kernel wakes us the moment a byte
         * lands, so a ring3 driver never has to poll. */
        long n = kdipc_recv(fd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (n == LNXRM_EINTR) continue; /* a signal, not a dead channel */
            break;                          /* EBADF: the channel went away */
        }

        for (long i = 0; i < n; i++) {
            struct lnxrm_mouse_packet p;
            int rc = lnxrm_mouse_decode(&dec, (unsigned char)buf[i], &p);
            if (rc > 0) {
                /* A whole packet: the stream is at a boundary again, and
                 * the packet now has somewhere to go -- dipc_send(55)
                 * hands it to the kernel's sink, which lands it in
                 * mouse_get 37's ring and the unified queue exactly where
                 * its own reassembly would (S1c).  A send that fails means
                 * the channel is gone (EBADF) or the kernel has no sink
                 * (EINVAL): exit, close the descriptor, and IRQ12 hands
                 * the reassembly back to the kernel on its next byte. */
                at_boundary = 1;
                if (kdipc_send((int)fd, &p, sizeof p, 0) < 0) {
                    kclose((int)fd);
                    return 1;
                }
            } else if (rc == 0) {
                /* dec.n == 1 means a flags byte took hold, so the stream
                 * is framed again and the boundary is behind us. */
                if (dec.n == 1) at_boundary = 0;
            } else if (at_boundary) {
                /* A byte arrived where a flags byte belongs and lacked
                 * the 0x08 sync bit -- and we are sitting on a packet
                 * boundary.  Framing has to be worked out here because
                 * ring3 is not told: mouse_info 38 has no field for the
                 * negotiated packet length and mouse_get 37 is
                 * GATE_ADMIN.  Read as 3 bytes, a 4-byte (wheel) stream
                 * lands on the wheel byte at exactly this spot, and a
                 * wheel value of 0..7 fails the sync check while a real
                 * flags byte never can.  A correct 3-byte stream has the
                 * opposite property -- at a boundary the next byte IS the
                 * flags byte -- so it produces no evidence at all; two
                 * pieces are required because one corrupt flags byte
                 * must not latch this reader onto the wrong framing for
                 * good (the resyncs that follow it are not counted here:
                 * the boundary flag is consumed by the first one). */
                at_boundary = 0;
                if (dec.bytes == LNXRM_MOUSE_PKT_MIN && ++evidence >= 2)
                    dec.bytes = LNXRM_MOUSE_PKT_WHEEL;
            }
        }
    }

    kclose((int)fd);
    return 0;
}
