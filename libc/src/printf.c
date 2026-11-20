/* rozlibc — printf পরিবার (একটাই ফরম্যাট-ইঞ্জিন, আউটপুট callback দিয়ে)
 *
 * সাপোর্ট: %d %i %u %x %X %o %c %s %p %% %n, %f %F %e %g (double),
 * ফ্ল্যাগ - + space # 0, প্রস্থ ও precision (সংখ্যা বা *), দৈর্ঘ্য hh h l ll z t j L। */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

struct sink {
    void (*put)(struct sink *s, const char *p, size_t n);
    char *buf;                   /* snprintf */
    size_t size, len;
    FILE *file;                  /* fprintf */
    int fd;                      /* dprintf */
    size_t total;
};

#define F_LEFT  1
#define F_PLUS  2
#define F_SPACE 4
#define F_ALT   8
#define F_ZERO  16

static void emit(struct sink *s, const char *p, size_t n)
{
    if (n) {
        s->put(s, p, n);
        s->total += n;
    }
}

static void pad(struct sink *s, char c, int n)
{
    char b[32];
    memset(b, c, sizeof(b));
    while (n > 0) {
        int k = n > 32 ? 32 : n;
        emit(s, b, (size_t)k);
        n -= k;
    }
}

/* prefix (যেমন "-", "0x") + অঙ্কগুলো, প্রস্থ ও precision মেনে */
static void emit_number(struct sink *s, const char *prefix, const char *digits, int ndig,
                        int width, int prec, int flags)
{
    int plen = (int)strlen(prefix), zeros = 0, total;
    if (prec >= 0 && ndig < prec) zeros = prec - ndig;
    if (prec == 0 && ndig == 1 && digits[0] == '0') ndig = 0;   /* printf("%.0d", 0) = "" */
    total = plen + zeros + ndig;
    if ((flags & F_ZERO) && !(flags & F_LEFT) && prec < 0 && width > total) {
        zeros += width - total;
        total = width;
    }
    if (!(flags & F_LEFT)) pad(s, ' ', width - total);
    emit(s, prefix, (size_t)plen);
    pad(s, '0', zeros);
    emit(s, digits, (size_t)ndig);
    if (flags & F_LEFT) pad(s, ' ', width - total);
}

static int utoa(unsigned long long v, unsigned base, int upper, char *out)
{
    char tmp[24];
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0, i;
    do { tmp[n++] = dig[v % base]; v /= base; } while (v);
    for (i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

/* double → দশমিক লেখা (ছোট বাস্তবায়ন: 18 অঙ্ক পর্যন্ত নিখুঁত, তার বেশি আনুমানিক) */
static void emit_double(struct sink *s, double v, char conv, int width, int prec, int flags)
{
    char buf[400], digs[40];
    const char *sign = "";
    int n = 0, exp10 = 0, i, use_exp = 0, g_mode = 0;
    unsigned long long ip, fp, scale = 1;

    if (v != v) { emit_number(s, "", "nan", 3, width, -1, flags & ~F_ZERO); return; }
    if (v < 0) { sign = "-"; v = -v; }
    else if (flags & F_PLUS) sign = "+";
    else if (flags & F_SPACE) sign = " ";
    if (v > 1.7976931348623157e308) { emit_number(s, sign, "inf", 3, width, -1, flags & ~F_ZERO); return; }
    if (prec < 0) prec = 6;

    if (conv == 'g' || conv == 'G') {
        double t = v;
        g_mode = 1;
        if (prec == 0) prec = 1;
        if (t != 0.0) {
            while (t >= 10.0) { t /= 10.0; exp10++; }
            while (t < 1.0) { t *= 10.0; exp10--; }
        }
        if (exp10 < -4 || exp10 >= prec) { use_exp = 1; prec -= 1; }
        else prec = prec - 1 - exp10;
        if (prec < 0) prec = 0;
    } else if (conv == 'e' || conv == 'E') {
        use_exp = 1;
    }
    if (use_exp) {
        exp10 = 0;
        if (v != 0.0) {
            while (v >= 10.0) { v /= 10.0; exp10++; }
            while (v < 1.0) { v *= 10.0; exp10--; }
        }
    }
    if (prec > 17) prec = 17;
    for (i = 0; i < prec; i++) scale *= 10;
    if (v >= 1e18) {
        /* বিশাল সংখ্যা: অঙ্ক আনুমানিক */
        int e = 0;
        while (v >= 1e18) { v /= 10.0; e++; }
        ip = (unsigned long long)v;
        n = utoa(ip, 10, 0, buf);
        while (e-- > 0 && n < 360) buf[n++] = '0';
        fp = 0;
    } else {
        ip = (unsigned long long)v;
        fp = (unsigned long long)((v - (double)ip) * (double)scale + 0.5);
        if (fp >= scale) { ip++; fp -= scale; }
        if (use_exp && ip >= 10) { ip /= 10; exp10++; }
        n = utoa(ip, 10, 0, buf);
    }
    if (prec > 0 || (flags & F_ALT)) {
        int k;
        buf[n++] = '.';
        k = utoa(fp, 10, 0, digs);
        for (i = k; i < prec; i++) buf[n++] = '0';
        memcpy(buf + n, digs, (size_t)k);
        n += k;
        if (prec == 0) n -= k;
    }
    if (g_mode && !(flags & F_ALT) && memchr(buf, '.', (size_t)n)) {
        while (buf[n - 1] == '0') n--;
        if (buf[n - 1] == '.') n--;
    }
    if (use_exp) {
        int e = exp10 < 0 ? -exp10 : exp10;
        buf[n++] = (conv == 'E' || conv == 'G') ? 'E' : 'e';
        buf[n++] = exp10 < 0 ? '-' : '+';
        if (e < 10) buf[n++] = '0';
        n += utoa((unsigned)e, 10, 0, buf + n);
    }
    emit_number(s, sign, buf, n, width, -1, flags);
}

static int format(struct sink *s, const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        int flags = 0, width = 0, prec = -1, lng = 0;
        const char *start;
        char digits[72];
        int nd;

        if (*fmt != '%') {
            start = fmt;
            while (fmt[1] && fmt[1] != '%') fmt++;
            emit(s, start, (size_t)(fmt - start + 1));
            continue;
        }
        fmt++;
        for (;; fmt++) {
            if (*fmt == '-') flags |= F_LEFT;
            else if (*fmt == '+') flags |= F_PLUS;
            else if (*fmt == ' ') flags |= F_SPACE;
            else if (*fmt == '#') flags |= F_ALT;
            else if (*fmt == '0') flags |= F_ZERO;
            else break;
        }
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) { flags |= F_LEFT; width = -width; }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        }
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
        }
        /* দৈর্ঘ্য: 0 = int, 1 = long, 2 = long long, -1 = short, -2 = char */
        for (;; fmt++) {
            if (*fmt == 'l') lng++;
            else if (*fmt == 'h') lng--;
            else if (*fmt == 'z' || *fmt == 't' || *fmt == 'j') lng = 1;
            else if (*fmt == 'L') lng = 2;
            else break;
        }

        switch (*fmt) {
        case 'd':
        case 'i': {
            long long v = lng >= 1 ? (lng >= 2 ? va_arg(ap, long long) : va_arg(ap, long)) : va_arg(ap, int);
            unsigned long long u;
            const char *pre = "";
            if (lng == -1) v = (short)v;
            if (lng <= -2) v = (signed char)v;
            if (v < 0) { pre = "-"; u = (unsigned long long)(-(v + 1)) + 1; }
            else { u = (unsigned long long)v; if (flags & F_PLUS) pre = "+"; else if (flags & F_SPACE) pre = " "; }
            nd = utoa(u, 10, 0, digits);
            emit_number(s, pre, digits, nd, width, prec, flags);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            unsigned long long u = lng >= 1 ? (lng >= 2 ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned long))
                                            : va_arg(ap, unsigned int);
            unsigned base = (*fmt == 'u') ? 10 : (*fmt == 'o') ? 8 : 16;
            const char *pre = "";
            if (lng == -1) u = (unsigned short)u;
            if (lng <= -2) u = (unsigned char)u;
            if ((flags & F_ALT) && u) pre = (*fmt == 'x') ? "0x" : (*fmt == 'X') ? "0X" : (*fmt == 'o') ? "0" : "";
            nd = utoa(u, base, *fmt == 'X', digits);
            emit_number(s, pre, digits, nd, width, prec, flags);
            break;
        }
        case 'p': {
            unsigned long u = (unsigned long)va_arg(ap, void *);
            nd = utoa(u, 16, 0, digits);
            emit_number(s, "0x", digits, nd, width, prec, flags);
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            if (!(flags & F_LEFT)) pad(s, ' ', width - 1);
            emit(s, &c, 1);
            if (flags & F_LEFT) pad(s, ' ', width - 1);
            break;
        }
        case 's': {
            const char *str = va_arg(ap, const char *);
            int len;
            if (!str) str = "(null)";
            len = (int)(prec >= 0 ? strnlen(str, (size_t)prec) : strlen(str));
            if (!(flags & F_LEFT)) pad(s, ' ', width - len);
            emit(s, str, (size_t)len);
            if (flags & F_LEFT) pad(s, ' ', width - len);
            break;
        }
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G': {
            double v = (lng == 2) ? (double)va_arg(ap, long double) : va_arg(ap, double);
            emit_double(s, v, *fmt == 'F' ? 'f' : *fmt, width, prec, flags);
            break;
        }
        case 'n': {
            int *p = va_arg(ap, int *);
            if (p) *p = (int)s->total;
            break;
        }
        case '%':
            emit(s, "%", 1);
            break;
        case '\0':
            return (int)s->total;
        default:
            emit(s, "%", 1);
            emit(s, fmt, 1);
            break;
        }
    }
    return (int)s->total;
}

/* ---------- আউটপুটের জায়গা ---------- */

static void put_buf(struct sink *s, const char *p, size_t n)
{
    size_t room = s->size > s->len ? s->size - s->len - 1 : 0;
    size_t k = n < room ? n : room;
    if (s->size) {
        memcpy(s->buf + s->len, p, k);
        s->len += k;
    }
}

static void put_file(struct sink *s, const char *p, size_t n)
{
    fwrite(p, 1, n, s->file);
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct sink s;
    int r;
    memset(&s, 0, sizeof(s));
    s.put = put_buf;
    s.buf = buf;
    s.size = size;
    r = format(&s, fmt, ap);
    if (size) buf[s.len] = '\0';
    return r;
}

int vsprintf(char *buf, const char *fmt, va_list ap)
{
    return vsnprintf(buf, (size_t)-1 / 2, fmt, ap);
}

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    struct sink s;
    memset(&s, 0, sizeof(s));
    s.put = put_file;
    s.file = f;
    return format(&s, fmt, ap);
}

int vprintf(const char *fmt, va_list ap)
{
    return vfprintf(stdout, fmt, ap);
}

int vdprintf(int fd, const char *fmt, va_list ap)
{
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    write(fd, buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1));
    return n;
}

int printf(const char *fmt, ...)
{ va_list ap; int r; va_start(ap, fmt); r = vfprintf(stdout, fmt, ap); va_end(ap); return r; }

int fprintf(FILE *f, const char *fmt, ...)
{ va_list ap; int r; va_start(ap, fmt); r = vfprintf(f, fmt, ap); va_end(ap); return r; }

int dprintf(int fd, const char *fmt, ...)
{ va_list ap; int r; va_start(ap, fmt); r = vdprintf(fd, fmt, ap); va_end(ap); return r; }

int sprintf(char *buf, const char *fmt, ...)
{ va_list ap; int r; va_start(ap, fmt); r = vsprintf(buf, fmt, ap); va_end(ap); return r; }

int snprintf(char *buf, size_t size, const char *fmt, ...)
{ va_list ap; int r; va_start(ap, fmt); r = vsnprintf(buf, size, fmt, ap); va_end(ap); return r; }
