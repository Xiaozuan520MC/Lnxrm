#include "ulib.h"

size_t xstrlen(const char *s)
{
    size_t n = 0;
    while (*s++) n++;
    return n;
}

void xputs(const char *s)
{ kwrite(1, s, xstrlen(s)); }

void xprinti(long v)
{
    char buf[24];
    int i = 23;
    int neg = v < 0;
    if (neg) v = -v;
    buf[i--] = 0;
    if (!v) buf[i--] = '0';
    while (v) {
        buf[i--] = '0' + (v % 10);
        v /= 10;
    }
    if (neg) buf[i--] = '-';
    xputs(&buf[i + 1]);
}

void xphex(u64 v)
{
    static const char h[] = "0123456789abcdef";
    char buf[19];
    for (int i = 0; i < 16; i++) buf[i] = h[(v >> (60 - i * 4)) & 0xF];
    buf[16] = 0;
    xputs("0x");
    xputs(buf);
}

typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_end(ap)         __builtin_va_end(ap)

void xprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            putc_(*fmt);
            continue;
        }
        fmt++;
        switch (*fmt) {
        case 'd':
            xprinti(__builtin_va_arg(ap, long));
            break;
        case 'u':
            xprinti((long)__builtin_va_arg(ap, unsigned long));
            break;
        case 'x':
            xphex(__builtin_va_arg(ap, u64));
            break;
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            xputs(s ? s : "(null)");
            break;
        }
        case 'c':
            putc_((char)__builtin_va_arg(ap, int));
            break;
        case '%':
            putc_('%');
            break;
        default:
            putc_('%');
            putc_(*fmt);
        }
    }
    va_end(ap);
}
