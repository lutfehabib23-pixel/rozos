/* rozlibc — printf পরিবার (একটাই ফরম্যাট-ইঞ্জিন, আউটপুট callback দিয়ে)
 *
 * সাপোর্ট: %d %i %u %x %X %o %c %s %p %% %n, %f %F %e %g (double),
 * ফ্ল্যাগ - + space # 0, প্রস্থ ও precision (সংখ্যা বা *), দৈর্ঘ্য hh h l ll z t j L।
 * ধাপ ১৬ক: %f/%e/%g যেকোনো precision-এ নিখুঁত (long double সহ), %a %A (হেক্স ভাসমান),
 * positional (%2$s, %*1$d), %lc %ls (wide → UTF-8), %m (strerror(errno)), %b (বাইনারি),
 * %n-এর সব দৈর্ঘ্য, asprintf/vasprintf। */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <wchar.h>
#include "pthread_impl.h"

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

/* ================= ধাপ ১৬ক: নিখুঁত দশমিক রূপান্তর =================
 *
 * মান = m × 2^e (m ≤ 64 বিট, long double থেকে — double-ও long double-এ হুবহু ধরে)।
 * e ≥ 0 হলে N = m·2^e, নইলে N = m·5^(−e) আর মান = N / 10^(−e)। N-এর দশমিক অঙ্ক বড়
 * পূর্ণসংখ্যা (base 10^9) দিয়ে হুবহু বের হয়, তারপর ঠিক জায়গায় round-half-even —
 * তাই %f/%e/%g যেকোনো precision-এ নিখুঁত (glibc-র সাথে মেলে)। */

typedef uint32_t limb_t;
#define LIMB_BASE 1000000000u

static int big_mul(limb_t *a, int n, uint32_t mul)
{
    uint64_t carry = 0;
    int i;
    for (i = 0; i < n; i++) {
        uint64_t t = (uint64_t)a[i] * mul + carry;
        a[i] = (limb_t)(t % LIMB_BASE);
        carry = t / LIMB_BASE;
    }
    while (carry) {
        a[n++] = (limb_t)(carry % LIMB_BASE);
        carry /= LIMB_BASE;
    }
    return n;
}

struct fpparts {
    int neg, cls;                /* cls: 0 = সংখ্যা, 1 = inf, 2 = nan */
    uint64_t m;
    int e;
};

static void fp_split(long double v, struct fpparts *p)
{
    union { long double v; struct { uint64_t m; uint16_t se; } b; } u;
    int ex;
    memset(&u, 0, sizeof(u));
    u.v = v;
    p->neg = (u.b.se >> 15) & 1;
    ex = u.b.se & 0x7fff;
    p->m = u.b.m;
    p->cls = 0;
    if (ex == 0x7fff) {
        p->cls = (p->m << 1) ? 2 : 1;
        return;
    }
    p->e = ex ? ex - 16383 - 63 : 1 - 16383 - 63;
}

/* N-এর অঙ্ক dig-এ (malloc করা বা stack-এর), ফেরত অঙ্কের সংখ্যা; *dp = দশমিক বিন্দুর আগে কত অঙ্ক */
static int fp_digits(uint64_t m, int e, char **dig, int *dp, char *stackbuf, size_t stacksz,
                     limb_t *stacklimbs, size_t nstack)
{
    int k = 0, n, i, nd, est;
    limb_t *a;
    char *d;
    if (m == 0) {
        *dig = stackbuf;
        stackbuf[0] = '0';
        *dp = 1;
        return 1;
    }
    while (!(m & 1) && e < 0) { m >>= 1; e++; }
    est = e >= 0 ? (int)((64 + e) * 0.30103) + 2 : 21 + (int)((-e) * 0.69897) + 2;
    if ((size_t)(est / 9 + 4) <= nstack) a = stacklimbs;
    else if (!(a = (limb_t *)malloc(sizeof(limb_t) * (size_t)(est / 9 + 4)))) return -1;
    if ((size_t)est + 12 <= stacksz) d = stackbuf;
    else if (!(d = (char *)malloc((size_t)est + 12))) { if (a != stacklimbs) free(a); return -1; }

    a[0] = (limb_t)(m % LIMB_BASE);
    a[1] = (limb_t)((m / LIMB_BASE) % LIMB_BASE);
    a[2] = (limb_t)(m / LIMB_BASE / LIMB_BASE);
    n = a[2] ? 3 : a[1] ? 2 : 1;
    if (e >= 0) {
        int t = e;
        while (t >= 29) { n = big_mul(a, n, 1u << 29); t -= 29; }
        if (t) n = big_mul(a, n, 1u << t);
    } else {
        int t = -e;
        k = t;
        while (t >= 13) { n = big_mul(a, n, 1220703125u); t -= 13; }   /* 5^13 */
        if (t) { uint32_t p5 = 1; while (t--) p5 *= 5; n = big_mul(a, n, p5); }
    }
    /* অঙ্কে লেখা: সবচেয়ে বড় limb শূন্য ছাড়া, বাকিগুলো ৯ অঙ্ক করে */
    nd = 0;
    {
        char tmp[12];
        int tl = 0;
        limb_t top = a[n - 1];
        do { tmp[tl++] = (char)('0' + top % 10); top /= 10; } while (top);
        while (tl) d[nd++] = tmp[--tl];
        for (i = n - 2; i >= 0; i--) {
            limb_t v = a[i];
            int j;
            for (j = 8; j >= 0; j--) { d[nd + j] = (char)('0' + v % 10); v /= 10; }
            nd += 9;
        }
    }
    if (a != stacklimbs) free(a);
    *dig = d;
    *dp = nd - k;
    return nd;
}

/* প্রথম cut অঙ্ক রেখে round-half-even; ফেরত নতুন অঙ্কের সংখ্যা (cut বা ০), *dp দরকারে বাড়ে।
 * cut ≤ 0 হলে ফল শূন্য বা এক অঙ্ক "1" (তখন dp আগের চেয়ে ১ বেশি জায়গায়)। */
static int fp_round(char *d, int nd, int cut, int *dp)
{
    int up = 0, i;
    if (cut >= nd) return nd;
    if (cut < 0) return 0;
    if (d[cut] > '5') up = 1;
    else if (d[cut] == '5') {
        for (i = cut + 1; i < nd; i++) if (d[i] != '0') { up = 1; break; }
        if (!up) up = cut > 0 && ((d[cut - 1] - '0') & 1);
    }
    if (!up) return cut;
    for (i = cut - 1; i >= 0; i--) {
        if (d[i] != '9') { d[i]++; return cut; }
        d[i] = '0';
    }
    /* সব ৯ ছিল (বা cut == 0): সামনে একটা ১ */
    d[0] = '1';
    for (i = 1; i < cut; i++) d[i] = '0';
    (*dp)++;
    return cut ? cut : 1;
}

/* নিখুঁত %f %e %g; ব্যর্থ (মেমরি নেই) হলে 0 ফেরত — তখন পুরনো emit_double */
static int emit_fp_exact(struct sink *s, long double v, char conv, int width, int prec, int flags)
{
    struct fpparts p;
    char dbuf[1200], *d, obuf[600], *o;
    limb_t limbs[150];
    const char *sign;
    int nd, dp, upper = (conv == 'E' || conv == 'G' || conv == 'F'), i, n = 0;
    int use_exp = 0, g_mode = 0, keep;
    size_t need;
    char lc = (char)(conv | 0x20);

    fp_split(v, &p);
    sign = p.neg ? "-" : (flags & F_PLUS) ? "+" : (flags & F_SPACE) ? " " : "";
    if (p.cls) {
        const char *t = p.cls == 1 ? (upper ? "INF" : "inf") : (upper ? "NAN" : "nan");
        if (p.cls == 2) sign = (flags & F_PLUS) ? "+" : (flags & F_SPACE) ? " " : "";
        emit_number(s, sign, t, 3, width, -1, flags & ~F_ZERO);
        return 1;
    }
    if (prec < 0) prec = 6;
    nd = fp_digits(p.m, p.e, &d, &dp, dbuf, sizeof(dbuf), limbs, sizeof(limbs) / sizeof(limbs[0]));
    if (nd < 0) return 0;
    if (p.m == 0) dp = 1;

    if (lc == 'g') {
        int P = prec ? prec : 1, X, dp2 = dp, nd2;
        g_mode = 1;
        /* P অঙ্কে গোল করলে সূচক কত হবে — অনুলিপিতে দেখা (d বদলায়, তাই আসলটায় পরে আবার গোল) */
        if (p.m == 0) X = 0;
        else {
            /* শুধু প্রথম P+1 অঙ্ক লাগে — বাকিগুলো শূন্য কিনা সেটাই জানা দরকার */
            char cp[64], *cpp = cp;
            int ncp = nd < P + 2 ? nd : P + 2;
            if (ncp + 1 > (int)sizeof(cp) && !(cpp = (char *)malloc((size_t)ncp + 1))) {
                if (d != dbuf) free(d);
                return 0;
            }
            memcpy(cpp, d, (size_t)ncp);
            if (ncp < nd) {
                for (i = ncp; i < nd; i++) if (d[i] != '0') break;
                if (i < nd && cpp[ncp - 1] == '0') cpp[ncp - 1] = '1';   /* "আরও কিছু আছে" চিহ্ন */
            }
            nd2 = fp_round(cpp, ncp, P, &dp2);
            (void)nd2;
            if (cpp != cp) free(cpp);
            X = dp2 - 1;
        }
        if (P > X && X >= -4) { lc = 'f'; prec = P - 1 - X; }
        else { lc = 'e'; prec = P - 1; }
    }
    if (lc == 'e') use_exp = 1;

    if (use_exp) {
        keep = p.m == 0 ? 1 : fp_round(d, nd, prec + 1, &dp);
        if (keep == 0) keep = 1;
    } else {
        keep = fp_round(d, nd, dp + prec, &dp);
        if (dp + prec < 0) keep = 0;
    }
    /* আউটপুটের মাপ */
    need = (size_t)(dp > 0 ? dp : 1) + (size_t)prec + 16;
    if (use_exp) need = (size_t)prec + 24;
    o = need <= sizeof(obuf) ? obuf : (char *)malloc(need);
    if (!o) { if (d != dbuf) free(d); return 0; }

    if (use_exp) {
        int ex = p.m == 0 ? 0 : dp - 1, ae;
        char eb[8];
        int el = 0;
        o[n++] = d[0];
        if (prec > 0 || (flags & F_ALT)) o[n++] = '.';
        for (i = 1; i <= prec; i++) o[n++] = i < keep ? d[i] : '0';
        if (g_mode && !(flags & F_ALT)) {
            while (n > 1 && o[n - 1] == '0') n--;
            if (o[n - 1] == '.') n--;
        }
        o[n++] = upper ? 'E' : 'e';
        o[n++] = ex < 0 ? '-' : '+';
        ae = ex < 0 ? -ex : ex;
        do { eb[el++] = (char)('0' + ae % 10); ae /= 10; } while (ae);
        if (el < 2) eb[el++] = '0';
        while (el) o[n++] = eb[--el];
    } else {
        if (dp > 0) {
            for (i = 0; i < dp; i++) o[n++] = i < keep ? d[i] : '0';
        } else {
            o[n++] = '0';
        }
        if (prec > 0 || (flags & F_ALT)) o[n++] = '.';
        for (i = 0; i < prec; i++) {
            int idx = dp + i;
            o[n++] = (idx >= 0 && idx < keep) ? d[idx] : '0';
        }
        if (g_mode && !(flags & F_ALT) && memchr(o, '.', (size_t)n)) {
            while (o[n - 1] == '0') n--;
            if (o[n - 1] == '.') n--;
        }
    }
    emit_number(s, sign, o, n, width, -1, flags);
    if (o != obuf) free(o);
    if (d != dbuf) free(d);
    return 1;
}

/* %a / %A: 0x1.hhhhp±d (long double-এর ৬৪ বিট ম্যান্টিসা থেকে, স্বাভাবিক রূপে) */
static void emit_fp_hex(struct sink *s, long double v, char conv, int width, int prec, int flags)
{
    struct fpparts p;
    char o[64], pre[4];
    const char *hex = conv == 'A' ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0, i, ex, nd, ae, el = 0;
    uint64_t m, frac;
    char eb[8];
    unsigned lead;

    fp_split(v, &p);
    if (p.cls) {
        emit_fp_exact(s, v, conv == 'A' ? 'E' : 'e', width, prec, flags);
        return;
    }
    pre[0] = '\0';
    if (p.neg) strcpy(pre, "-");
    else if (flags & F_PLUS) strcpy(pre, "+");
    else if (flags & F_SPACE) strcpy(pre, " ");
    strcat(pre, conv == 'A' ? "0X" : "0x");
    m = p.m;
    if (m == 0) {
        ex = 0;
        lead = 0;
        frac = 0;
    } else {
        ex = p.e + 63;
        while (!(m >> 63)) { m <<= 1; ex--; }
        lead = 1;
        frac = m << 1;                    /* ৬৩টা ভগ্নাংশ বিট, ওপরে বসানো (৬৪ বিটে) */
    }
    /* ভগ্নাংশ: ১৬টা হেক্স অঙ্ক (শেষ বিট শূন্য); precision থাকলে round-half-even
     * (গোল করে 0x1.f… → 0x2… হলে সেভাবেই লেখা — glibc-র মতো) */
    nd = 16;
    if (prec >= 0 && prec < 16) {
        uint64_t keepv, rest, half;
        int up;
        if (prec == 0) { keepv = 0; rest = frac; half = 1ULL << 63; }
        else {
            keepv = frac >> (64 - 4 * prec);
            rest = frac & ((1ULL << (64 - 4 * prec)) - 1);
            half = 1ULL << (63 - 4 * prec);
        }
        up = rest > half || (rest == half && ((prec ? keepv : lead) & 1));
        if (up) {
            keepv++;
            if (prec == 0 || (keepv >> (4 * prec))) { keepv = 0; lead++; }
        }
        frac = prec ? keepv << (64 - 4 * prec) : 0;
        nd = prec;
    } else if (prec < 0) {
        while (nd > 0 && !((frac >> (64 - 4 * nd)) & 0xf)) nd--;
    }
    o[n++] = hex[lead & 0xf];
    if (nd > 0 || prec > 0 || (flags & F_ALT)) o[n++] = '.';
    for (i = 0; i < nd; i++) o[n++] = hex[(frac >> (60 - 4 * i)) & 0xf];
    for (; prec > 16 && i < prec; i++) o[n++] = '0';
    o[n++] = conv == 'A' ? 'P' : 'p';
    o[n++] = ex < 0 ? '-' : '+';
    ae = ex < 0 ? -ex : ex;
    do { eb[el++] = (char)('0' + ae % 10); ae /= 10; } while (ae);
    while (el) o[n++] = eb[--el];
    emit_number(s, pre, o, n, width, -1, flags);
}

/* ================= ফরম্যাট পার্সার (ধাপ ১৬ক: positional %1$d, *2$) ================= */

#define MAX_POS 32
enum { A_NONE, A_INT, A_LONG, A_LLONG, A_DBL, A_LDBL, A_PTR };

union argval {
    long long i;
    double d;
    long double ld;
    void *p;
};

struct spec {
    int argpos;                  /* ১ থেকে; ০ = ক্রমানুসারে */
    int flags, width, prec;
    int wstar, wpos, pstar, ppos;
    int lng, isL;
    char conv;
};

/* fmt '%'-এর পরে; ফেরত রূপান্তর-অক্ষরের ঠিকানা (শেষে '\0' হলে সেখানেই) */
static const char *parse_spec(const char *fmt, struct spec *sp)
{
    memset(sp, 0, sizeof(*sp));
    sp->prec = -1;
    {
        const char *q = fmt;
        int v = 0;
        while (*q >= '0' && *q <= '9') v = v * 10 + (*q++ - '0');
        if (*q == '$' && v > 0) { sp->argpos = v; fmt = q + 1; }
    }
    for (;; fmt++) {
        if (*fmt == '-') sp->flags |= F_LEFT;
        else if (*fmt == '+') sp->flags |= F_PLUS;
        else if (*fmt == ' ') sp->flags |= F_SPACE;
        else if (*fmt == '#') sp->flags |= F_ALT;
        else if (*fmt == '0') sp->flags |= F_ZERO;
        else if (*fmt == '\'' || *fmt == 'I') { /* হাজারের কমা: C locale-এ নেই */ }
        else break;
    }
    if (*fmt == '*') {
        const char *q = ++fmt;
        int v = 0;
        sp->wstar = 1;
        while (*q >= '0' && *q <= '9') v = v * 10 + (*q++ - '0');
        if (*q == '$' && v > 0) { sp->wpos = v; fmt = q + 1; }
    } else {
        while (*fmt >= '0' && *fmt <= '9') sp->width = sp->width * 10 + (*fmt++ - '0');
    }
    if (*fmt == '.') {
        fmt++;
        sp->prec = 0;
        if (*fmt == '*') {
            const char *q = ++fmt;
            int v = 0;
            sp->pstar = 1;
            while (*q >= '0' && *q <= '9') v = v * 10 + (*q++ - '0');
            if (*q == '$' && v > 0) { sp->ppos = v; fmt = q + 1; }
        } else {
            while (*fmt >= '0' && *fmt <= '9') sp->prec = sp->prec * 10 + (*fmt++ - '0');
        }
    }
    /* দৈর্ঘ্য: 0 = int, 1 = long, 2 = long long, -1 = short, -2 = char */
    for (;; fmt++) {
        if (*fmt == 'l') sp->lng++;
        else if (*fmt == 'h') sp->lng--;
        else if (*fmt == 'z' || *fmt == 't' || *fmt == 'j') sp->lng = 1;
        else if (*fmt == 'L' || *fmt == 'q') { sp->lng = 2; sp->isL = 1; }
        else break;
    }
    if (sp->lng > 2) sp->lng = 2;
    sp->conv = *fmt;
    return fmt;
}

static int arg_type(const struct spec *sp)
{
    switch (sp->conv) {
    case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'b': case 'B':
        return sp->lng >= 2 ? A_LLONG : sp->lng == 1 ? A_LONG : A_INT;
    case 'c': case 'C':
        return A_INT;
    case 's': case 'S': case 'p': case 'n':
        return A_PTR;
    case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
        return sp->isL ? A_LDBL : A_DBL;
    }
    return A_NONE;
}

/* positional ফরম্যাট হলে সব আর্গুমেন্ট আগেই ধরন অনুযায়ী তুলে রাখা; ফেরত 1 = positional */
static int prescan(const char *fmt, va_list ap0, union argval *vals)
{
    unsigned char types[MAX_POS];
    int maxpos = 0, any = 0, i;
    va_list ap;
    struct spec sp;
    memset(types, 0, sizeof(types));
    for (; *fmt; fmt++) {
        if (*fmt != '%') continue;
        if (fmt[1] == '%') { fmt++; continue; }
        fmt = parse_spec(fmt + 1, &sp);
        if (!sp.argpos) {
            if (!*fmt) break;
            continue;
        }
        any = 1;
        if (sp.argpos <= MAX_POS) {
            int t = arg_type(&sp);
            if (t) types[sp.argpos - 1] = (unsigned char)t;
            if (sp.argpos > maxpos && t) maxpos = sp.argpos;
        }
        if (sp.wpos && sp.wpos <= MAX_POS) { types[sp.wpos - 1] = A_INT; if (sp.wpos > maxpos) maxpos = sp.wpos; }
        if (sp.ppos && sp.ppos <= MAX_POS) { types[sp.ppos - 1] = A_INT; if (sp.ppos > maxpos) maxpos = sp.ppos; }
        if (!*fmt) break;
    }
    if (!any) return 0;
    va_copy(ap, ap0);
    for (i = 0; i < maxpos; i++) {
        switch (types[i]) {
        case A_LONG:  vals[i].i = va_arg(ap, long); break;
        case A_LLONG: vals[i].i = va_arg(ap, long long); break;
        case A_DBL:   vals[i].d = va_arg(ap, double); break;
        case A_LDBL:  vals[i].ld = va_arg(ap, long double); break;
        case A_PTR:   vals[i].p = va_arg(ap, void *); break;
        default:      vals[i].i = va_arg(ap, int); break;     /* A_INT বা ফাঁক */
        }
    }
    va_end(ap);
    return 1;
}

static int format(struct sink *s, const char *fmt, va_list ap0)
{
    union argval vals[MAX_POS];
    int positional;
    va_list ap;
    int saved_errno = errno;

    positional = prescan(fmt, ap0, vals);
    va_copy(ap, ap0);
    for (; *fmt; fmt++) {
        struct spec sp;
        const char *start;
        char digits[72];
        int nd, flags, width, prec, lng;
        union argval av;
        int t;

        if (*fmt != '%') {
            start = fmt;
            while (fmt[1] && fmt[1] != '%') fmt++;
            emit(s, start, (size_t)(fmt - start + 1));
            continue;
        }
        if (fmt[1] == '%') {
            emit(s, "%", 1);
            fmt++;
            continue;
        }
        fmt = parse_spec(fmt + 1, &sp);
        if (!*fmt) break;
        flags = sp.flags;
        width = sp.width;
        prec = sp.prec;
        lng = sp.lng;
        if (sp.wstar) {
            width = positional ? (sp.wpos && sp.wpos <= MAX_POS ? (int)vals[sp.wpos - 1].i : 0) : va_arg(ap, int);
            if (width < 0) { flags |= F_LEFT; width = -width; }
        }
        if (sp.pstar) {
            prec = positional ? (sp.ppos && sp.ppos <= MAX_POS ? (int)vals[sp.ppos - 1].i : -1) : va_arg(ap, int);
            if (prec < 0) prec = -1;
        }
        t = arg_type(&sp);
        memset(&av, 0, sizeof(av));
        if (t) {
            if (positional) {
                if (sp.argpos >= 1 && sp.argpos <= MAX_POS) av = vals[sp.argpos - 1];
            } else {
                switch (t) {
                case A_LONG:  av.i = va_arg(ap, long); break;
                case A_LLONG: av.i = va_arg(ap, long long); break;
                case A_DBL:   av.d = va_arg(ap, double); break;
                case A_LDBL:  av.ld = va_arg(ap, long double); break;
                case A_PTR:   av.p = va_arg(ap, void *); break;
                default:      av.i = va_arg(ap, int); break;
                }
            }
        }

        switch (sp.conv) {
        case 'd':
        case 'i': {
            long long v = t == A_INT ? (long long)(int)av.i : av.i;
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
        case 'o':
        case 'b':
        case 'B': {
            unsigned long long u = t == A_INT ? (unsigned long long)(unsigned int)av.i : (unsigned long long)av.i;
            unsigned base = (sp.conv == 'u') ? 10 : (sp.conv == 'o') ? 8 : (sp.conv == 'b' || sp.conv == 'B') ? 2 : 16;
            const char *pre = "";
            if (lng == -1) u = (unsigned short)u;
            if (lng <= -2) u = (unsigned char)u;
            if ((flags & F_ALT) && u) {
                pre = (sp.conv == 'x') ? "0x" : (sp.conv == 'X') ? "0X" : (sp.conv == 'b') ? "0b" :
                      (sp.conv == 'B') ? "0B" : "";
            }
            nd = utoa(u, base, sp.conv == 'X', digits);
            if ((flags & F_ALT) && sp.conv == 'o' && (prec <= nd) && digits[0] != '0') {
                memmove(digits + 1, digits, (size_t)nd);
                digits[0] = '0';
                nd++;
            }
            emit_number(s, pre, digits, nd, width, prec, flags);
            break;
        }
        case 'p': {
            unsigned long u = (unsigned long)av.p;
            nd = utoa(u, 16, 0, digits);
            emit_number(s, "0x", digits, nd, width, prec, flags);
            break;
        }
        case 'c':
        case 'C': {
            char mb[8];
            int len = 1;
            if (lng >= 1 || sp.conv == 'C') {
                size_t r = wcrtomb(mb, (wchar_t)av.i, NULL);
                len = r == (size_t)-1 ? 0 : (int)r;
            } else {
                mb[0] = (char)av.i;
            }
            if (!(flags & F_LEFT)) pad(s, ' ', width - len);
            emit(s, mb, (size_t)len);
            if (flags & F_LEFT) pad(s, ' ', width - len);
            break;
        }
        case 's':
        case 'S': {
            if (lng >= 1 || sp.conv == 'S') {
                /* wide স্ট্রিং → UTF-8; precision = সর্বোচ্চ বাইট (অক্ষর ভাঙে না) */
                const wchar_t *ws = (const wchar_t *)av.p;
                int len = 0, k;
                char mb[8];
                if (!ws) ws = L"(null)";
                for (k = 0; ws[k]; k++) {
                    size_t r = wcrtomb(mb, ws[k], NULL);
                    if (r == (size_t)-1) break;
                    if (prec >= 0 && len + (int)r > prec) break;
                    len += (int)r;
                }
                if (!(flags & F_LEFT)) pad(s, ' ', width - len);
                {
                    int out = 0;
                    for (k = 0; ws[k] && out < len; k++) {
                        size_t r = wcrtomb(mb, ws[k], NULL);
                        if (r == (size_t)-1) break;
                        emit(s, mb, r);
                        out += (int)r;
                    }
                }
                if (flags & F_LEFT) pad(s, ' ', width - len);
            } else {
                const char *str = (const char *)av.p;
                int len;
                if (!str) str = "(null)";
                len = (int)(prec >= 0 ? strnlen(str, (size_t)prec) : strlen(str));
                if (!(flags & F_LEFT)) pad(s, ' ', width - len);
                emit(s, str, (size_t)len);
                if (flags & F_LEFT) pad(s, ' ', width - len);
            }
            break;
        }
        case 'm': {
            const char *str = strerror(saved_errno);
            int len = (int)(prec >= 0 ? strnlen(str, (size_t)prec) : strlen(str));
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
            long double v = t == A_LDBL ? av.ld : (long double)av.d;
            if (!emit_fp_exact(s, v, sp.conv, width, prec, flags)) {
                emit_double(s, (double)v, sp.conv == 'F' ? 'f' : sp.conv, width, prec, flags);
            }
            break;
        }
        case 'a':
        case 'A': {
            long double v = t == A_LDBL ? av.ld : (long double)av.d;
            emit_fp_hex(s, v, sp.conv, width, prec, flags);
            break;
        }
        case 'n': {
            void *p = av.p;
            if (p) {
                if (lng >= 2) *(long long *)p = (long long)s->total;
                else if (lng == 1) *(long *)p = (long)s->total;
                else if (lng == -1) *(short *)p = (short)s->total;
                else if (lng <= -2) *(signed char *)p = (signed char)s->total;
                else *(int *)p = (int)s->total;
            }
            break;
        }
        default:
            emit(s, "%", 1);
            emit(s, fmt, 1);
            break;
        }
    }
    va_end(ap);
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

/* ধাপ ১৬খ-২: পুরো printf একবারে (থ্রেডদের লেখা এক লাইনের মাঝে মিশে না যায়) */
extern struct __roz_rlock __roz_stdio_lock;

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    struct sink s;
    int r;
    memset(&s, 0, sizeof(s));
    s.put = put_file;
    s.file = f;
    __roz_rlock_lock(&__roz_stdio_lock);
    r = format(&s, fmt, ap);
    __roz_rlock_unlock(&__roz_stdio_lock);
    return r;
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

/* ---------- ধাপ ১৬ক: asprintf ---------- */

int vasprintf(char **out, const char *fmt, va_list ap)
{
    va_list ap2;
    int n;
    char *buf;
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0) { *out = NULL; return -1; }
    buf = (char *)malloc((size_t)n + 1);
    if (!buf) { *out = NULL; return -1; }
    vsnprintf(buf, (size_t)n + 1, fmt, ap);
    *out = buf;
    return n;
}

int asprintf(char **out, const char *fmt, ...)
{ va_list ap; int r; va_start(ap, fmt); r = vasprintf(out, fmt, ap); va_end(ap); return r; }
