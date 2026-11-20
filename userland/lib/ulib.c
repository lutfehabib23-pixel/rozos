/* rozOS — টেস্ট প্রোগ্রামগুলোর ছোট সহায়ক লাইব্রেরি (ধাপ ৬) */

#include "rozsys.h"
#include <stdarg.h>

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

void *memset(void *d, int c, size_t n)
{
    unsigned char *p = (unsigned char *)d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *p = (unsigned char *)d;
    const unsigned char *q = (const unsigned char *)s;
    while (n--) *p++ = *q++;
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *p = (unsigned char *)d;
    const unsigned char *q = (const unsigned char *)s;
    if (p < q) {
        while (n--) *p++ = *q++;
    } else {
        while (n--) p[n] = q[n];
    }
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = (const unsigned char *)a, *q = (const unsigned char *)b;
    for (; n; n--, p++, q++) {
        if (*p != *q) return *p - *q;
    }
    return 0;
}

static char out[512];
static size_t olen;

static void flush(void)
{
    if (olen) {
        write(1, out, olen);
        olen = 0;
    }
}

static void put(char c)
{
    if (olen == sizeof(out)) flush();
    out[olen++] = c;
}

static void put_num(unsigned long v, int base, int neg, int width, char pad)
{
    char buf[24];
    int n = 0;
    do {
        int d = (int)(v % (unsigned long)base);
        buf[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v /= (unsigned long)base;
    } while (v);
    if (neg) buf[n++] = '-';
    while (n < width--) put(pad);
    while (n) put(buf[--n]);
}

/* %d %u %x %s %c %p, l-প্রিফিক্স, প্রস্থ (যেমন %08x) */
int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        int lng = 0, width = 0;
        char pad = ' ';
        if (*fmt != '%') { put(*fmt); continue; }
        fmt++;
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l') { lng = 1; fmt++; }
        switch (*fmt) {
        case 'd': {
            long v = lng ? va_arg(ap, long) : va_arg(ap, int);
            put_num(v < 0 ? (unsigned long)-v : (unsigned long)v, 10, v < 0, width, pad);
            break;
        }
        case 'u': put_num(lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), 10, 0, width, pad); break;
        case 'x': put_num(lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned), 16, 0, width, pad); break;
        case 'p': put('0'); put('x'); put_num((unsigned long)va_arg(ap, void *), 16, 0, width, pad); break;
        case 'c': put((char)va_arg(ap, int)); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s) put(*s++);
            break;
        }
        case '%': put('%'); break;
        default: put('%'); put(*fmt); break;
        }
    }
    va_end(ap);
    flush();
    return 0;
}

int puts(const char *s)
{
    write(1, s, strlen(s));
    write(1, "\n", 1);
    return 0;
}

long now_ms(void)
{
    struct timespec ts;
    clock_gettime(1, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void sleep_ms(long ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    nanosleep(&ts, NULL);
}
