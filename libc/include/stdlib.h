/* rozlibc — stdlib.h */
#ifndef _STDLIB_H
#define _STDLIB_H
#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX     2147483647
#define MB_CUR_MAX   4            /* ধাপ ১৬ক: সব locale UTF-8 */

typedef struct { int quot, rem; } div_t;
typedef struct { long quot, rem; } ldiv_t;
typedef struct { long long quot, rem; } lldiv_t;

void  *malloc(size_t n);
void  *calloc(size_t count, size_t n);
void  *realloc(void *p, size_t n);
void   free(void *p);

void   exit(int code) __attribute__((noreturn));
void   _Exit(int code) __attribute__((noreturn));
void   abort(void) __attribute__((noreturn));
int    atexit(void (*fn)(void));
int    __cxa_atexit(void (*fn)(void *), void *arg, void *dso);
void   __cxa_finalize(void *dso);

int    atoi(const char *s);
long   atol(const char *s);
long long atoll(const char *s);
long   strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
long long strtoll(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
double strtod(const char *s, char **end);
float  strtof(const char *s, char **end);
long double strtold(const char *s, char **end);
double atof(const char *s);
char  *realpath(const char *path, char *resolved);

int    abs(int v);
long   labs(long v);
long long llabs(long long v);

int    rand(void);
void   srand(unsigned seed);

void   qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
void  *bsearch(const void *key, const void *base, size_t n, size_t size,
               int (*cmp)(const void *, const void *));

char  *getenv(const char *name);
int    setenv(const char *name, const char *value, int overwrite);
int    unsetenv(const char *name);
int    putenv(char *s);
int    system(const char *command);      /* ধাপ ১১: /bin/sh -c command */
extern char **environ;

/* ---------- ধাপ ১৬ক ---------- */
div_t   div(int a, int b);
ldiv_t  ldiv(long a, long b);
lldiv_t lldiv(long long a, long long b);
void   qsort_r(void *base, size_t n, size_t size,
               int (*cmp)(const void *, const void *, void *), void *arg);
int    posix_memalign(void **out, size_t align, size_t n);
void  *aligned_alloc(size_t align, size_t n);
void  *memalign(size_t align, size_t n);
void  *valloc(size_t n);
void  *reallocarray(void *p, size_t count, size_t n);
size_t malloc_usable_size(void *p);
long   random(void);
void   srandom(unsigned seed);
char  *initstate(unsigned seed, char *state, size_t n);
char  *setstate(char *state);
int    rand_r(unsigned *seed);
double drand48(void);
double erand48(unsigned short x[3]);
long   lrand48(void);
long   nrand48(unsigned short x[3]);
long   mrand48(void);
long   jrand48(unsigned short x[3]);
void   srand48(long seed);
unsigned short *seed48(unsigned short x[3]);
int    mkstemp(char *tmpl);
int    mkostemp(char *tmpl, int flags);
int    mkstemps(char *tmpl, int suffixlen);
char  *mkdtemp(char *tmpl);
char  *mktemp(char *tmpl);
int    clearenv(void);
char  *secure_getenv(const char *name);
void   quick_exit(int code) __attribute__((noreturn));
int    at_quick_exit(void (*fn)(void));
int    getsubopt(char **opt, char *const *tokens, char **value);
char  *l64a(long v);
long   a64l(const char *s);
char  *ecvt(double v, int ndig, int *dec, int *sign);
char  *fcvt(double v, int ndig, int *dec, int *sign);
char  *gcvt(double v, int ndig, char *buf);
int    getloadavg(double loads[], int n);
/* মাল্টিবাইট (UTF-8) */
int    mblen(const char *s, size_t n);
int    mbtowc(wchar_t *wc, const char *s, size_t n);
int    wctomb(char *s, wchar_t wc);
size_t mbstowcs(wchar_t *dst, const char *src, size_t n);
size_t wcstombs(char *dst, const wchar_t *src, size_t n);

/* ধাপ ১৪: প্রোগ্রামের নাম (argv[0]-এর শেষ অংশ), crt0-এর পর থেকে */
extern char *__progname;
extern char *program_invocation_short_name;

#ifdef __cplusplus
}
#endif
#endif
