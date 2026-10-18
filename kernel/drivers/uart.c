/* uart.c: 16550 UART driver (COM1) -- the kernel's primary console.
 *
 * Pure C (ported from rust/uart.rs 2026-10-17): same register programming,
 * same pre-init gating, same ISA-post settle read after every byte.  The four
 * lnxrm_uart_* entry points below are the entire ABI -- print.c / isr.c /
 * vfs.c / main.c declare them locally and call them exactly as before.
 *
 * uart.rs's write_str() is not ported: its only consumer was Rust's own
 * panic handler (lib.rs), and both left with the Rust component.
 *
 * The Rust original guarded everything with a SeqCst AtomicBool; lnxrm_
 * uart_init() runs once on the BSP before any other CPU exists and `inited`
 * is never written again, so a plain int gives the same guarantee here. */
#include <types.h>
#include <io.h>

#define COM1 0x3F8u

/* 16550 register offsets */
#define U_DATA 0 /* RBR / THR */
#define U_IER  1
#define U_FCR  2
#define U_LCR  3
#define U_MCR  4
#define U_LSR  5

static int inited; /* output before lnxrm_uart_init() is dropped, as before */

void lnxrm_uart_init(void)
{
    outb(COM1 + U_IER, 0x00);  /* no irq while programming */
    outb(COM1 + U_LCR, 0x80);  /* DLAB */
    outb(COM1 + U_DATA, 0x01); /* divisor low: 115200 baud */
    outb(COM1 + U_IER, 0x00);  /* divisor high (DLAB maps IER -> DLM) */
    outb(COM1 + U_LCR, 0x03);  /* 8N1 */
    outb(COM1 + U_FCR, 0xC7);  /* FIFO on */
    outb(COM1 + U_MCR, 0x0B);
    inited = 1;
}

static void send_byte(u8 b)
{
    if (!inited) return;
    while (!(inb(COM1 + U_LSR) & 0x20)) /* LSR.THRE: holding register empty */
        __asm__ volatile("pause");
    outb(COM1 + U_DATA, b);
    /* settle: an ISA-post style dummy read keeps QEMU's chardev backpressure
     * happy during long bursts */
    (void)inb(0x84);
}

void lnxrm_uart_putc(char c)
{
    if (c == '\n') send_byte('\r');
    send_byte((u8)c);
}

static int recv_ready(void)
{
    return inited && (inb(COM1 + U_LSR) & 0x01); /* LSR.DR: data ready */
}

int lnxrm_uart_trygetc(void)
{
    return recv_ready() ? (int)inb(COM1 + U_DATA) : -1;
}

/* enable the RX interrupt so IRQ4 pushes bytes into the input queue */
int lnxrm_uart_irq_enable(void)
{
    if (!inited) return -1;
    outb(COM1 + U_IER, 0x01);
    return 0;
}
