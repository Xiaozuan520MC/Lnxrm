// SPDX-License-Identifier: Apache-2.0

/* ps2-keyboard.c: the keyboard's decode, and the ring0 half of the S1c
 * channel.
 *
 * WHAT MOVED HERE (2026-11-17)
 *   The two lookup tables, the shift/capslock/E0 state machine and
 *   kbd_process_sc() used to sit inline in irq/isr.c between the exception
 *   vectors and the serial handler.  They are now reached through
 *   include/abi/ps2-keyboard.h -- the same header userland/mod/ps2-keyboard.c
 *   includes -- so the kernel's IRQ path and the ring3 module compile ONE
 *   table instead of two that can drift apart.
 *
 * WHO DECODES (S1c, 2026-11-19)
 *   IRQ1 hands each raw byte to exactly ONE of the two decoders: the
 *   ring3 module while a descriptor holds the "kbd" channel open
 *   (dipc_push_if_listening), the kernel otherwise -- no .km on the
 *   image, or the module exited.  The module runs the SAME
 *   lnxrm_kbd_decode() and sends every character plus its modifiers back
 *   through the channel's sink (kbd_sink below), which lands it exactly
 *   where kbd_process_sc's tail would: the console inbuf, then the
 *   unified queue39-41.  A boot with the module, a boot without it and
 *   sys_kbd_inject 35 therefore cannot be told apart from outside.
 *
 * WHAT STILL DECODES HERE, AND WHY
 *   Two callers: the no-reader fallback in the IRQ1 stub, and ktest's
 *   kbd_inject_scancode(), which drives shift/caps through the real
 *   state machine without a keyboard or a module present.
 *   sys_kbd_inject 35 never decoded -- it pushes ASCII into the console
 *   inbuf directly -- so the 34/35 contract is untouched either way.
 *
 * TWO DECODERS, ONE STREAM
 *   This file owns the kernel's struct lnxrm_kbd_state; the module owns
 *   its own over the bytes it pops from the ring.  They are deliberately
 *   not shared: each byte reaches only ONE of them, so a break code seen
 *   by one must not clear the other's shift bit, and the kernel's copy is
 *   the one ktest's injector drives. */
#include <sys/cpu.h>
#include <sys/input.h>
#include <sys/dipc.h>
#include <console.h> /* kprintf */
#include <abi/ps2-keyboard.h>

static struct lnxrm_kbd_state kbd_state;

/* One make-code, decoded, pushed to both of the queues that want it. */
void kbd_process_sc(u8 sc)
{
    char c = lnxrm_kbd_decode(&kbd_state, sc);
    if (!c) return; /* break code, bare modifier, E0 key, unmapped index */

    input_push(c);
    /* 39: the unified queue gets the same decoded character plus the
     * recipe (which modifiers were engaged) -- the character is already
     * transformed, the flags say *why*.  Read the modifiers AFTER the
     * decode, which is where they settled: the case flip above changes the
     * character, never the state that produced it. */
    input_push_key((int)c,
                   (u32)(kbd_state.shift ? INPUT_MOD_SHIFT : 0) |
                       (u32)(kbd_state.caps ? INPUT_MOD_CAPS : 0));
}

/* The ring3 module's return path (S1c): one decoded character plus the
 * modifiers that produced it, delivered with dipc_send(55).  The tail is
 * kbd_process_sc's own -- input_push into the console inbuf, then
 * input_push_key with the modifiers read AFTER the decode -- so a takeover
 * by the module is invisible to sys_kbd_get 34, the shell and the unified
 * queue39-41.  Pairs, because the character alone cannot say why it is
 * upper-case; an odd length is a truncated pair, not a keystroke, and is
 * refused rather than half-applied.  Called outside dipc_lock (see
 * sys_dipc_send), so taking con_lock/evq_lock here is the same lock order
 * the IRQ producers already use. */
static long kbd_sink(const u8 *buf, u32 len)
{
    if (len & 1) return LNXRM_EINVAL;
    long fed = 0;
    for (u32 i = 0; i + 1 < len; i += 2) {
        char c = (char)buf[i];
        if (!c) continue; /* the decoder's "nothing to show" answer */
        input_push(c);
        /* Mask to the two bits this channel defines: a module that sent
         * anything else would be inventing modifiers the queue has no
         * name for. */
        input_push_key((int)c,
                       (u32)buf[i + 1] & (INPUT_MOD_SHIFT | INPUT_MOD_CAPS));
        fed++;
    }
    return fed;
}

/* Bind IRQ1's raw byte stream to a ring3 reader -- and give that reader
 * its way back in (the sink).  Called once from start_kernel, and
 * deliberately LATE: see the call site. */
void kbd_decode_init(void)
{
    int rc = dipc_create(LNXRM_KBD_CHANNEL, kbd_sink);
    if (rc == 0) {
        kprintf("[kbd] raw scancodes available on dipc \"%s\"\n", LNXRM_KBD_CHANNEL);
    } else {
        kprintf("[kbd] channel \"%s\" not created (%d) -- keyboard stays kernel-only\n",
                LNXRM_KBD_CHANNEL, rc);
    }
}
