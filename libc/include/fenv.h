/* rozlibc — fenv.h (ধাপ ১৬ক): x87 আর SSE দুটোরই rounding mode ও exception ফ্ল্যাগ */
#ifndef _FENV_H
#define _FENV_H

#define FE_INVALID    0x01
#define FE_DIVBYZERO  0x04
#define FE_OVERFLOW   0x08
#define FE_UNDERFLOW  0x10
#define FE_INEXACT    0x20
#define FE_ALL_EXCEPT 0x3d

#define FE_TONEAREST  0x000
#define FE_DOWNWARD   0x400
#define FE_UPWARD     0x800
#define FE_TOWARDZERO 0xc00

typedef unsigned short fexcept_t;
typedef struct {
    unsigned short cw, pad1, sw, pad2, tags, pad3;
    unsigned int   ip;
    unsigned short cs, op;
    unsigned int   dp;
    unsigned short ds, pad4;
    unsigned int   mxcsr;
} fenv_t;
#define FE_DFL_ENV ((const fenv_t *)-1)

int feclearexcept(int e);
int fegetexceptflag(fexcept_t *f, int e);
int feraiseexcept(int e);
int fesetexceptflag(const fexcept_t *f, int e);
int fetestexcept(int e);
int fegetround(void);
int fesetround(int r);
int fegetenv(fenv_t *env);
int feholdexcept(fenv_t *env);
int fesetenv(const fenv_t *env);
int feupdateenv(const fenv_t *env);

#endif
