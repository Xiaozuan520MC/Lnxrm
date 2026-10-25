#include <console.h>

void *memset(void *d, int c, size_t n)
{
    u8 *p = d;
    while (n--) *p++ = (u8)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    u8 *dp = d;
    const u8 *sp = s;
    while (n--) *dp++ = *sp++;
    return d;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (*s++) n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) a++, b++;
    return (u8)*a - (u8)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) a++, b++, n--;
    return n ? (u8)*a - (u8)*b : 0;
}

char *strcpy(char *d, const char *s)
{
    char *r = d;
    while ((*d++ = *s++));
    return r;
}

char *strncpy(char *d, const char *s, size_t n)
{
    char *r = d;
    while (n-- && (*d++ = *s++));
    while ((long)n-- > 0) *d++ = 0;
    return r;
}

char *strchr(const char *s, int c)
{
    while (*s && *s != (char)c) s++;
    return *s == (char)c ? (char *)s : NULL;
}

char *strrchr(const char *s, int c)
{
    char *r = NULL;
    while (*s) {
        if (*s == (char)c) r = (char *)s;
        s++;
    }
    return r;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const u8 *x = a, *y = b;
    while (n--)
        if (*x++ != *y++) return x[-1] < y[-1] ? -1 : 1;
    return 0;
}

static char tolower(int c)
{
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

int strcasecmp(const char *a, const char *b)
{
    while (*a && *b) {
        int diff = tolower(*a) - tolower(*b);
        if (diff) return diff;
        a++, b++;
    }
    return tolower(*a) - tolower(*b);
}
