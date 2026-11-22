/* rozlibc — libm-এর বাকি অংশ (ধাপ ১৬ক)
 *
 * math.c-তে double-এর মূল ফাংশন (ধাপ ৭); এখানে:
 *   - long double (…l) রূপ x87-এর ৮০-বিট নির্ভুলতায়, float (…f) রূপ double দিয়ে
 *   - C99-এর নতুনগুলো: expm1 log1p (fyl2xp1/f2xm1 — ছোট x-এ নির্ভুল), cbrt (Newton),
 *     fma (ভাঙা গুণফল + ক্ষতিপূরণ যোগ), remainder/remquo (fprem1), nextafter, rint,
 *     lrint/lround, ilogb/logb/scalbn, fmin/fmax/fdim, asinh/acosh/atanh,
 *     erf/erfc (ধনাত্মক ধারা + continued fraction), tgamma/lgamma (Lanczos/Stirling)
 *   - শ্রেণিবিভাগ (__fpclassify…), fenv.h (x87 + SSE MXCSR একসাথে)
 * pow-এর বিশেষ ক্ষেত্রগুলো (inf, ±0, -1, NaN) C99 Annex F মেনে। */

#include <math.h>
#include <fenv.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

typedef long double ld;

int signgam;

#define LD_PI   3.14159265358979323846264338327950288L
#define LD_LN2  0.693147180559945309417232121458176568L
#define LD_LOG2E 1.44269504088896340735992468100189214L

/* ---------- বিট দেখা ---------- */

union ldbits { ld v; struct { uint64_t m; uint16_t se; } b; };

int __fpclassify(double x)
{
    uint64_t u;
    int e;
    memcpy(&u, &x, 8);
    e = (int)((u >> 52) & 0x7ff);
    if (e == 0x7ff) return (u << 12) ? FP_NAN : FP_INFINITE;
    if (e == 0) return (u << 1) ? FP_SUBNORMAL : FP_ZERO;
    return FP_NORMAL;
}

int __fpclassifyf(float x)
{
    uint32_t u;
    int e;
    memcpy(&u, &x, 4);
    e = (int)((u >> 23) & 0xff);
    if (e == 0xff) return (u << 9) ? FP_NAN : FP_INFINITE;
    if (e == 0) return (u << 1) ? FP_SUBNORMAL : FP_ZERO;
    return FP_NORMAL;
}

int __fpclassifyl(ld x)
{
    union ldbits u;
    int e;
    memset(&u, 0, sizeof(u));
    u.v = x;
    e = u.b.se & 0x7fff;
    if (e == 0x7fff) return (u.b.m << 1) ? FP_NAN : FP_INFINITE;
    if (e == 0) return u.b.m ? FP_SUBNORMAL : FP_ZERO;
    return FP_NORMAL;
}

int __signbitf(float x) { uint32_t u; memcpy(&u, &x, 4); return (int)(u >> 31); }
int __signbitl(ld x) { union ldbits u; memset(&u, 0, sizeof(u)); u.v = x; return (u.b.se >> 15) & 1; }

int finite(double x) { return isfinite(x); }

/* ---------- x87 সাহায্যকারী ---------- */

static ld x_rnd(ld x, int mode)              /* mode: 0 নিকটতম, 1 নিচে, 2 ওপরে, 3 শূন্যের দিকে, -1 = এখনকার */
{
    unsigned short cw, ncw;
    ld r;
    if (isnan(x) || isinf(x) || x == 0) return x;
    if (mode < 0) {
        __asm__ __volatile__("frndint" : "=t"(r) : "0"(x));
        return r;
    }
    __asm__ __volatile__("fnstcw %0" : "=m"(cw));
    ncw = (unsigned short)((cw & ~0x0C00) | (mode << 10));
    __asm__ __volatile__("fldcw %0" : : "m"(ncw));
    __asm__ __volatile__("frndint" : "=t"(r) : "0"(x));
    __asm__ __volatile__("fldcw %0" : : "m"(cw));
    return r;
}

ld sqrtl(ld x) { ld r; __asm__("fsqrt" : "=t"(r) : "0"(x)); return r; }
ld floorl(ld x) { return x_rnd(x, 1); }
ld ceill(ld x)  { return x_rnd(x, 2); }
ld truncl(ld x) { return x_rnd(x, 3); }
ld rintl(ld x)  { return x_rnd(x, -1); }
ld nearbyintl(ld x) { return x_rnd(x, -1); }
double rint(double x) { return (double)x_rnd(x, -1); }
double nearbyint(double x) { return (double)x_rnd(x, -1); }

ld roundl(ld x)
{
    ld t = truncl(x);
    if (fabsl(x - t) >= 0.5L) t += x < 0 ? -1.0L : 1.0L;
    return t;
}

ld copysignl(ld x, ld y)
{
    x = fabsl(x);
    return __signbitl(y) ? -x : x;
}

ld modfl(ld x, ld *ip)
{
    ld t = truncl(x);
    *ip = t;
    if (isinf(x)) return copysignl(0.0L, x);
    return x - t;
}

ld fmodl(ld x, ld y)
{
    ld r = x;
    unsigned short sw;
    if (y == 0 || isnan(x) || isnan(y) || isinf(x)) return NAN;
    if (isinf(y)) return x;
    do {
        __asm__ __volatile__("fprem; fnstsw %%ax" : "=t"(r), "=a"(sw) : "0"(r), "u"(y));
    } while (sw & 0x400);
    return r;
}

ld remainderl(ld x, ld y)
{
    ld r = x;
    unsigned short sw;
    if (y == 0 || isnan(x) || isnan(y) || isinf(x)) return NAN;
    if (isinf(y)) return x;
    do {
        __asm__ __volatile__("fprem1; fnstsw %%ax" : "=t"(r), "=a"(sw) : "0"(r), "u"(y));
    } while (sw & 0x400);
    return r;
}

double remainder(double x, double y) { return (double)remainderl(x, y); }
double drem(double x, double y) { return remainder(x, y); }

ld remquol(ld x, ld y, int *q)
{
    ld r = remainderl(x, y);
    ld qq;
    *q = 0;
    if (isnan(r)) return r;
    qq = (x - r) / y;
    qq = fabsl(qq);
    if (qq < 9.2e18L) *q = (int)((unsigned long long)rintl(qq) & 0x7fffffff);
    else *q = (int)((unsigned long long)rintl(fmodl(qq, 2147483648.0L)) & 0x7fffffff);
    if ((x < 0) != (y < 0)) *q = -*q;
    return r;
}

double remquo(double x, double y, int *q) { return (double)remquol(x, y, q); }

/* ---------- সূচক ও লগ ---------- */

static ld x_exp2(ld x)
{
    ld i, f, r;
    if (isnan(x)) return x;
    if (x > 16384) return HUGE_VALL;
    if (x < -16450) return 0;
    i = x_rnd(x, 0);
    f = x - i;
    __asm__("f2xm1" : "=t"(r) : "0"(f));
    r += 1;
    return ldexpl(r, (int)i);
}

static ld x_yl2x(ld y, ld x)
{
    ld r;
    __asm__("fyl2x" : "=t"(r) : "0"(x), "u"(y) : "st(1)");
    return r;
}

static ld x_yl2xp1(ld y, ld x)                /* y * log2(1 + x), |x| < 1 − √2/2 */
{
    ld r;
    __asm__("fyl2xp1" : "=t"(r) : "0"(x), "u"(y) : "st(1)");
    return r;
}

ld exp2l(ld x) { return x_exp2(x); }

ld expl(ld x)
{
    if (isnan(x)) return x;
    if (x > 11357.3L) return HUGE_VALL;
    if (x < -11400) return 0;
    /* x·log2(e)-কে দুই ভাগে, যাতে গুণের ভুল কম হয় */
    {
        ld t = x * LD_LOG2E, i = x_rnd(t, 0);
        ld f = (x - i * LD_LN2) * LD_LOG2E, r;
        __asm__("f2xm1" : "=t"(r) : "0"(f));
        return ldexpl(r + 1, (int)i);
    }
}

ld expm1l(ld x)
{
    if (isnan(x)) return x;
    if (x == 0) return x;
    if (fabsl(x) < 0.5L) {
        ld r, f = x * LD_LOG2E;
        __asm__("f2xm1" : "=t"(r) : "0"(f));
        return r;
    }
    if (x < -45) return -1.0L;
    return expl(x) - 1;
}

ld logl(ld x)   { return x < 0 || isnan(x) ? (isnan(x) ? x : NAN) : x == 0 ? -HUGE_VALL : x_yl2x(LD_LN2, x); }
ld log2l(ld x)  { return x < 0 || isnan(x) ? (isnan(x) ? x : NAN) : x == 0 ? -HUGE_VALL : x_yl2x(1.0L, x); }
ld log10l(ld x) { return x < 0 || isnan(x) ? (isnan(x) ? x : NAN) : x == 0 ? -HUGE_VALL : x_yl2x(0.301029995663981195213738894724493027L, x); }

ld log1pl(ld x)
{
    if (isnan(x)) return x;
    if (x == -1) return -HUGE_VALL;
    if (x < -1) return NAN;
    if (fabsl(x) < 0.29L) return x == 0 ? x : x_yl2xp1(LD_LN2, x);
    return logl(1 + x);
}

double expm1(double x) { return (double)expm1l(x); }
double log1p(double x) { return (double)log1pl(x); }
ld exp10l(ld x) { return x_exp2(x * 3.32192809488736234787031942948939018L); }
double exp10(double x) { return (double)exp10l(x); }
double pow10(double x) { return exp10(x); }

ld powl(ld x, ld y)
{
    ld iy, r;
    int yint, yodd = 0, neg = 0;
    if (y == 0) return 1.0L;
    if (x == 1) return 1.0L;
    if (isnan(x) || isnan(y)) return x + y;
    yint = modfl(y, &iy) == 0;
    if (yint && fabsl(iy) < 1.8e19L) yodd = fmodl(iy, 2.0L) != 0;
    if (isinf(y)) {
        ld ax = fabsl(x);
        if (ax == 1) return 1.0L;
        if ((ax < 1) == (y < 0)) return HUGE_VALL;
        return 0.0L;
    }
    if (x == 0) {
        if (y < 0) {
            errno = ERANGE;
            return yodd ? copysignl(HUGE_VALL, x) : HUGE_VALL;
        }
        return yodd ? x : 0.0L;
    }
    if (isinf(x)) {
        if (x > 0) return y > 0 ? HUGE_VALL : 0.0L;
        if (y > 0) return yodd ? -HUGE_VALL : HUGE_VALL;
        return yodd ? -0.0L : 0.0L;
    }
    if (x < 0) {
        if (!yint) { errno = EDOM; return NAN; }
        neg = yodd;
        x = -x;
    }
    /* ছোট পূর্ণসংখ্যা ঘাত: সরাসরি গুণ (নির্ভুল) */
    if (yint && fabsl(y) <= 64) {
        long n = (long)fabsl(y);
        ld b = x;
        r = 1.0L;
        while (n) { if (n & 1) r *= b; b *= b; n >>= 1; }
        if (y < 0) r = 1.0L / r;
    } else {
        r = x_exp2(x_yl2x(y, x));
    }
    return neg ? -r : r;
}

/* ---------- ত্রিকোণমিতি ---------- */

static ld x_reduce(ld x)
{
    if (fabsl(x) > 1e15L) return fmodl(x, 2.0L * LD_PI);
    return x;
}

ld sinl(ld x) { ld r; if (isinf(x)) return NAN; __asm__("fsin" : "=t"(r) : "0"(x_reduce(x))); return r; }
ld cosl(ld x) { ld r; if (isinf(x)) return NAN; __asm__("fcos" : "=t"(r) : "0"(x_reduce(x))); return r; }

ld tanl(ld x)
{
    ld r, one;
    if (isinf(x)) return NAN;
    __asm__("fptan" : "=t"(one), "=u"(r) : "0"(x_reduce(x)));
    (void)one;
    return r;
}

void sincosl(ld x, ld *s, ld *c) { *s = sinl(x); *c = cosl(x); }
void sincos(double x, double *s, double *c) { *s = sin(x); *c = cos(x); }
void sincosf(float x, float *s, float *c) { *s = (float)sin(x); *c = (float)cos(x); }

ld atan2l(ld y, ld x)
{
    ld r;
    __asm__("fpatan" : "=t"(r) : "0"(x), "u"(y) : "st(1)");
    return r;
}

ld atanl(ld x) { return atan2l(x, 1.0L); }
ld asinl(ld x) { return (x < -1 || x > 1) ? NAN : atan2l(x, sqrtl((1 - x) * (1 + x))); }
ld acosl(ld x) { return (x < -1 || x > 1) ? NAN : atan2l(sqrtl((1 - x) * (1 + x)), x); }

ld sinhl(ld x)
{
    ld a = fabsl(x), e;
    if (a < 1) { e = expm1l(a); e = (e + e / (e + 1)) / 2; }
    else { e = expl(a); e = (e - 1 / e) / 2; }
    return x < 0 ? -e : e;
}

ld coshl(ld x)
{
    ld e = expl(fabsl(x));
    return (e + 1 / e) / 2;
}

ld tanhl(ld x)
{
    ld a = fabsl(x), t;
    if (a > 23) t = 1;
    else { t = expm1l(2 * a); t = t / (t + 2); }
    return x < 0 ? -t : t;
}

ld asinhl(ld x)
{
    ld a = fabsl(x), r;
    if (a > 1e10L) r = logl(a) + LD_LN2;
    else r = log1pl(a + a * a / (1 + sqrtl(1 + a * a)));
    return x < 0 ? -r : r;
}

ld acoshl(ld x)
{
    if (x < 1) return NAN;
    if (x > 1e10L) return logl(x) + LD_LN2;
    return log1pl((x - 1) + sqrtl((x - 1) * (x + 1)));
}

ld atanhl(ld x)
{
    ld a = fabsl(x), r;
    if (a > 1) return NAN;
    if (a == 1) return copysignl(HUGE_VALL, x);
    r = 0.5L * log1pl(2 * a / (1 - a));
    return x < 0 ? -r : r;
}

double asinh(double x) { return (double)asinhl(x); }
double acosh(double x) { return (double)acoshl(x); }
double atanh(double x) { return (double)atanhl(x); }

ld hypotl(ld x, ld y)
{
    int ex, ey, e;
    if (isinf(x) || isinf(y)) return HUGE_VALL;
    if (isnan(x) || isnan(y)) return NAN;
    x = fabsl(x);
    y = fabsl(y);
    if (x == 0) return y;
    if (y == 0) return x;
    frexpl(x, &ex);
    frexpl(y, &ey);
    e = ex > ey ? ex : ey;
    x = ldexpl(x, -e);
    y = ldexpl(y, -e);
    return ldexpl(sqrtl(x * x + y * y), e);
}

/* ---------- ঘনমূল ---------- */

ld cbrtl(ld x)
{
    ld a = fabsl(x), y;
    int i;
    if (a == 0 || isnan(x) || isinf(x)) return x;
    y = x_exp2(x_yl2x(1.0L / 3.0L, a));
    for (i = 0; i < 2; i++) y = y - (y * y * y - a) / (3 * y * y);
    return x < 0 ? -y : y;
}

double cbrt(double x) { return (double)cbrtl(x); }

/* ---------- fma ---------- */

/* a = hi + lo, প্রত্যেকে ~৩২ বিট (long double-এ গুণফল নির্ভুল থাকে) */
static void split_ld(ld a, ld *hi, ld *lo)
{
    ld c = a * 4294967297.0L;                 /* 2^32 + 1 */
    *hi = c - (c - a);
    *lo = a - *hi;
}

static void two_sum(ld a, ld b, ld *s, ld *e)
{
    ld z;
    *s = a + b;
    z = *s - a;
    *e = (a - (*s - z)) + (b - z);
}

ld fmal(ld x, ld y, ld z)
{
    ld xh, xl, yh, yl, p, pe, s, se, t, te;
    if (!isfinite(x) || !isfinite(y) || !isfinite(z)) return x * y + z;
    if (x == 0 || y == 0) return x * y + z;
    split_ld(x, &xh, &xl);
    split_ld(y, &yh, &yl);
    p = x * y;
    pe = ((xh * yh - p) + xh * yl + xl * yh) + xl * yl;     /* গুণফলের হারানো অংশ */
    two_sum(p, z, &s, &se);
    two_sum(s, pe + se, &t, &te);
    return t + te;
}

double fma(double x, double y, double z)
{
    /* double-এর গুণফল (১০৬ বিট) = দুটো ৫৩-বিটের long double; বাকিটা ক্ষতিপূরণসহ যোগ */
    ld p, pe, s, se, t, te;
    ld xh, xl, yh, yl;
    if (!isfinite(x) || !isfinite(y) || !isfinite(z)) return x * y + z;
    if (x == 0 || y == 0) return x * y + z;
    split_ld(x, &xh, &xl);
    split_ld(y, &yh, &yl);
    p = (ld)x * (ld)y;
    pe = ((xh * yh - p) + xh * yl + xl * yh) + xl * yl;
    two_sum(p, (ld)z, &s, &se);
    two_sum(s, pe + se, &t, &te);
    return (double)(t + te);
}

float fmaf(float x, float y, float z) { return (float)((double)x * (double)y + (double)z); }

/* ---------- ছোটখাটো ---------- */

double fmin(double a, double b) { return isnan(a) ? b : isnan(b) ? a : (a < b || (a == b && signbit(a))) ? a : b; }
double fmax(double a, double b) { return isnan(a) ? b : isnan(b) ? a : (a > b || (a == b && !signbit(a))) ? a : b; }
double fdim(double a, double b) { return isnan(a) || isnan(b) ? a + b : a > b ? a - b : 0.0; }
ld fminl(ld a, ld b) { return isnan(a) ? b : isnan(b) ? a : (a < b || (a == b && signbit(a))) ? a : b; }
ld fmaxl(ld a, ld b) { return isnan(a) ? b : isnan(b) ? a : (a > b || (a == b && !signbit(a))) ? a : b; }
ld fdiml(ld a, ld b) { return isnan(a) || isnan(b) ? a + b : a > b ? a - b : 0.0L; }

ld scalbnl(ld x, int e) { return ldexpl(x, e); }
ld scalblnl(ld x, long e) { return ldexpl(x, e > 100000 ? 100000 : e < -100000 ? -100000 : (int)e); }
double scalbn(double x, int e) { return ldexp(x, e); }
double scalbln(double x, long e) { return (double)scalblnl(x, e); }

int ilogbl(ld x)
{
    int e;
    if (x == 0) return FP_ILOGB0;
    if (isnan(x)) return FP_ILOGBNAN;
    if (isinf(x)) return 2147483647;
    frexpl(x, &e);
    return e - 1;
}

int ilogb(double x) { return ilogbl(x); }

ld logbl(ld x)
{
    if (x == 0) return -HUGE_VALL;
    if (isinf(x)) return HUGE_VALL;
    if (isnan(x)) return x;
    return (ld)ilogbl(x);
}

double logb(double x) { return (double)logbl(x); }

double significand(double x) { return ldexp(x, -ilogb(x)); }

long lrintl(ld x) { return (long)rintl(x); }
long long llrintl(ld x) { return (long long)rintl(x); }
long lroundl(ld x) { return (long)roundl(x); }
long long llroundl(ld x) { return (long long)roundl(x); }
long lrint(double x) { return (long)rint(x); }
long long llrint(double x) { return (long long)rint(x); }
long lround(double x) { return (long)round(x); }
long long llround(double x) { return (long long)round(x); }
long lrintf(float x) { return (long)rint(x); }
long long llrintf(float x) { return (long long)rint(x); }
long lroundf(float x) { return (long)round(x); }
long long llroundf(float x) { return (long long)round(x); }

double nan(const char *t) { (void)t; return (double)NAN; }
float nanf(const char *t) { (void)t; return NAN; }
ld nanl(const char *t) { (void)t; return (ld)NAN; }

double nextafter(double x, double y)
{
    uint64_t u;
    if (isnan(x) || isnan(y)) return x + y;
    if (x == y) return y;
    if (x == 0) {
        u = 1;
        memcpy(&x, &u, 8);
        return y > 0 ? x : -x;
    }
    memcpy(&u, &x, 8);
    if ((x < y) == (x > 0)) u++;
    else u--;
    memcpy(&x, &u, 8);
    return x;
}

float nextafterf(float x, float y)
{
    uint32_t u;
    if (isnan(x) || isnan(y)) return x + y;
    if (x == y) return y;
    if (x == 0) {
        u = 1;
        memcpy(&x, &u, 4);
        return y > 0 ? x : -x;
    }
    memcpy(&u, &x, 4);
    if ((x < y) == (x > 0)) u++;
    else u--;
    memcpy(&x, &u, 4);
    return x;
}

ld nextafterl(ld x, ld y)
{
    union ldbits u;
    int up;
    if (isnan(x) || isnan(y)) return x + y;
    if (x == y) return y;
    memset(&u, 0, sizeof(u));
    if (x == 0) {
        u.b.m = 1;
        u.b.se = y > 0 ? 0 : 0x8000;
        return u.v;
    }
    u.v = x;
    up = (x < y) == (x > 0);                 /* |x| বাড়বে */
    if (up) {
        if (u.b.m == 0xffffffffffffffffULL) { u.b.m = 0x8000000000000000ULL; u.b.se++; }
        else if ((u.b.se & 0x7fff) == 0 && u.b.m == 0x7fffffffffffffffULL) { u.b.m++; u.b.se++; }
        else u.b.m++;
    } else {
        if (u.b.m == 0x8000000000000000ULL && (u.b.se & 0x7fff) > 1) { u.b.m = 0xffffffffffffffffULL; u.b.se--; }
        else if (u.b.m == 0x8000000000000000ULL && (u.b.se & 0x7fff) == 1) { u.b.m = 0x7fffffffffffffffULL; u.b.se--; }
        else u.b.m--;
    }
    return u.v;
}

double nexttoward(double x, ld y) { return nextafter(x, x < y ? HUGE_VAL : x > y ? -HUGE_VAL : (double)y); }
float nexttowardf(float x, ld y) { return nextafterf(x, x < y ? HUGE_VALF : x > y ? -HUGE_VALF : (float)y); }
ld nexttowardl(ld x, ld y) { return nextafterl(x, y); }

/* ---------- erf / erfc ---------- */

#define LD_2_SQRTPI 1.12837916709551257389615890312154517L
#define LD_1_SQRTPI 0.564189583547756286948079451560772586L

static ld erf_series(ld x)                     /* |x| ≤ ~3: সব পদ ধনাত্মক, বাতিল হয় না */
{
    ld x2 = x * x, t = x, sum = x;
    int n;
    for (n = 1; n < 200; n++) {
        t *= 2 * x2 / (2 * n + 1);
        sum += t;
        if (t < sum * 1e-21L) break;
    }
    return LD_2_SQRTPI * expl(-x2) * sum;
}

static ld erfc_cf(ld x)                         /* x ≥ 2: continued fraction */
{
    ld f = x;
    int k, n = x < 3 ? 400 : x < 6 ? 120 : 40;
    for (k = n; k >= 1; k--) f = x + (k / 2.0L) / f;
    return expl(-x * x) * LD_1_SQRTPI / f;
}

ld erfl(ld x)
{
    ld a = fabsl(x), r;
    if (isnan(x)) return x;
    if (a > 6.5L) r = 1;
    else if (a < 2.5L) r = erf_series(a);
    else r = 1 - erfc_cf(a);
    return x < 0 ? -r : r;
}

ld erfcl(ld x)
{
    if (isnan(x)) return x;
    if (x < 0.5L) return 1 - erfl(x);
    if (x < 2) return 1 - erf_series(x);
    if (x > 107) return 0;
    return erfc_cf(x);
}

double erf(double x) { return (double)erfl(x); }
double erfc(double x) { return (double)erfcl(x); }

/* ---------- gamma ---------- */

static const double lanczos[9] = {
    0.99999999999980993, 676.5203681218851, -1259.1392167224028, 771.32342877765313,
    -176.61502916214059, 12.507343278686905, -0.13857109526572012, 9.9843695780195716e-6,
    1.5056327351493116e-7,
};

static ld gamma_pos(ld x)                       /* x > 0 */
{
    ld a, t;
    int i;
    if (x == x_rnd(x, 3) && x <= 171) {
        ld r = 1;
        long n;
        for (n = 2; n < (long)x; n++) r *= n;
        return r;
    }
    if (x < 0.5L) return LD_PI / (sinl(LD_PI * x) * gamma_pos(1 - x));
    x -= 1;
    a = lanczos[0];
    t = x + 7.5L;
    for (i = 1; i < 9; i++) a += lanczos[i] / (x + i);
    return 2.50662827463100050241576528481104525L * powl(t, x + 0.5L) * expl(-t) * a;
}

ld tgammal(ld x)
{
    if (isnan(x)) return x;
    if (x == 0) { errno = ERANGE; return copysignl(HUGE_VALL, x); }
    if (isinf(x)) return x > 0 ? x : NAN;
    if (x < 0 && x == x_rnd(x, 3)) { errno = EDOM; return NAN; }
    if (x > 171.7L) { errno = ERANGE; return HUGE_VALL; }
    if (x < 0) {
        /* প্রতিফলন: Γ(x) = π / (sin(πx) Γ(1−x)) */
        ld s = sinl(LD_PI * x), g;
        if (x < -185) return 0;
        g = gamma_pos(1 - x);
        return LD_PI / (s * g);
    }
    return gamma_pos(x);
}

static ld lgamma_pos(ld x)                      /* x > 0 */
{
    ld shift = 0, z, z2, s;
    if (x < 15) {
        if (x < 10 && x > 0.1L) {
            ld g = gamma_pos(x);
            return logl(fabsl(g));
        }
        while (x < 15) { shift += logl(x); x += 1; }
    }
    z = 1 / x;
    z2 = z * z;
    s = z * (1.0L / 12 - z2 * (1.0L / 360 - z2 * (1.0L / 1260 - z2 * (1.0L / 1680 - z2 / 1188))));
    return (x - 0.5L) * logl(x) - x + 0.918938533204672741780329736405617640L + s - shift;
}

ld lgammal_r(ld x, int *sign)
{
    *sign = 1;
    if (isnan(x)) return x;
    if (isinf(x)) return HUGE_VALL;
    if (x <= 0 && x == x_rnd(x, 3)) { errno = ERANGE; return HUGE_VALL; }
    if (x < 0) {
        ld s = sinl(LD_PI * x);
        *sign = s < 0 ? -1 : 1;
        return logl(LD_PI / fabsl(s)) - lgamma_pos(1 - x);
    }
    return lgamma_pos(x);
}

ld lgammal(ld x) { return lgammal_r(x, &signgam); }
double lgamma_r(double x, int *sign) { return (double)lgammal_r(x, sign); }
float lgammaf_r(float x, int *sign) { return (float)lgammal_r(x, sign); }
double lgamma(double x) { return (double)lgammal_r(x, &signgam); }
double gamma(double x) { return lgamma(x); }
double tgamma(double x) { return (double)tgammal(x); }

/* ---------- float ও long double মোড়ক ---------- */

#define F1(n) float n##f(float x) { return (float)n(x); }
#define F2(n) float n##f(float x, float y) { return (float)n(x, y); }

F1(trunc) F1(round) F1(rint) F1(nearbyint) F1(cbrt)
F1(exp) F1(exp2) F1(expm1) F1(log) F1(log2) F1(log10) F1(log1p) F1(logb)
F1(sin) F1(cos) F1(tan) F1(asin) F1(acos) F1(atan) F1(sinh) F1(cosh) F1(tanh)
F1(asinh) F1(acosh) F1(atanh) F1(erf) F1(erfc) F1(tgamma) F1(lgamma) F1(exp10)
F2(fmod) F2(remainder) F2(pow) F2(atan2) F2(hypot) F2(copysign) F2(fmin) F2(fmax) F2(fdim)

float scalbnf(float x, int e) { return (float)ldexp(x, e); }
float scalblnf(float x, long e) { return (float)scalbln(x, e); }
float frexpf(float x, int *e) { return (float)frexp(x, e); }
float modff(float x, float *ip) { double i; float r = (float)modf(x, &i); *ip = (float)i; return r; }
int ilogbf(float x) { return ilogb(x); }
float remquof(float x, float y, int *q) { return (float)remquo(x, y, q); }

/* ---------- fenv ---------- */

int fegetround(void)
{
    unsigned short cw;
    __asm__ __volatile__("fnstcw %0" : "=m"(cw));
    return cw & 0xc00;
}

int fesetround(int r)
{
    unsigned short cw;
    unsigned int mx;
    if (r & ~0xc00) return -1;
    __asm__ __volatile__("fnstcw %0" : "=m"(cw));
    cw = (unsigned short)((cw & ~0xc00) | r);
    __asm__ __volatile__("fldcw %0" : : "m"(cw));
    __asm__ __volatile__("stmxcsr %0" : "=m"(mx));
    mx = (mx & ~0x6000u) | ((unsigned)r << 3);
    __asm__ __volatile__("ldmxcsr %0" : : "m"(mx));
    return 0;
}

int fetestexcept(int e)
{
    unsigned short sw;
    unsigned int mx;
    __asm__ __volatile__("fnstsw %0" : "=m"(sw));
    __asm__ __volatile__("stmxcsr %0" : "=m"(mx));
    return (int)((sw | mx) & (unsigned)e & FE_ALL_EXCEPT);
}

int feclearexcept(int e)
{
    fenv_t env;
    unsigned int mx;
    e &= FE_ALL_EXCEPT;
    __asm__ __volatile__("fnstenv %0" : "=m"(env));
    env.sw &= (unsigned short)~e;
    __asm__ __volatile__("fldenv %0" : : "m"(env));
    __asm__ __volatile__("stmxcsr %0" : "=m"(mx));
    mx &= ~(unsigned)e;
    __asm__ __volatile__("ldmxcsr %0" : : "m"(mx));
    return 0;
}

int feraiseexcept(int e)
{
    unsigned int mx;
    e &= FE_ALL_EXCEPT;
    __asm__ __volatile__("stmxcsr %0" : "=m"(mx));
    mx |= (unsigned)e;
    __asm__ __volatile__("ldmxcsr %0" : : "m"(mx));
    return 0;
}

int fegetexceptflag(fexcept_t *f, int e)
{
    *f = (fexcept_t)fetestexcept(e);
    return 0;
}

int fesetexceptflag(const fexcept_t *f, int e)
{
    feclearexcept(e);
    feraiseexcept(*f & e);
    return 0;
}

int fegetenv(fenv_t *env)
{
    __asm__ __volatile__("fnstenv %0" : "=m"(*env));
    __asm__ __volatile__("fldenv %0" : : "m"(*env));       /* fnstenv সব exception mask করে দেয় */
    __asm__ __volatile__("stmxcsr %0" : "=m"(env->mxcsr));
    return 0;
}

int fesetenv(const fenv_t *env)
{
    if (env == FE_DFL_ENV) {
        unsigned int mx = 0x1f80;
        __asm__ __volatile__("fninit");
        __asm__ __volatile__("ldmxcsr %0" : : "m"(mx));
        return 0;
    }
    __asm__ __volatile__("fldenv %0" : : "m"(*env));
    __asm__ __volatile__("ldmxcsr %0" : : "m"(env->mxcsr));
    return 0;
}

int feholdexcept(fenv_t *env)
{
    fegetenv(env);
    feclearexcept(FE_ALL_EXCEPT);
    return 0;
}

int feupdateenv(const fenv_t *env)
{
    int e = fetestexcept(FE_ALL_EXCEPT);
    fesetenv(env);
    feraiseexcept(e);
    return 0;
}
