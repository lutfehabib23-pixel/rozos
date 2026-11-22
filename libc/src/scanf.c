/* rozlibc — scanf পরিবার (ধাপ ১২; আগের ছোট sscanf-এর সব রূপ সহ)
 *
 * একটাই ইঞ্জিন (do_scan), ইনপুট আসে দুই জায়গা থেকে: স্ট্রিং (sscanf) বা FILE (scanf,
 * fscanf)। FILE-এর ক্ষেত্রে শেষে না-লাগা একটা অক্ষর ungetc দিয়ে ফেরত যায়।
 *
 *   ফাঁকা অক্ষর (fmt-এ)   ইনপুটের যত ফাঁকা সব বাদ
 *   %d %i %u %o %x %X     পূর্ণসংখ্যা (i: 0x/0 উপসর্গ বোঝে)   h hh l ll z j t
 *   %f %e %g %E %G %a     double (l) বা float, L = long double হিসেবে double
 *   %s                    ফাঁকা পর্যন্ত শব্দ          %c   ঠিক width টা অক্ষর (ফাঁকা বাদ দেয় না)
 *   %[abc] %[^\n] %[a-z]  এই অক্ষরগুলোর দল           %n   এ পর্যন্ত কত অক্ষর পড়া হলো
 *   %p                    ঠিকানা (হেক্স)              %%   একটা '%'
 *   %*d ...               পড়ে ফেলে দেওয়া (গোনায় আসে না)
 * ধাপ ১৬ক: %Lf = long double (আগে ভুল করে double বসাত), %ms %m[..] %mc = malloc করে
 *   বসানো (char **), %ls %lc %l[..] = UTF-8 পড়ে wchar_t-তে, %1$d ধরনের positional নয়
 * ফেরত: কয়টা মান বসানো হলো; প্রথম রূপান্তরের আগেই ইনপুট শেষ হলে EOF। */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <ctype.h>
#include <wchar.h>

struct in {
    const char *s;          /* স্ট্রিং থেকে */
    FILE *f;                /* বা ফাইল থেকে */
    long count;             /* %n-এর জন্য */
    int eof;
};

static int in_get(struct in *in)
{
    int c;
    if (in->f) {
        c = fgetc(in->f);
    } else {
        c = *in->s ? (unsigned char)*in->s++ : EOF;
    }
    if (c == EOF) in->eof = 1;
    else in->count++;
    return c;
}

static void in_unget(struct in *in, int c)
{
    if (c == EOF) return;
    in->count--;
    if (in->f) ungetc(c, in->f);
    else in->s--;
}

static void skip_space(struct in *in)
{
    int c;
    while ((c = in_get(in)) != EOF && isspace(c)) {
    }
    in_unget(in, c);
}

/* [set] পার্স: fmt '[' এর পরে; ফেরত fmt ']' এর পরে, set[256]-এ 1/0 */
static const char *parse_set(const char *fmt, unsigned char *set)
{
    int neg = 0, i;
    memset(set, 0, 256);
    if (*fmt == '^') { neg = 1; fmt++; }
    if (*fmt == ']') { set[']'] = 1; fmt++; }
    while (*fmt && *fmt != ']') {
        unsigned char a = (unsigned char)*fmt++;
        if (*fmt == '-' && fmt[1] && fmt[1] != ']') {
            unsigned char b = (unsigned char)fmt[1];
            for (i = a; i <= b; i++) set[i] = 1;
            fmt += 2;
        } else {
            set[a] = 1;
        }
    }
    if (*fmt == ']') fmt++;
    if (neg) for (i = 0; i < 256; i++) set[i] = !set[i];
    return fmt;
}

static void store_int(va_list *ap, int size, unsigned long long v)
{
    switch (size) {
    case -2: *va_arg(*ap, signed char *) = (signed char)v; break;
    case -1: *va_arg(*ap, short *) = (short)v; break;
    case 1:  *va_arg(*ap, long *) = (long)v; break;
    case 2:  *va_arg(*ap, long long *) = (long long)v; break;
    default: *va_arg(*ap, int *) = (int)v; break;
    }
}

/* ধাপ ১৬ক: ইনপুট থেকে একটা UTF-8 অক্ষর → wchar_t; ফেরত 0 = পাওয়া, -1 = শেষ/ভুল */
static int scan_wc(struct in *in, wchar_t *out)
{
    char b[4];
    int c = in_get(in), n, i;
    mbstate_t st;
    if (c == EOF) return -1;
    b[0] = (char)c;
    n = c < 0x80 ? 1 : c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
    for (i = 1; i < n; i++) {
        c = in_get(in);
        if (c == EOF) return -1;
        b[i] = (char)c;
    }
    memset(&st, 0, sizeof(st));
    if (mbrtowc(out, b, (size_t)n, &st) > (size_t)n) *out = (unsigned char)b[0];
    return 0;
}

/* set-এর মধ্যে থাকলে পড়া (ASCII-বাইরের অক্ষর: set[0x80]-এর মান মেনে); ফেরত 1/0/-1 */
static int scan_wc_if(struct in *in, wchar_t *out, const unsigned char *set)
{
    int c = in_get(in);
    if (c == EOF) return -1;
    if (c < 0x80) {
        if (!set[c]) { in_unget(in, c); return 0; }
        *out = (wchar_t)c;
        return 1;
    }
    if (!set[0x80]) { in_unget(in, c); return 0; }
    in_unget(in, c);
    return scan_wc(in, out) < 0 ? -1 : 1;
}

static int do_scan(struct in *in, const char *fmt, va_list ap0)
{
    va_list ap;
    int assigned = 0, converted = 0;
    va_copy(ap, ap0);
    while (*fmt) {
        int skip = 0, width = 0, size = 0, c, alloc = 0;
        if (isspace((unsigned char)*fmt)) {
            skip_space(in);
            fmt++;
            continue;
        }
        if (*fmt != '%') {
            c = in_get(in);
            if (c != (unsigned char)*fmt) {
                in_unget(in, c);
                break;
            }
            fmt++;
            continue;
        }
        fmt++;
        if (*fmt == '%') {
            skip_space(in);
            c = in_get(in);
            if (c != '%') { in_unget(in, c); break; }
            fmt++;
            continue;
        }
        if (*fmt == '*') { skip = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        if (*fmt == 'm') { alloc = 1; fmt++; }          /* ধাপ ১৬ক */
        for (;;) {
            if (*fmt == 'h') { size = size == -1 ? -2 : -1; fmt++; }
            else if (*fmt == 'l') { size = size == 1 ? 2 : 1; fmt++; }
            else if (*fmt == 'L' || *fmt == 'q') { size = 2; fmt++; }
            else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') { size = 1; fmt++; }
            else break;
        }
        if (*fmt == 'n') {
            if (!skip) store_int(&ap, size, (unsigned long long)in->count);
            fmt++;
            continue;
        }
        if (*fmt != 'c' && *fmt != '[') skip_space(in);
        if (*fmt == 'c' && size >= 1) {
            /* ধাপ ১৬ক: %lc — width টা UTF-8 অক্ষর → wchar_t */
            wchar_t *d = NULL, **dp = NULL;
            int k;
            if (width == 0) width = 1;
            if (!skip) {
                if (alloc) {
                    dp = va_arg(ap, wchar_t **);
                    d = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)width);
                    if (!d) break;
                } else d = va_arg(ap, wchar_t *);
            }
            for (k = 0; k < width; k++) {
                wchar_t wc;
                if (scan_wc(in, &wc) < 0) break;
                if (d) d[k] = wc;
            }
            if (k == 0) { if (dp) free(d); break; }
            if (dp) *dp = d;
            if (!skip) assigned++;
            converted++;
            fmt++;
            continue;
        }
        if (*fmt == 'c') {
            char *d = NULL, **dp = NULL;
            int k;
            if (width == 0) width = 1;
            if (!skip) {
                if (alloc) {
                    dp = va_arg(ap, char **);
                    d = (char *)malloc((size_t)width);
                    if (!d) break;
                } else d = va_arg(ap, char *);
            }
            for (k = 0; k < width; k++) {
                c = in_get(in);
                if (c == EOF) break;
                if (d) d[k] = (char)c;
            }
            if (k == 0) { if (dp) free(d); break; }
            if (dp) *dp = d;
            if (!skip) assigned++;
            converted++;
            fmt++;
            continue;
        }
        if (*fmt == 's' || *fmt == '[') {
            unsigned char set[256];
            char *d;
            int k = 0;
            if (*fmt == '[') {
                fmt = parse_set(fmt + 1, set);
            } else {
                int i;
                for (i = 0; i < 256; i++) set[i] = !isspace(i);
                set[0] = 0;
                fmt++;
            }
            if (size >= 1) {
                /* ধাপ ১৬ক: %ls / %l[..] — wchar_t-তে (set শুধু ASCII অক্ষর বিচার করে;
                 * ASCII-র বাইরের অক্ষর %ls-এ সবসময়, %l[..]-এ শুধু ^ সেট হলে নেওয়া) */
                wchar_t *wd = NULL, **wdp = NULL;
                size_t wcap = 0;
                if (!skip) {
                    if (alloc) {
                        wdp = va_arg(ap, wchar_t **);
                        wcap = 32;
                        wd = (wchar_t *)malloc(sizeof(wchar_t) * wcap);
                        if (!wd) break;
                    } else wd = va_arg(ap, wchar_t *);
                }
                for (;;) {
                    wchar_t wc;
                    int r;
                    if (width && k >= width) break;
                    r = scan_wc_if(in, &wc, set);
                    if (r <= 0) break;
                    if (wdp && (size_t)k + 2 > wcap) {
                        wchar_t *nw = (wchar_t *)realloc(wd, sizeof(wchar_t) * wcap * 2);
                        if (!nw) break;
                        wd = nw;
                        wcap *= 2;
                    }
                    if (wd) wd[k] = wc;
                    k++;
                }
                if (k == 0) { if (wdp) free(wd); break; }
                if (wd) wd[k] = 0;
                if (wdp) *wdp = wd;
                if (!skip) assigned++;
                converted++;
                continue;
            }
            if (!skip && alloc) {
                char **dp = va_arg(ap, char **);
                size_t cap = 32;
                char *ad = (char *)malloc(cap);
                if (!ad) break;
                for (;;) {
                    if (width && k >= width) break;
                    c = in_get(in);
                    if (c == EOF || !set[(unsigned char)c]) { in_unget(in, c); break; }
                    if ((size_t)k + 2 > cap) {
                        char *na = (char *)realloc(ad, cap * 2);
                        if (!na) break;
                        ad = na;
                        cap *= 2;
                    }
                    ad[k++] = (char)c;
                }
                if (k == 0) { free(ad); break; }
                ad[k] = '\0';
                *dp = ad;
                assigned++;
                converted++;
                continue;
            }
            d = skip ? NULL : va_arg(ap, char *);
            for (;;) {
                if (width && k >= width) break;
                c = in_get(in);
                if (c == EOF || !set[(unsigned char)c]) { in_unget(in, c); break; }
                if (d) d[k] = (char)c;
                k++;
            }
            if (k == 0) break;
            if (d) d[k] = '\0';
            if (!skip) assigned++;
            converted++;
            continue;
        }
        {
            /* সংখ্যা: আগে টুকরোটা বাফারে জমা, তারপর strtoull/strtod */
            char buf[128], conv = *fmt;
            int n = 0, base = 10, is_float = 0, ok;
            char *end;
            if (conv == 'x' || conv == 'X' || conv == 'p') base = 16;
            else if (conv == 'o') base = 8;
            else if (conv == 'i') base = 0;
            else if (conv == 'f' || conv == 'e' || conv == 'g' || conv == 'E' || conv == 'G' ||
                     conv == 'a' || conv == 'A' || conv == 'F') is_float = 1;
            else if (conv != 'd' && conv != 'u') break;               /* অজানা */
            if (width == 0 || width > (int)sizeof(buf) - 1) width = (int)sizeof(buf) - 1;
            while (n < width) {
                c = in_get(in);
                if (c == EOF) break;
                ok = isxdigit(c) || c == '-' || c == '+' ||
                     (is_float && (c == '.' || c == 'e' || c == 'E' || c == 'p' || c == 'P' ||
                                   c == 'n' || c == 'N' || c == 'i' || c == 'I' || c == 'a' || c == 'A' ||
                                   c == 'f' || c == 'F' || c == 't' || c == 'T' || c == 'y' || c == 'Y')) ||
                     ((c == 'x' || c == 'X') && (base == 16 || base == 0)) ||
                     /* ধাপ ১৬ক: হেক্স ভাসমান (0x1p4) — শুধু শুরুর "0"-এর পরে */
                     ((c == 'x' || c == 'X') && is_float &&
                      ((n == 1 && buf[0] == '0') || (n == 2 && buf[1] == '0' && (buf[0] == '-' || buf[0] == '+'))));
                if (ok && (c == '-' || c == '+') && n > 0 &&
                    !(is_float && (buf[n - 1] == 'e' || buf[n - 1] == 'E' || buf[n - 1] == 'p' || buf[n - 1] == 'P'))) ok = 0;
                if (ok && !is_float && base == 10 && isalpha(c)) ok = 0;
                if (ok && !is_float && base == 8 && (c == '8' || c == '9' || isalpha(c))) ok = 0;
                if (!ok) { in_unget(in, c); break; }
                buf[n++] = (char)c;
            }
            buf[n] = '\0';
            if (n == 0) break;
            if (is_float) {
                long double v = strtold(buf, &end);
                if (end == buf) break;
                if (!skip) {
                    if (size >= 2) *va_arg(ap, long double *) = v;          /* ধাপ ১৬ক: %Lf */
                    else if (size >= 1) *va_arg(ap, double *) = (double)v;
                    else *va_arg(ap, float *) = (float)v;
                }
            } else {
                unsigned long long v;
                if (buf[0] == '-') v = (unsigned long long)strtoll(buf, &end, base);
                else v = strtoull(buf, &end, base);
                if (end == buf) break;
                if (!skip) {
                    if (conv == 'p') *va_arg(ap, void **) = (void *)(uintptr_t)v;
                    else store_int(&ap, size, v);
                }
            }
            /* বাফারে বেশি পড়া অংশ (যেমন "12abc"-এর বেলায় শুধু স্ট্রিং থেকে) ফেরত */
            {
                int extra = (int)(buf + n - end);
                while (extra-- > 0) in_unget(in, (unsigned char)buf[--n]);
            }
            if (!skip) assigned++;
            converted++;
            fmt++;
        }
    }
    va_end(ap);
    if (converted == 0 && in->eof && assigned == 0) return EOF;
    return assigned;
}

int vsscanf(const char *s, const char *fmt, va_list ap)
{
    struct in in;
    in.s = s;
    in.f = NULL;
    in.count = 0;
    in.eof = 0;
    return do_scan(&in, fmt, ap);
}

int vfscanf(FILE *f, const char *fmt, va_list ap)
{
    struct in in;
    in.s = NULL;
    in.f = f;
    in.count = 0;
    in.eof = 0;
    return do_scan(&in, fmt, ap);
}

int vscanf(const char *fmt, va_list ap)
{
    fflush(stdout);                     /* প্রশ্ন ("Enter a number: ") আগে দেখা যাক */
    return vfscanf(stdin, fmt, ap);
}

int sscanf(const char *s, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = vsscanf(s, fmt, ap);
    va_end(ap);
    return r;
}

int fscanf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = vfscanf(f, fmt, ap);
    va_end(ap);
    return r;
}

int scanf(const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = vscanf(fmt, ap);
    va_end(ap);
    return r;
}
