/* rozlibc — wchar.h (ধাপ ১৬ক)
 *
 * wchar_t = 32-বিট Unicode code point; মাল্টিবাইট সবসময় UTF-8 (সব locale-এ)।
 * mbstate_t-তে অর্ধেক পড়া UTF-8 অক্ষর জমা থাকে (mbrtowc টুকরো টুকরো পেলেও চলে)। */
#ifndef _WCHAR_H
#define _WCHAR_H

#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <_rozlibc.h>

#ifndef _ROZLIBC_WINT
#define _ROZLIBC_WINT
typedef unsigned int wint_t;
#endif
#define WEOF ((wint_t)-1)
#ifndef WCHAR_MIN
#define WCHAR_MIN (-2147483647 - 1)
#define WCHAR_MAX 2147483647
#endif

typedef struct {
    unsigned int count;          /* আরও কত বাইট বাকি */
    unsigned int wc;             /* এ পর্যন্ত জমা */
} mbstate_t;

struct tm;
typedef struct _rozlibc_file FILE;

/* রূপান্তর */
wint_t btowc(int c);
int    wctob(wint_t c);
int    mbsinit(const mbstate_t *ps);
size_t mbrlen(const char *s, size_t n, mbstate_t *ps);
size_t mbrtowc(wchar_t *wc, const char *s, size_t n, mbstate_t *ps);
size_t wcrtomb(char *s, wchar_t wc, mbstate_t *ps);
size_t mbsrtowcs(wchar_t *dst, const char **src, size_t n, mbstate_t *ps);
size_t mbsnrtowcs(wchar_t *dst, const char **src, size_t nms, size_t n, mbstate_t *ps);
size_t wcsrtombs(char *dst, const wchar_t **src, size_t n, mbstate_t *ps);
size_t wcsnrtombs(char *dst, const wchar_t **src, size_t nwc, size_t n, mbstate_t *ps);

/* স্ট্রিং */
size_t   wcslen(const wchar_t *s);
size_t   wcsnlen(const wchar_t *s, size_t max);
wchar_t *wcscpy(wchar_t *d, const wchar_t *s);
wchar_t *wcsncpy(wchar_t *d, const wchar_t *s, size_t n);
wchar_t *wcpcpy(wchar_t *d, const wchar_t *s);
wchar_t *wcpncpy(wchar_t *d, const wchar_t *s, size_t n);
wchar_t *wcscat(wchar_t *d, const wchar_t *s);
wchar_t *wcsncat(wchar_t *d, const wchar_t *s, size_t n);
int      wcscmp(const wchar_t *a, const wchar_t *b);
int      wcsncmp(const wchar_t *a, const wchar_t *b, size_t n);
int      wcscasecmp(const wchar_t *a, const wchar_t *b);
int      wcsncasecmp(const wchar_t *a, const wchar_t *b, size_t n);
int      wcscoll(const wchar_t *a, const wchar_t *b);
size_t   wcsxfrm(wchar_t *d, const wchar_t *s, size_t n);
wchar_t *wcschr(const wchar_t *s, wchar_t c);
wchar_t *wcsrchr(const wchar_t *s, wchar_t c);
wchar_t *wcsstr(const wchar_t *hay, const wchar_t *needle);
size_t   wcsspn(const wchar_t *s, const wchar_t *acc);
size_t   wcscspn(const wchar_t *s, const wchar_t *rej);
wchar_t *wcspbrk(const wchar_t *s, const wchar_t *acc);
wchar_t *wcstok(wchar_t *s, const wchar_t *delim, wchar_t **save);
wchar_t *wcsdup(const wchar_t *s);
wchar_t *wmemcpy(wchar_t *d, const wchar_t *s, size_t n);
wchar_t *wmemmove(wchar_t *d, const wchar_t *s, size_t n);
wchar_t *wmemset(wchar_t *d, wchar_t c, size_t n);
int      wmemcmp(const wchar_t *a, const wchar_t *b, size_t n);
wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n);
int      wcwidth(wchar_t c);
int      wcswidth(const wchar_t *s, size_t n);

/* সংখ্যা */
long               wcstol(const wchar_t *s, wchar_t **end, int base);
unsigned long      wcstoul(const wchar_t *s, wchar_t **end, int base);
long long          wcstoll(const wchar_t *s, wchar_t **end, int base);
unsigned long long wcstoull(const wchar_t *s, wchar_t **end, int base);
double             wcstod(const wchar_t *s, wchar_t **end);
float              wcstof(const wchar_t *s, wchar_t **end);
long double        wcstold(const wchar_t *s, wchar_t **end);

/* wide I/O (FILE-এ UTF-8 হিসেবে পড়া/লেখা) */
wint_t   fgetwc(FILE *f);
wint_t   getwc(FILE *f);
wint_t   getwchar(void);
wint_t   fputwc(wchar_t c, FILE *f);
wint_t   putwc(wchar_t c, FILE *f);
wint_t   putwchar(wchar_t c);
wint_t   ungetwc(wint_t c, FILE *f);
wchar_t *fgetws(wchar_t *s, int n, FILE *f);
int      fputws(const wchar_t *s, FILE *f);
int      fwide(FILE *f, int mode);
int      wprintf(const wchar_t *fmt, ...);
int      fwprintf(FILE *f, const wchar_t *fmt, ...);
int      swprintf(wchar_t *s, size_t n, const wchar_t *fmt, ...);
int      vwprintf(const wchar_t *fmt, va_list ap);
int      vfwprintf(FILE *f, const wchar_t *fmt, va_list ap);
int      vswprintf(wchar_t *s, size_t n, const wchar_t *fmt, va_list ap);
int      wscanf(const wchar_t *fmt, ...);
int      fwscanf(FILE *f, const wchar_t *fmt, ...);
int      swscanf(const wchar_t *s, const wchar_t *fmt, ...);
int      vswscanf(const wchar_t *s, const wchar_t *fmt, va_list ap);
size_t   wcsftime(wchar_t *s, size_t n, const wchar_t *fmt, const struct tm *tm);

#endif
