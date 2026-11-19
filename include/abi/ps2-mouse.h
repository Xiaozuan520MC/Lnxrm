/* The PS/2 aux (mouse) packet decode: the SINGLE source of it, compiled
 * into both halves of the S1b mouse channel.
 *
 *   kernel/drivers/ps2-mouse.c   ring0 -- the IRQ12 path, syscalls 37/38
 *   userland/mod/ps2-mouse.c        ring3 -- the /mod driver module
 *
 * WHY IT IS A HEADER AND NOT A .C
 *   The two halves live in different address spaces and are linked by
 *   different linkers, so "one source" here cannot mean "one object".  It
 *   means one file: a second copy of this reassembly state machine is a
 *   second copy that drifts the first time somebody fixes the sign
 *   convention and forgets which of the two they were editing.  (The
 *   keyboard's tables live the same way -- see abi/ps2-keyboard.h.)
 *
 * WHY THE STATE IS NOT SHARED
 *   Each side keeps its OWN struct lnxrm_mouse_dec, on purpose.  IRQ12
 *   gives each byte to exactly ONE of them -- the module while a
 *   descriptor holds the channel open (S1c), the kernel under ms_lock
 *   otherwise -- so a half-assembled packet in one must never be finished
 *   by the other, and the kernel's copy is the one 37/38 answer with,
 *   which has to keep behaving exactly as it did before the channel
 *   existed.
 *
 * WHY FRAMING IS A FIELD AND NOT A CONSTANT
 *   A packet is 3 bytes on a plain PS/2 mouse and 4 once the IntelliMouse
 *   rate probe switches the device into wheel mode; the kernel learns
 *   which at boot from the 0xF2 identify reply.  The field lives in the
 *   decoder so each side states its own framing: the kernel's is
 *   authoritative (it negotiated it), the ring3 module has to work its
 *   own out -- see userland/mod/ps2-mouse.c. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* The dipc channel IRQ12 pushes its raw bytes into, named here so the
 * producer (kernel/drivers/ps2-mouse.c), the binder (mouse_decode_init)
 * and the consumer (userland/mod/ps2-mouse.c) cannot disagree about it. */
#define LNXRM_MOUSE_CHANNEL "mouse"

/* byte 0 of every packet: {LB, MB, RB, 0x08, Xs, Ys, Xov, Yov} */
#define LNXRM_MOUSE_F_LB    0x01
#define LNXRM_MOUSE_F_MB    0x02
#define LNXRM_MOUSE_F_RB    0x04
#define LNXRM_MOUSE_F_SYNC  0x08 /* always set: without it the stream is desynced */
#define LNXRM_MOUSE_F_XS    0x10 /* dx sign, 9th bit */
#define LNXRM_MOUSE_F_YS    0x20 /* dy sign, 9th bit */
#define LNXRM_MOUSE_F_XOV   0x40 /* dx overflow: force dx to 0, never garbage */
#define LNXRM_MOUSE_F_YOV   0x80
#define LNXRM_MOUSE_F_BTNS  0x07 /* LB|MB|RB, already where buttons want to be */

#define LNXRM_MOUSE_PKT_MIN   3 /* standard packet: flags, dx, dy */
#define LNXRM_MOUSE_PKT_WHEEL 4 /* IntelliMouse: + sign-extended wheel nibble */

/* One decoder's reassembly state.  Plain fields, no bitfields: the kernel
 * mutates it under ms_lock, the module on its own stack, and there is
 * nothing wide enough for either to tear. */
struct lnxrm_mouse_dec {
    unsigned char pkt[4]; /* at most LNXRM_MOUSE_PKT_WHEEL bytes */
    int n;                /* bytes collected so far */
    int bytes;            /* LNXRM_MOUSE_PKT_MIN or LNXRM_MOUSE_PKT_WHEEL */
};

/* One decoded packet, in the shape both syscalls 37 and the unified queue
 * already want: screen-space deltas (y down), buttons as a bitmask. */
struct lnxrm_mouse_packet {
    int dx, dy;
    unsigned buttons;
    int wheel;
};

/* The 4th byte's low nibble is a 4-bit two's-complement wheel delta
 * (-8..7); its upper nibble is the horizontal wheel on exotic devices and
 * is ignored, exactly as it always has been. */
static inline int lnxrm_mouse_wheel(unsigned char b)
{
    int w = b & 0x0F;
    return w >= 8 ? w - 16 : w;
}

/* Decode exactly ONE byte of the stream.
 *
 * Returns  1 -- a whole packet is ready in *out (d->n is back to 0),
 *          0 -- need more bytes (d->n says how many are in hand),
 *         -1 -- discarded: d->n was 0 and the byte lacked the 0x08 sync
 *               bit, so the stream was desynced and the byte is not a
 *               flags byte.  Nothing is consumed and nothing is produced;
 *               the caller decides whether to count it as a resync.
 *
 * The dy sign flip is the PS/2 wire convention (positive y = up) meeting
 * ours (screen y = down); it is the single most drift-prone line in this
 * file, which is the whole reason the file exists. */
static inline int lnxrm_mouse_decode(struct lnxrm_mouse_dec *d, unsigned char b,
                                     struct lnxrm_mouse_packet *out)
{
    if (d->n == 0) {
        if (!(b & LNXRM_MOUSE_F_SYNC)) return -1;
        d->pkt[0] = b;
        d->n = 1;
        return 0;
    }
    /* Defensive: bytes is only ever 3 or 4, so n is at most 3 here and
     * this cannot fire -- but a corrupt framing must cost a dropped byte,
     * never an overrun of a 4-byte array. */
    if (d->n >= (int)(sizeof d->pkt / sizeof d->pkt[0])) {
        d->n = 0;
        return -1;
    }
    d->pkt[d->n++] = b;
    if (d->n < d->bytes) return 0;

    d->n = 0;
    unsigned char f = d->pkt[0];
    int dx = d->pkt[1] - ((f & LNXRM_MOUSE_F_XS) ? 256 : 0);
    int dy = d->pkt[2] - ((f & LNXRM_MOUSE_F_YS) ? 256 : 0);
    if (f & LNXRM_MOUSE_F_XOV) dx = 0;
    if (f & LNXRM_MOUSE_F_YOV) dy = 0;
    dy = -dy; /* PS/2 wire: positive y = up; ours is screen-space (down) */

    out->dx = dx;
    out->dy = dy;
    out->buttons = f & LNXRM_MOUSE_F_BTNS;
    out->wheel = (d->bytes == LNXRM_MOUSE_PKT_WHEEL) ? lnxrm_mouse_wheel(d->pkt[3]) : 0;
    return 1;
}

#ifdef __cplusplus
}
#endif
