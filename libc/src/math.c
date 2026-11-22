/* rozlibc — math.h ও strtod
 *
 * বেশিরভাগ ফাংশন x87 FPU-র নিজস্ব নির্দেশ দিয়ে (fsin, fcos, fpatan, fyl2x, f2xm1,
 * fscale, fsqrt, frndint, fprem) — ছোট কোড, 80-বিট ভেতরের নির্ভুলতা।
 * x87 ও SSE অবস্থা কার্নেল প্রতিটি থ্রেডের জন্য আলাদা রাখে (fxsave)। */

#include <math.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>

typedef long double ld;

static ld x87_1(const char *op, ld x);

int __rozlibc_signbit(double x)
{
    unsigned long bits;
    memcpy(&bits, &x, sizeof(bits));
    return (int)(bits >> 63);
}

double fabs(double x) { return x < 0 ? -x : (x == 0 ? 0.0 : x); }
float  fabsf(float x) { return (float)fabs(x); }
ld     fabsl(ld x) { return x < 0 ? -x : x; }

double copysign(double x, double y)
{
    x = fabs(x);
    return __rozlibc_signbit(y) ? -x : x;
}

double sqrt(double x)
{
    ld r;
    __asm__("fsqrt" : "=t"(r) : "0"((ld)x));
    return (double)r;
}
float sqrtf(float x) { return (float)sqrt(x); }

/* rounding mode বদলে frndint: mode 0 = nearest, 1 = নিচে, 2 = ওপরে, 3 = শূন্যের দিকে */
static double round_mode(double x, unsigned mode)
{
    unsigned short cw, ncw;
    ld r;
    if (isnan(x) || isinf(x)) return x;
    __asm__ __volatile__("fnstcw %0" : "=m"(cw));
    ncw = (unsigned short)((cw & ~0x0C00) | (mode << 10));
    __asm__ __volatile__("fldcw %0" : : "m"(ncw));
    __asm__ __volatile__("frndint" : "=t"(r) : "0"((ld)x));
    __asm__ __volatile__("fldcw %0" : : "m"(cw));
    return (double)r;
}

double floor(double x) { return round_mode(x, 1); }
double ceil(double x)  { return round_mode(x, 2); }
double trunc(double x) { return round_mode(x, 3); }
float  floorf(float x) { return (float)floor(x); }
float  ceilf(float x)  { return (float)ceil(x); }

double round(double x)
{
    double t = trunc(x);
    if (fabs(x - t) >= 0.5) t += x < 0 ? -1.0 : 1.0;
    return t;
}

double fmod(double x, double y)
{
    ld r;
    unsigned short sw;
    if (y == 0.0 || isnan(x) || isnan(y) || isinf(x)) return NAN;
    r = x;
    do {
        __asm__ __volatile__("fprem; fnstsw %%ax" : "=t"(r), "=a"(sw) : "0"(r), "u"((ld)y));
    } while (sw & 0x400);
    return (double)r;
}

ld ldexpl(ld x, int e)
{
    ld r;
    __asm__("fscale" : "=t"(r) : "0"(x), "u"((ld)e));
    return r;
}

double ldexp(double x, int e) { return (double)ldexpl(x, e); }
float  ldexpf(float x, int e) { return (float)ldexpl(x, e); }

ld frexpl(ld x, int *e)
{
    int k = 0;
    if (x == 0 || isnan(x) || isinf(x)) { *e = 0; return x; }
    while (fabsl(x) >= 1.0L) { x /= 2; k++; }
    while (fabsl(x) < 0.5L) { x *= 2; k--; }
    *e = k;
    return x;
}

double frexp(double x, int *e) { return (double)frexpl(x, e); }

double modf(double x, double *ip)
{
    double t = trunc(x);
    *ip = t;
    return x - t;
}

/* 2^x (যেকোনো x): পূর্ণসংখ্যা অংশ fscale, ভগ্নাংশ f2xm1 */
static ld exp2l_(ld x)
{
    ld i, f, r;
    if (isnan(x)) return x;
    if (x > 16384) return HUGE_VALL;
    if (x < -16450) return 0;
    i = (ld)round_mode((double)x, 0);
    f = x - i;
    __asm__("f2xm1" : "=t"(r) : "0"(f));
    r += 1;
    return ldexpl(r, (int)i);
}

/* y * log2(x) */
static ld yl2x(ld y, ld x)
{
    ld r;
    __asm__("fyl2x" : "=t"(r) : "0"(x), "u"(y) : "st(1)");
    return r;
}

double exp2(double x) { return (double)exp2l_(x); }
double exp(double x)  { return (double)exp2l_((ld)x * 1.4426950408889634073599246810019L); }

double log2(double x)
{
    if (x < 0 || isnan(x)) return NAN;
    if (x == 0) return -HUGE_VAL;
    return (double)yl2x(1.0L, x);
}

double log(double x)   { return x <= 0 ? (x == 0 ? -HUGE_VAL : NAN) : (double)yl2x(0.69314718055994530941723212145818L, x); }
double log10(double x) { return x <= 0 ? (x == 0 ? -HUGE_VAL : NAN) : (double)yl2x(0.30102999566398119521373889472449L, x); }

/* ধাপ ১৬ক: বিশেষ ক্ষেত্র (inf, ±0, −1, NaN, ঋণাত্মক ভিত্তি) C99 মেনে libm.c-র powl-এ;
 * সাধারণ ক্ষেত্রের হিসাব আগের মতোই 2^(y·log2 x), ছোট পূর্ণসংখ্যা ঘাত সরাসরি গুণ */
double pow(double x, double y)
{
    return (double)powl((ld)x, (ld)y);
}

static ld x87_1(const char *op, ld x)
{
    ld r;
    if (op[1] == 's') __asm__("fsin" : "=t"(r) : "0"(x));
    else __asm__("fcos" : "=t"(r) : "0"(x));
    return r;
}

/* fsin/fcos শুধু |x| < 2^63-এ; বড় হলে আগে 2π দিয়ে ভাগশেষ */
static ld reduce(double x)
{
    if (fabs(x) > 1e15) return (ld)fmod(x, 6.283185307179586476925286766559);
    return x;
}

double sin(double x) { return (double)x87_1("fsin", reduce(x)); }
double cos(double x) { return (double)x87_1("fcos", reduce(x)); }
double tan(double x) { ld r = reduce(x); return (double)(x87_1("fsin", r) / x87_1("fcos", r)); }

double atan2(double y, double x)
{
    ld r;
    __asm__("fpatan" : "=t"(r) : "0"((ld)x), "u"((ld)y) : "st(1)");
    return (double)r;
}

double atan(double x) { return atan2(x, 1.0); }
double asin(double x) { return (x < -1 || x > 1) ? NAN : atan2(x, sqrt((1 - x) * (1 + x))); }
double acos(double x) { return (x < -1 || x > 1) ? NAN : atan2(sqrt((1 - x) * (1 + x)), x); }
double sinh(double x) { double e = exp(x); return (e - 1 / e) / 2; }
double cosh(double x) { double e = exp(x); return (e + 1 / e) / 2; }
double tanh(double x)
{
    double e;
    if (x > 20) return 1.0;
    if (x < -20) return -1.0;
    e = exp(2 * x);
    return (e - 1) / (e + 1);
}
double hypot(double x, double y) { return sqrt(x * x + y * y); }

/* ---------- strtod ----------
 * দশমিক: ১৯টা গুরুত্বপূর্ণ অঙ্ক পর্যন্ত 64-বিট পূর্ণসংখ্যায় জমিয়ে, তারপর 10-এর ঘাত
 * long double (80-বিট) দিয়ে গুণ/ভাগ — double-এর জন্য প্রায় সবসময় সঠিকভাবে গোল হয়।
 * সাথে হেক্স (0x1.8p3), inf, nan। */

static ld pow10l_(int e)
{
    ld r = 1, b = 10;
    int neg = e < 0;
    unsigned n = (unsigned)(neg ? -e : e);
    while (n) {
        if (n & 1) r *= b;
        b *= b;
        n >>= 1;
    }
    return neg ? 1 / r : r;
}

ld strtold(const char *s, char **end)
{
    const char *p = s;
    unsigned long mant = 0;
    int neg = 0, digits = 0, exp10 = 0, any = 0;
    ld r;

    while (isspace((unsigned char)*p)) p++;
    if (*p == '+' || *p == '-') neg = (*p++ == '-');

    if (!strncasecmp(p, "inf", 3)) {
        p += 3;
        if (!strncasecmp(p, "inity", 5)) p += 5;
        if (end) *end = (char *)p;
        return neg ? -HUGE_VALL : HUGE_VALL;
    }
    if (!strncasecmp(p, "nan", 3)) {
        p += 3;
        if (end) *end = (char *)p;
        return NAN;
    }

    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        int exp2 = 0, after = 0;
        p += 2;
        for (;; p++) {
            int d;
            if (*p == '.' && !after) { after = 1; continue; }
            if (isdigit((unsigned char)*p)) d = *p - '0';
            else if (isxdigit((unsigned char)*p)) d = tolower((unsigned char)*p) - 'a' + 10;
            else break;
            any = 1;
            if (mant >> 60) { if (!after) exp2 += 4; continue; }
            mant = mant * 16 + (unsigned)d;
            if (after) exp2 -= 4;
        }
        if (any && (*p == 'p' || *p == 'P')) {
            char *e2;
            long x = strtol(p + 1, &e2, 10);
            if (e2 != p + 1) { exp2 += (int)x; p = e2; }
        }
        if (end) *end = (char *)(any ? p : s);
        r = ldexpl((ld)mant, exp2);
        return neg ? -r : r;
    }

    for (; isdigit((unsigned char)*p); p++) {
        any = 1;
        if (digits < 19) { mant = mant * 10 + (unsigned)(*p - '0'); if (mant) digits++; }
        else exp10++;
    }
    if (*p == '.') {
        p++;
        for (; isdigit((unsigned char)*p); p++) {
            any = 1;
            if (digits < 19) { mant = mant * 10 + (unsigned)(*p - '0'); exp10--; if (mant) digits++; }
        }
    }
    if (!any) {
        if (end) *end = (char *)s;
        return 0;
    }
    if (*p == 'e' || *p == 'E') {
        char *e2;
        long x = strtol(p + 1, &e2, 10);
        if (e2 != p + 1) {
            if (x > 100000) x = 100000;
            if (x < -100000) x = -100000;
            exp10 += (int)x;
            p = e2;
        }
    }
    if (end) *end = (char *)p;
    r = (ld)mant;
    if (mant != 0) {
        if (exp10 < -4900) { r = 0; errno = ERANGE; }
        else if (exp10 > 4900) { r = HUGE_VALL; errno = ERANGE; }
        else if (exp10 < 0) r /= pow10l_(-exp10);    /* ভাগ: গুণের চেয়ে নির্ভুল */
        else r *= pow10l_(exp10);
    }
    return neg ? -r : r;
}

double strtod(const char *s, char **end)
{
    ld r = strtold(s, end);
    if (fabsl(r) > 1.7976931348623157e308L && !isinf(r)) errno = ERANGE;
    return (double)r;
}

float strtof(const char *s, char **end) { return (float)strtold(s, end); }
double atof(const char *s) { return strtod(s, NULL); }
