/* The keyboard's scancode set 1 -> ASCII decode: the SINGLE source of it,
 * compiled into both halves of the S1b keyboard channel.
 *
 *   kernel/drivers/ps2-keyboard.c   ring0 -- the IRQ1 path, syscalls 34/35
 *   userland/mod/ps2-keyboard.c            ring3 -- the /mod driver module
 *
 * WHY IT IS A HEADER AND NOT A .C
 *   The two halves live in different address spaces and are linked by
 *   different linkers, so "one source" here cannot mean "one object".  It
 *   means one file: a second copy of these tables is a second copy that
 *   drifts the first time somebody fixes a shifted punctuation mark and
 *   forgets which of the two they were editing.
 *
 * WHY THE STATE IS NOT SHARED
 *   Each side keeps its OWN struct lnxrm_kbd_state, on purpose.  IRQ1
 *   gives each byte to exactly ONE of them -- the module while a
 *   descriptor holds the channel open (S1c), the kernel otherwise -- so a
 *   break code seen by one must not clear the other's shift bit, and the
 *   kernel's copy is the one ktest's kbd_inject_scancode() drives, which
 *   has to keep answering sys_kbd_get 34 exactly as it did before the
 *   channel existed.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* The dipc channel IRQ1 pushes its raw bytes into, named here so the
 * producer (kernel/irq/isr.c), the binder (drivers/ps2-keyboard.c) and the
 * consumer (userland/mod/ps2-keyboard.c) cannot disagree about it. */
#define LNXRM_KBD_CHANNEL "kbd"

/* scancode set 1: index is the code with its break bit (0x80) masked off */
#define LNXRM_KBD_MAP_N 128

#define LNXRM_KBD_SC_LSHIFT   0x2A
#define LNXRM_KBD_SC_RSHIFT   0x36
#define LNXRM_KBD_SC_CAPSLOCK 0x3A

/* One decoder's modifier and prefix state.  Three bits, so a stack copy is
 * free and there is nothing wide enough for an interrupt to tear. */
struct lnxrm_kbd_state {
    unsigned shift : 1; /* either shift key is down */
    unsigned caps : 1;  /* capslock latched */
    unsigned ext : 1;   /* an E0 prefix is pending for the NEXT byte */
};

/* Decode exactly ONE byte of the stream.
 *
 * Returns the ASCII character, or 0 when the byte produced nothing a
 * terminal can render: a break code, a bare modifier, an E0-prefixed key
 * (arrows, keypad enter -- not ASCII, so they keep console behaviour only)
 * or an index the table leaves empty.  0 is the only "no output" answer;
 * '\0' is not a character this path can produce.
 *
 * The tables are block-scope statics inside the function on purpose: they
 * are then emitted only in translation units that actually call this, and
 * no -Wunused-const-variable can fire on a table a TU included but did not
 * use. */
static inline char lnxrm_kbd_decode(struct lnxrm_kbd_state *st, unsigned char sc)
{
    /* clang-format off */   /* raw lookup table */
    static const char plain[LNXRM_KBD_MAP_N] = {
        0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
        '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
        0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
        0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
        0, '*', 0, ' ', 0,
    };

    static const char shifted[LNXRM_KBD_MAP_N] = {
        0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
        '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
        0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
        0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
        0, '*', 0, ' ', 0,
    };
    /* clang-format on */

    if (sc == 0xE0) {
        st->ext = 1;
        return 0;
    }
    int release = (sc & 0x80) != 0;
    int ext = st->ext;
    unsigned code = sc & 0x7fu; /* always < LNXRM_KBD_MAP_N */
    st->ext = 0;                /* the E0 prefix belongs to this code only */

    if (code == LNXRM_KBD_SC_LSHIFT || code == LNXRM_KBD_SC_RSHIFT) {
        st->shift = release ? 0u : 1u;
        return 0;
    }
    if (code == LNXRM_KBD_SC_CAPSLOCK) {
        if (!release) st->caps ^= 1u; /* toggles on the make, not the break */
        return 0;
    }
    if (release) return 0;
    if (ext || plain[code] == 0) return 0;

    char c = st->shift ? shifted[code] : plain[code];
    if (st->caps) {
        /* Capslock flips the case of letters and only letters: it must not
         * turn '1' into '!'.  The shift table already chose the symbol, so
         * this runs on the final character, not on the raw index. */
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        else if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
    }
    return c;
}

#ifdef __cplusplus
}
#endif
