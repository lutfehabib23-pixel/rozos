/* rozlibc — math.h (double; float/long double রূপগুলো double দিয়ে) */
#ifndef _MATH_H
#define _MATH_H

#define HUGE_VAL  __builtin_huge_val()
#define HUGE_VALF __builtin_huge_valf()
#define HUGE_VALL __builtin_huge_vall()
#define INFINITY  __builtin_inff()
#define NAN       __builtin_nanf("")
#define M_PI      3.14159265358979323846
#define M_E       2.7182818284590452354
#define M_LN2     0.69314718055994530942
#define M_SQRT2   1.41421356237309504880

#define isnan(x)    ((x) != (x))
#define isinf(x)    (!isnan(x) && isnan((x) - (x)))
#define isfinite(x) (!isnan((x) - (x)))
#define signbit(x)  (__rozlibc_signbit((double)(x)))

int    __rozlibc_signbit(double x);

double fabs(double x);
double floor(double x);
double ceil(double x);
double trunc(double x);
double round(double x);
double fmod(double x, double y);
double sqrt(double x);
double ldexp(double x, int e);
double frexp(double x, int *e);
double modf(double x, double *ip);
double exp(double x);
double exp2(double x);
double log(double x);
double log2(double x);
double log10(double x);
double pow(double x, double y);
double sin(double x);
double cos(double x);
double tan(double x);
double atan(double x);
double atan2(double y, double x);
double asin(double x);
double acos(double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double hypot(double x, double y);
double copysign(double x, double y);

float  fabsf(float x);
float  sqrtf(float x);
float  floorf(float x);
float  ceilf(float x);
float  ldexpf(float x, int e);
long double ldexpl(long double x, int e);
long double fabsl(long double x);
long double frexpl(long double x, int *e);

#endif
