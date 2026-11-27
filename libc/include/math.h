/* rozlibc — math.h
 *
 * ধাপ ৭: double ফাংশন (x87 FPU দিয়ে)। ধাপ ১৬ক: পুরো C99 libm — float (…f) ও
 * long double (…l) রূপ, শ্রেণিবিভাগ (fpclassify, isnormal, isgreater…), expm1/log1p,
 * cbrt, fma, erf/erfc, tgamma/lgamma, asinh/acosh/atanh, nextafter, remquo ইত্যাদি। */
#ifndef _MATH_H
#define _MATH_H
#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) && !defined(__TINYC__)
#define HUGE_VAL  __builtin_huge_val()
#define HUGE_VALF __builtin_huge_valf()
#define HUGE_VALL __builtin_huge_vall()
#define INFINITY  __builtin_inff()
#define NAN       __builtin_nanf("")
#else
/* ধাপ ১৬ক: TCC-তে এই builtin-গুলো নেই — ধ্রুবক রাশি দিয়ে (static initializer-এও চলে) */
#define HUGE_VAL  (1e5000)
#define HUGE_VALF (1e5000f)
#define HUGE_VALL (1e5000L)
#define INFINITY  (1e5000f)
#define NAN       (0.0f / 0.0f)
#endif
#define MAXFLOAT  3.40282346638528859812e+38F

#define M_E        2.7182818284590452354
#define M_LOG2E    1.4426950408889634074
#define M_LOG10E   0.43429448190325182765
#define M_LN2      0.69314718055994530942
#define M_LN10     2.30258509299404568402
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.78539816339744830962
#define M_1_PI     0.31830988618379067154
#define M_2_PI     0.63661977236758134308
#define M_2_SQRTPI 1.12837916709551257390
#define M_SQRT2    1.41421356237309504880
#define M_SQRT1_2  0.70710678118654752440

/* ধাপ ১৬ক: শ্রেণিবিভাগ */
#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4
#define FP_ILOGB0    (-2147483647 - 1)
#define FP_ILOGBNAN  (-2147483647 - 1)
#define MATH_ERRNO     1
#define MATH_ERREXCEPT 2
#define math_errhandling MATH_ERREXCEPT
#define FLT_EVAL_METHOD_ROZLIBC 0
typedef float  float_t;
typedef double double_t;

int __fpclassifyf(float x);
int __fpclassify(double x);
int __fpclassifyl(long double x);
int __rozlibc_signbit(double x);
int __signbitf(float x);
int __signbitl(long double x);

#define fpclassify(x) (sizeof(x) == sizeof(float) ? __fpclassifyf((float)(x)) : \
                       sizeof(x) == sizeof(double) ? __fpclassify((double)(x)) : \
                       __fpclassifyl((long double)(x)))
#define isnan(x)    ((x) != (x))
#define isinf(x)    (!isnan(x) && isnan((x) - (x)))
#define isfinite(x) (!isnan((x) - (x)))
#define isnormal(x) (fpclassify(x) == FP_NORMAL)
#define signbit(x)  (sizeof(x) == sizeof(float) ? __signbitf((float)(x)) : \
                     sizeof(x) == sizeof(double) ? __rozlibc_signbit((double)(x)) : \
                     __signbitl((long double)(x)))
#define isunordered(a, b)    (isnan(a) || isnan(b))
#define isgreater(a, b)      (!isunordered(a, b) && (a) > (b))
#define isgreaterequal(a, b) (!isunordered(a, b) && (a) >= (b))
#define isless(a, b)         (!isunordered(a, b) && (a) < (b))
#define islessequal(a, b)    (!isunordered(a, b) && (a) <= (b))
#define islessgreater(a, b)  (!isunordered(a, b) && ((a) < (b) || (a) > (b)))

extern int signgam;

/* প্রতিটা ফাংশনের তিন রূপ: double, float, long double */
#define __ROZ_M1(n)  double n(double x); float n##f(float x); long double n##l(long double x);
#define __ROZ_M2(n)  double n(double x, double y); float n##f(float x, float y); \
                     long double n##l(long double x, long double y);

__ROZ_M1(fabs) __ROZ_M1(floor) __ROZ_M1(ceil) __ROZ_M1(trunc) __ROZ_M1(round)
__ROZ_M1(rint) __ROZ_M1(nearbyint) __ROZ_M1(sqrt) __ROZ_M1(cbrt)
__ROZ_M1(exp) __ROZ_M1(exp2) __ROZ_M1(expm1) __ROZ_M1(log) __ROZ_M1(log2) __ROZ_M1(log10)
__ROZ_M1(log1p) __ROZ_M1(logb)
__ROZ_M1(sin) __ROZ_M1(cos) __ROZ_M1(tan) __ROZ_M1(asin) __ROZ_M1(acos) __ROZ_M1(atan)
__ROZ_M1(sinh) __ROZ_M1(cosh) __ROZ_M1(tanh) __ROZ_M1(asinh) __ROZ_M1(acosh) __ROZ_M1(atanh)
__ROZ_M1(erf) __ROZ_M1(erfc) __ROZ_M1(tgamma) __ROZ_M1(lgamma)
__ROZ_M2(fmod) __ROZ_M2(remainder) __ROZ_M2(pow) __ROZ_M2(atan2) __ROZ_M2(hypot)
__ROZ_M2(copysign) __ROZ_M2(fmin) __ROZ_M2(fmax) __ROZ_M2(fdim) __ROZ_M2(nextafter)

double ldexp(double x, int e);       float ldexpf(float x, int e);       long double ldexpl(long double x, int e);
double scalbn(double x, int e);      float scalbnf(float x, int e);      long double scalbnl(long double x, int e);
double scalbln(double x, long e);    float scalblnf(float x, long e);    long double scalblnl(long double x, long e);
double frexp(double x, int *e);      float frexpf(float x, int *e);      long double frexpl(long double x, int *e);
double modf(double x, double *ip);   float modff(float x, float *ip);    long double modfl(long double x, long double *ip);
int    ilogb(double x);              int ilogbf(float x);                int ilogbl(long double x);
long   lrint(double x);              long lrintf(float x);               long lrintl(long double x);
long long llrint(double x);          long long llrintf(float x);         long long llrintl(long double x);
long   lround(double x);             long lroundf(float x);              long lroundl(long double x);
long long llround(double x);         long long llroundf(float x);        long long llroundl(long double x);
double remquo(double x, double y, int *q); float remquof(float x, float y, int *q);
long double remquol(long double x, long double y, int *q);
double fma(double x, double y, double z); float fmaf(float x, float y, float z);
long double fmal(long double x, long double y, long double z);
double nexttoward(double x, long double y); float nexttowardf(float x, long double y);
long double nexttowardl(long double x, long double y);
double nan(const char *tag);         float nanf(const char *tag);        long double nanl(const char *tag);
double lgamma_r(double x, int *sign); float lgammaf_r(float x, int *sign);
long double lgammal_r(long double x, int *sign);
/* GNU */
double exp10(double x);              float exp10f(float x);              long double exp10l(long double x);
double pow10(double x);
void   sincos(double x, double *s, double *c);
void   sincosf(float x, float *s, float *c);
void   sincosl(long double x, long double *s, long double *c);
double gamma(double x);
double drem(double x, double y);
int    finite(double x);
double significand(double x);

#undef __ROZ_M1
#undef __ROZ_M2

#ifdef __cplusplus
}
#endif
#endif
