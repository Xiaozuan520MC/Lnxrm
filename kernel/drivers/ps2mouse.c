/* ps2mouse.c: PS/2 aux-port mouse, riding the same i8042 as the keyboard.
 *
 * The controller splits the ports by the status register's AUX bit: IRQ12
 * (IOAPIC wire 12 -> vector 44) carries aux reads only.  Every reply below
 * is bounded, so a mouse-less machine degrades to inited=0 instead of
 * hanging the boot.  Packets are the standard 3-byte stream
 *   [0] {LB,MB,RB,1,Xs,Ys,Xov,Yov}, [1] dx low 8, [2] dy low 8
 * with the 9-bit sign in the flags; an overflow packet forces the delta
 * to 0 so no garbage deltas ever reach users.  The IntelliMouse probe
 * (rate triples 200/200/10, 200/200/80, 200/100/80; 0xF2 then answers
 * id 0x03 or 0x04) switches to the 4-byte
 * stream whose 4th byte is a sign-extended 4-bit wheel delta.
 *
 * ms_feed_byte() lets ktest drive complete packets without any IRQ,
 * which keeps headless boots deterministic (real aux traffic can't
 * appear in them, so the ring starts empty and the sole producer is
 * the test). (2026-10-26) */
#include <types.h>
#include <io.h>
#include <console.h>
#include <sys/cpu.h>
#include <sys/apic.h>
#include <sys/spinlock.h>
#include <abi/lnxrm_abi.h>

#define MS_DATA   0x60
#define MS_STATUS 0x64
#define ST_OBF    0x01 /* output buffer holds a byte for us */
#define ST_AUX    0x20 /* ...and it came from the aux (mouse) port */

/* 64-slot ring, usable 63 (kbd's full/empty convention).  A 100-packet/s
 * stream overflows it after ~0.6 s unread -- events are honestly dropped. */
#define MSR_SZ 64

/* Packet length: 3 bytes standard, 4 in IntelliMouse wheel mode (the 4th
 * byte bits 0..3 are the signed wheel delta).  Set by the 0xF2 identify
 * probe; ms_set_packet_bytes() lets ktest drive both framings. */
static int ms_bytes = 3;

struct ms_event {
    int dx, dy;
    u32 buttons;
    int wheel;
};

static struct ms_event msring[MSR_SZ];
static volatile u32 ms_r, ms_w;
static volatile u32 ms_packets, ms_moved, ms_resyncs, ms_dropped;
static volatile u32 ms_inited;
static spinlock_t ms_lock = SPINLOCK_INIT;

/* Packet reassembly state.  Only ms_feed_byte() writers, which run under
 * the same lock the ring does -- the ISR and a ktest caller cannot race
 * a half-assembled packet. */
static u8 ms_pkt[4];
static int ms_npkt;

static u32 ms_count(volatile u32 *c)
{
    return __atomic_load_n(c, __ATOMIC_RELAXED);
}

u32 ms_pending(void)
{
    u32 r = __atomic_load_n(&ms_r, __ATOMIC_SEQ_CST);
    u32 w = __atomic_load_n(&ms_w, __ATOMIC_SEQ_CST);
    return (w - r) % MSR_SZ;
}

int ms_bytes_per_packet(void)
{
    return ms_bytes;
}

void ms_set_packet_bytes(int n) /* ktest framing control; init sets it once */
{
    if (n == 3 || n == 4) ms_bytes = n;
}

static void ms_push(int dx, int dy, u32 buttons, int wheel)
{
    u64 flags;
    spin_lock_irqsave(&ms_lock, &flags);
    u32 w = ms_w, r = __atomic_load_n(&ms_r, __ATOMIC_SEQ_CST);
    if ((w + 1) % MSR_SZ == r) {
        __atomic_add_fetch(&ms_dropped, 1u, __ATOMIC_RELAXED);
        spin_unlock_irqrestore(&ms_lock, flags);
        return;
    }
    msring[w].dx = dx;
    msring[w].dy = dy;
    msring[w].buttons = buttons;
    msring[w].wheel = wheel;
    __atomic_store_n(&ms_w, (w + 1) % MSR_SZ, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&ms_packets, 1u, __ATOMIC_RELAXED);
    if (dx || dy) __atomic_add_fetch(&ms_moved, 1u, __ATOMIC_RELAXED);
    spin_unlock_irqrestore(&ms_lock, flags);
}

/* One byte down the exact path the ISR rides: a flags byte must carry the
 * 0x08 sanity bit or the stream is desynced and the byte is discarded
 * (counted); a complete packet (ms_bytes long) becomes one ring event and
 * one cursor nudge.  In wheel mode the 4th byte carries a sign-extended
 * 4-bit scroll delta; its upper bits (horizontal wheel on exotic devices)
 * are ignored. */
static int ms_wheel_decode(u8 b)
{
    int w = b & 0x0F;
    return w >= 8 ? w - 16 : w;
}

void ms_feed_byte(u8 b)
{
    u64 flags;
    spin_lock_irqsave(&ms_lock, &flags);
    if (ms_npkt == 0) {
        if (!(b & 0x08)) {
            __atomic_add_fetch(&ms_resyncs, 1u, __ATOMIC_RELAXED);
            spin_unlock_irqrestore(&ms_lock, flags);
            return;
        }
        ms_pkt[0] = b;
        ms_npkt = 1;
        spin_unlock_irqrestore(&ms_lock, flags);
        return;
    }
    ms_pkt[ms_npkt++] = b;
    if (ms_npkt < ms_bytes) {
        spin_unlock_irqrestore(&ms_lock, flags);
        return;
    }
    ms_npkt = 0;
    u8 f = ms_pkt[0];
    int dx = ms_pkt[1] - (f & 0x10 ? 256 : 0);
    int dy = ms_pkt[2] - (f & 0x20 ? 256 : 0);
    if (f & 0x40) dx = 0;
    if (f & 0x80) dy = 0;
    dy = -dy; /* PS/2 wire: positive y = up; our exit is screen-space (down) */
    u32 buttons = f & 7;
    int wheel = ms_bytes == 4 ? ms_wheel_decode(ms_pkt[3]) : 0;
    spin_unlock_irqrestore(&ms_lock, flags);
    ms_push(dx, dy, buttons, wheel);
}

/* mouse_get 37's kernel half: pop one decoded event, 0 or empty. */
long mouse_pop(struct lnxrm_mouse_state *out)
{
    u64 flags;
    spin_lock_irqsave(&ms_lock, &flags);
    u32 r = ms_r, w = __atomic_load_n(&ms_w, __ATOMIC_SEQ_CST);
    if (r == w) {
        spin_unlock_irqrestore(&ms_lock, flags);
        return -1;
    }
    struct ms_event e = msring[r];
    __atomic_store_n(&ms_r, (r + 1) % MSR_SZ, __ATOMIC_SEQ_CST);
    spin_unlock_irqrestore(&ms_lock, flags);
    out->dx = e.dx;
    out->dy = e.dy;
    out->buttons = e.buttons;
    out->wheel = e.wheel;
    return 0;
}

/* mouse_info 38's kernel half: a counters snapshot, same slackness as
 * kbd_stats -- a concurrent SMP pop may skew one field, never turn a lie. */
void ms_stats(struct lnxrm_mouse_info *m)
{
    if (!m) return;
    m->pending = ms_pending();
    m->capacity = MSR_SZ - 1;
    m->packets_total = ms_count(&ms_packets);
    m->moved_total = ms_count(&ms_moved);
    m->resyncs = ms_count(&ms_resyncs);
    m->dropped = ms_count(&ms_dropped);
    m->inited = ms_count(&ms_inited);
}

/* ---- i8042 aux plumbing ---- */

/* Bounded aux reply: wait for an OBF byte that carries AUX (a keyboard
 * byte flying by is drained, never kept), give up after `budget` port
 * polls so a dead port costs milliseconds, not the boot. */
static int aux_read(u32 budget, u8 *out)
{
    while (budget--) {
        u8 st = inb(MS_STATUS);
        if (!(st & ST_OBF)) continue;
        if (!(st & ST_AUX)) {
            (void)inb(MS_DATA); /* kbd byte: ours is the other port */
            continue;
        }
        *out = inb(MS_DATA);
        return 0;
    }
    return -1;
}

static void i8042_flush(void)
{
    for (int i = 0; i < 128; i++) {
        if (!(inb(MS_STATUS) & ST_OBF)) break;
        (void)inb(MS_DATA);
    }
}

static void i8042_cmd(u8 cmd)
{
    while (inb(MS_STATUS) & 2) {}
    outb(MS_STATUS, cmd);
}

static void i8042_write(u8 v)
{
    while (inb(MS_STATUS) & 2) {}
    outb(MS_DATA, v);
}

static void aux_cmd(u8 b) /* 0xD4 prefix: to the aux device */
{
    i8042_cmd(0xD4);
    i8042_write(b);
}

static int aux_ack(u8 port_reply, u32 budget)
{
    u8 r;
    return aux_read(budget, &r) == 0 && r == port_reply ? 0 : -1;
}

/* The config reply arrives on the *keyboard* channel (AUX bit clear), so
 * it needs a raw reader with no AUX discrimination.  Only safe before any
 * kbd IRQ routing -- which mouse init is, running ahead of ps2_kbd_init. */
static int i8042_read_any(u32 budget, u8 *out)
{
    while (budget--) {
        u8 st = inb(MS_STATUS);
        if (st & ST_OBF) {
            *out = inb(MS_DATA);
            return 0;
        }
    }
    return -1;
}

#define ACK_BUDGET_FAST 200000u  /* FA replies */
#define ACK_BUDGET_SLOW 1000000u /* 0xFF reset: real mice burn ~0.75 s */

static void mouse_irq_handler(struct intr_frame *f)
{
    for (int i = 0; i < 32; i++) {
        u8 st = inb(MS_STATUS);
        if (!(st & ST_OBF)) break;
        if (!(st & ST_AUX)) break; /* kbd byte: leave it for the IRQ1 handler */
        ms_feed_byte(inb(MS_DATA));
    }
}

/* IntelliMouse family probe.  The impossible-in-practice rate triple
 * 200/200/x switches a wheel mouse to a 4-byte reporting mode; vendors
 * disagree about x, so a few triples are offered and 0xF2 identifies the
 * live one afterwards: id 3 = wheel mouse, id 4 = "Explorer" -- both send
 * the 4-byte packet with the wheel byte.  QEMU's detector accepts
 * 200/200/80 and 200/100/80.  Every reply is bounded: an unresponsive or
 * plain 3-button device just stays 3-byte. */
static void ms_probe_wheel(void)
{
    static const u8 tries[3][3] = {
        {200, 200, 10},  /* MS IntelliMouse */
        {200, 200, 80},  /* MS IntelliMouse Explorer, QEMU IMEX */
        {200, 100, 80},  /* wheel clones, QEMU IMPS/2 */
    };
    for (int i = 0; i < 3 && ms_bytes == 3; i++) {
        for (int j = 0; j < 3; j++) {
            aux_cmd(0xF3);
            if (aux_ack(0xFA, ACK_BUDGET_FAST)) return; /* device went quiet */
            aux_cmd(tries[i][j]);
            if (aux_ack(0xFA, ACK_BUDGET_FAST)) return;
        }
        aux_cmd(0xF2);
        u8 id;
        if (aux_ack(0xFA, ACK_BUDGET_FAST) == 0 && aux_read(ACK_BUDGET_FAST, &id) == 0 &&
            (id == 0x03 || id == 0x04)) {
            ms_bytes = 4;
        }
    }
}

void ps2_mouse_init(void)
{
    i8042_flush();
    i8042_cmd(0xA8); /* enable the aux port */

    /* Controller config: aux interrupt on, aux clock on.  The reply is a
     * plain data read (kernels before us read it the same way); bit1 is
     * why routing below may see packets, so descriptor ordering matters. */
    i8042_cmd(0x20);
    u8 r;
    u32 cfg = 0;
    int got = i8042_read_any(ACK_BUDGET_FAST, &r);
    if (got == 0) cfg = r; /* read failure keeps whatever the ROM left */
    i8042_cmd(0x60);
    i8042_write((u8)((cfg | 0x02) & ~0x20));

    /* reset -> expect ACK 0xFA, then 0xAA and its id 0x00 */
    aux_cmd(0xFF);
    if (aux_ack(0xFA, ACK_BUDGET_SLOW) || aux_ack(0xAA, ACK_BUDGET_SLOW) ||
        aux_ack(0x00, ACK_BUDGET_FAST))
        goto fail;

    aux_cmd(0xF6); /* sane defaults */
    if (aux_ack(0xFA, ACK_BUDGET_FAST)) goto fail;

    ms_probe_wheel();

    aux_cmd(0xF4); /* enable streaming: packets may start any moment */
    if (aux_ack(0xFA, ACK_BUDGET_FAST)) goto fail;

    /* route before the first packet matters: an unhandled vector is a
     * silent EOI and the byte waits in the OBF for the future handler */
    ioapic_set_irq(12, 44, 0);
    ioapic_unmask_irq(12);
    irq_install(12, mouse_irq_handler);

    __atomic_store_n(&ms_inited, 1u, __ATOMIC_RELAXED);
    kprintf("[mouse] PS/2 aux ready, IRQ12 -> vector 44, %d-byte packets%s\n", ms_bytes,
            ms_bytes == 4 ? " (wheel)" : "");
    return;

fail:
    i8042_cmd(0xA7); /* disable aux: dead port stays quiet */
    kprintf("[mouse] no aux device, IRQ12 left down\n");
}
