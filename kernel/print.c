#include <console.h>
#include <mm/mm.h>
#include <sys/cpu.h>
#include <io.h>
#include <sys/spinlock.h>
#include <disk.h> /* C27: the panic door flushes staged sectors */

/* Serialize all console output (kprintf AND console_write) across CPUs.
 * Exported via console_out_lock/unlock: console_write in vfs.c shares it,
 * and both sides are irqsave because interrupt handlers print too (a
 * guard taken with interrupts on deadlocks when the same CPU re-enters
 * kprintf from an IRQ). IRQs are re-enabled only between console_write
 * chunks so the irq-off window stays bounded. */
static spinlock_t print_lock = SPINLOCK_INIT;

void console_out_lock(u64 *flags) { spin_lock_irqsave(&print_lock, flags); }
void console_out_unlock(u64 flags) { spin_unlock_irqrestore(&print_lock, flags); }

/* VGA text console at the high alias; COM1 via drivers/uart.c. */
extern void lnxrm_uart_init(void);
extern void lnxrm_uart_putc(char c);
extern int lnxrm_uart_trygetc(void);

/* framebuffer console */
#include <framebuffer.h>

#define VGA_COLS 80
#define VGA_ROWS 25
volatile u16 *vga = (volatile u16 *)VGA_VMA;
int vrow, vcol;

/* ring buffer that captures every character sent to the console,
 * so fb_console_init can replay messages that scrolled off VGA. */
#define RING_SIZE 4096
static char ring_buf[RING_SIZE];
static int ring_head, ring_count;

static void ring_putc(char c)
{
    int idx = (ring_head + ring_count) % RING_SIZE;
    if (ring_count < RING_SIZE) {
        ring_buf[idx] = c;
        ring_count++;
    } else {
        ring_buf[ring_head] = c;
        ring_head = (ring_head + 1) % RING_SIZE;
    }
}

/* called by fb_console_init to replay buffered output */
int ring_get_count(void)
{ return ring_count; }
char ring_get_char(int i)
{ return ring_buf[(ring_head + i) % RING_SIZE]; }

static void vga_update_cursor(void);

void console_init(void)
{
    lnxrm_uart_init();
    for (int r = 0; r < VGA_ROWS; r++)
        for (int c = 0; c < VGA_COLS; c++) vga[r * VGA_COLS + c] = 0x0700;
    vrow = vcol = 0;

    /* Set cursor shape: scanlines 13–15 (block cursor) */
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x0D);
    outb(0x3D4, 0x0B);
    outb(0x3D5, 0x0F);
    vga_update_cursor();
}

static void vga_update_cursor(void)
{
    u16 pos = vrow * VGA_COLS + vcol;
    outb(0x3D4, 0x0F);
    outb(0x3D5, pos & 0xFF);
    outb(0x3D4, 0x0E);
    outb(0x3D5, (pos >> 8) & 0xFF);
}

static void vga_scroll(void)
{
    for (int r = 1; r < VGA_ROWS; r++)
        memcpy((void *)&vga[(r - 1) * VGA_COLS], (void *)&vga[r * VGA_COLS], VGA_COLS * 2);
    for (int c = 0; c < VGA_COLS; c++) vga[(VGA_ROWS - 1) * VGA_COLS + c] = 0x0700;
}

/* ---- ANSI escape sequence tokenizer --------------------------------------
 * Lives here rather than in a file of its own because print.c is the console
 * core: it owns the shared console lock (console_out_lock) and this state
 * machine, and the two backends -- the VGA text console below and the
 * framebuffer console in framebuffer.c -- are its consumers.  A thirty-line
 * shared helper did not justify a translation unit of its own. */
int ansi_feed(struct ansi_seq *s, char c)
{
    if (s->phase == 0) {
        if (c == '\033') {
            s->phase = 1;
            return ANSI_CONSUMED;
        }
        return ANSI_TEXT;
    }
    if (s->phase == 1) {
        s->phase = (c == '[') ? 2 : 0;
        if (c == '[') s->len = 0;
        return ANSI_CONSUMED;
    }
    if (s->len < (int)sizeof(s->buf) - 1) s->buf[s->len++] = c;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
        s->buf[s->len] = 0;
        s->phase = 0;
        return ANSI_DONE;
    }
    return ANSI_CONSUMED;
}

int ansi_color_index(int code)
{
    static const u8 map[] = {0, 4, 2, 6, 1, 5, 3, 7};
    return map[code - 30];
}

/* ANSI escape sequence state machine (tokenizer shared with framebuffer.c) */
static struct ansi_seq vga_ansi;
static u8 cur_attr = 0x07; /* current VGA attribute (default: light grey on black) */

static void esc_process(const char *buf, int len)
{
    /* buf contains the params after ESC[ */
    char last = buf[len - 1];
    if (last == 'J') {
        /* ESC[2J = clear screen */
        if (len >= 2 && buf[0] == '2') {
            for (int r = 0; r < VGA_ROWS; r++)
                for (int c = 0; c < VGA_COLS; c++) vga[r * VGA_COLS + c] = 0x0700;
            vrow = vcol = 0;
        }
    } else if (last == 'H') {
        /* ESC[H or ESC[row;colH = cursor position (1-based) */
        int row = 1, col = 1;
        int num = 0, have_num = 0, semi = 0;
        for (int i = 0; i < len - 1; i++) {
            if (buf[i] >= '0' && buf[i] <= '9') {
                num = num * 10 + (buf[i] - '0');
                have_num = 1;
            } else if (buf[i] == ';') {
                if (have_num) {
                    row = num;
                    num = 0;
                    have_num = 0;
                }
                semi = 1;
            }
        }
        /* CSI n H = row n, column 1; only a ';' introduces the column */
        if (semi) {
            if (have_num) col = num;
        } else if (have_num) {
            row = num;
        }
        vrow = row - 1;
        vcol = col - 1;
        if (vrow < 0) vrow = 0;
        if (vrow >= VGA_ROWS) vrow = VGA_ROWS - 1;
        if (vcol < 0) vcol = 0;
        if (vcol >= VGA_COLS) vcol = VGA_COLS - 1;
    } else if (last == 'm') {
        /* ESC[...m = Set Graphics Rendition (colors) */
        if (len == 1) { /* ESC[m == ESC[0m: empty parameter list = reset */
            cur_attr = 0x07;
            return;
        }
        int num = 0;
        u8 fg = cur_attr & 0x0F;
        u8 bg = (cur_attr >> 4) & 0x07;
        int have_num = 0;
        for (int i = 0; i < len; i++) {
            if (buf[i] >= '0' && buf[i] <= '9') {
                num = num * 10 + (buf[i] - '0');
                have_num = 1;
            } else if (buf[i] == ';' || i == len - 1) {
                if (have_num) {
                    if (num == 0) {
                        fg = 7;
                        bg = 0; /* reset */
                    } else if (num >= 30 && num <= 37) {
                        fg = ansi_color_index(num);
                    } else if (num >= 40 && num <= 47) {
                        bg = ansi_color_index(num - 10);
                    } else if (num == 1) {
                        fg |= 0x08; /* bold = bright */
                    }
                    num = 0;
                    have_num = 0;
                }
            }
        }
        cur_attr = (bg << 4) | fg;
    }
}

void console_putc(char c)
{
    /* Always forward to fb console so ANSI escapes reach it */
    fb_console_putc(c);

    /* Feed ANSI escape sequences into the VGA state machine */
    int r = ansi_feed(&vga_ansi, c);
    if (r == ANSI_DONE) {
        esc_process(vga_ansi.buf, vga_ansi.len);
        vga_update_cursor(); /* the sequence may have moved it (ESC[H ...) */
        return;
    }
    if (r == ANSI_CONSUMED) return;

    /* Normal character handling */
    if (vcol >= VGA_COLS) {
        vcol = 0;
        if (++vrow >= VGA_ROWS) {
            vga_scroll();
            vrow--;
        }
    }
    switch (c) {
    case '\b':
        if (vcol > 0) {
            vcol--;
            vga[vrow * VGA_COLS + vcol] = (cur_attr << 8) | ' ';
        }
        break;
    case '\n':
        vcol = 0;
        if (++vrow >= VGA_ROWS) {
            vga_scroll();
            vrow--;
        }
        break;
    case '\r':
        break;
    case '\t':
        vcol = (vcol / 8 + 1) * 8;
        if (vcol >= VGA_COLS) {
            vcol = 0;
            if (++vrow >= VGA_ROWS) {
                vga_scroll();
                vrow--;
            }
        }
        break;
    default:
        vga[vrow * VGA_COLS + vcol] = (cur_attr << 8) | (u8)c;
        vcol++;
    }
    lnxrm_uart_putc(c);
    ring_putc(c);
    vga_update_cursor();
}

/* ---- minimal printf ---- */
static const char digits[] = "0123456789abcdef";

static void emit(char **buf, size_t *left, char c)
{
    if (*left > 1) {
        **buf = c;
        (*buf)++;
        (*left)--;
    }
}

static void putnum(char **b, size_t *l, u64 v, unsigned base, bool sgn, int width, char pad,
                   bool upper)
{
    char tmp[32];
    int i = 0;
    bool neg = false;
    if (sgn && (i64)v < 0) {
        neg = true;
        v = -(i64)v;
    }
    do {
        char c = digits[v % base];
        if (upper && c >= 'a') c -= 'a' - 'A';
        tmp[i++] = c;
        v /= base;
    } while (v);
    if (neg) tmp[i++] = '-';
    int padn = width - i;
    while (padn-- > 0) emit(b, l, pad);
    while (i--) emit(b, l, tmp[i]);
}

size_t vsnprintf(char *buf, size_t size, const char *fmt, __builtin_va_list ap)
{
    char *p = buf;
    size_t left = size;

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            emit(&p, &left, *fmt);
            continue;
        }
        fmt++;
        char pad = ' ';
        int width = 0;
        while (*fmt == '-' || *fmt == '+' || *fmt == ' ' || *fmt == '#' || *fmt == '\'')
            fmt++; /* ignored flags */
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        if (*fmt == '.') { /* precision -- ignored */
            fmt++;
            while (*fmt >= '0' && *fmt <= '9') fmt++;
        }
        bool lng = false;
        if (*fmt == 'l') {
            lng = true;
            fmt++;
            if (*fmt == 'l') fmt++;
        } else if (*fmt == 'z') {
            lng = true;
            fmt++;
        }
        switch (*fmt) {
        case 'd':
            putnum(&p, &left, lng ? __builtin_va_arg(ap, long) : __builtin_va_arg(ap, int), 10,
                   true, width, pad, false);
            break;
        case 'u':
            putnum(&p, &left,
                   lng ? __builtin_va_arg(ap, unsigned long) : __builtin_va_arg(ap, unsigned), 10,
                   false, width, pad, false);
            break;
        case 'x':
            putnum(&p, &left,
                   lng ? __builtin_va_arg(ap, unsigned long) : __builtin_va_arg(ap, unsigned), 16,
                   false, width, pad, false);
            break;
        case 'X':
            putnum(&p, &left,
                   lng ? __builtin_va_arg(ap, unsigned long) : __builtin_va_arg(ap, unsigned), 16,
                   false, width, pad, true);
            break;
        case 'p':
            emit(&p, &left, '0');
            emit(&p, &left, 'x');
            putnum(&p, &left, __builtin_va_arg(ap, uptr), 16, false, 16, '0', false);
            break;
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s) emit(&p, &left, *s++);
            break;
        }
        case 'c':
            emit(&p, &left, (char)__builtin_va_arg(ap, int));
            break;
        case '%':
            emit(&p, &left, '%');
            break;
        default:
            emit(&p, &left, '%');
            emit(&p, &left, *fmt);
        }
    }
    if (size) {
        if (left > 0)
            *p = 0;
        else
            buf[size - 1] = 0;
    }
    return p - buf;
}

/* Emit a NUL-terminated literal with no formatting and no lock.  Used by
 * panic() for the fixed "[PANIC] " / color-reset wraps around the message. */
static void putstr_unlocked(const char *s)
{
    while (*s) console_putc(*s++);
}

/* Format `fmt` and push it to the console WITHOUT taking print_lock.
 * vsnprintf does the formatting and console_putc is itself lock-free, so
 * the only thing skipped here is the serialization.  This is what panic()
 * must use: if the caller already holds print_lock (e.g. a BUG inside the
 * console path itself), taking it again would spin forever with interrupts
 * off instead of halting -- the exact self-deadlock T-002 describes. */
void kvprintf_unlocked(const char *fmt, __builtin_va_list ap)
{
    static char kbuf[1024];
    vsnprintf(kbuf, sizeof(kbuf), fmt, ap);
    for (char *s = kbuf; *s; s++) console_putc(*s);
}

void kprintf_unlocked(const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    kvprintf_unlocked(fmt, ap);
    __builtin_va_end(ap);
}

void kvprintf(const char *fmt, __builtin_va_list ap)
{
    static char kbuf[1024];
    u64 flags;
    console_out_lock(&flags);
    vsnprintf(kbuf, sizeof(kbuf), fmt, ap);
    for (char *s = kbuf; *s; s++) console_putc(*s);
    console_out_unlock(flags);
}

void kprintf(const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    kvprintf(fmt, ap);
    __builtin_va_end(ap);
}

/* Reentrancy latch: once panic() has begun it never returns, so a second
 * entry (a fault raised inside the panic path) must not recurse -- it halts
 * the machine immediately.  Exported so tests can observe the guard. */
static volatile int s_panicking;
int in_panic(void) { return s_panicking; }

__attribute__((noreturn)) void panic(const char *fmt, ...)
{
    __builtin_va_list ap;

    __asm__ volatile("cli"); /* interrupts off first: no preemption mid-panic */
    if (s_panicking) {       /* already panicking: a nested fault must halt, not recurse */
        __asm__ volatile("hlt; jmp .");
        for (;;);
    }
    s_panicking = 1;

    /* Unlocked, bold-red path: works even if the caller holds print_lock.
     * Output may interleave with another CPU's print, but the machine is
     * dying anyway -- halting beats a clean transcript. */
    putstr_unlocked("\033[1;31m[PANIC] "); /* bold red */
    __builtin_va_start(ap, fmt);
    kvprintf_unlocked(fmt, ap);
    __builtin_va_end(ap);
    putstr_unlocked("\033[0m\n"); /* reset color */

    /* The panic text sits in the back surface like any other console
     * output, and no tick will ever present it: interrupts are off and
     * the machine halts below.  The last word on screen is the one thing
     * here that must not depend on the present clock. */
    fb_flip();

    /* C27: best effort -- what the caller was promised goes to the device
     * even as the machine dies.  The try-flush never waits anywhere, so a
     * panic that happens to own a lock still reaches `hlt` below (to
     * attempt waits on a dying machine would trade lost bytes for lost
     * halts). */
    blk_cache_sync_all_try();

    __asm__ volatile("hlt; jmp .");
    for (;;);
}
