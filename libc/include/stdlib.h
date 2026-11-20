/* rozlibc — stdlib.h */
#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX     2147483647

void  *malloc(size_t n);
void  *calloc(size_t count, size_t n);
void  *realloc(void *p, size_t n);
void   free(void *p);

void   exit(int code) __attribute__((noreturn));
void   _Exit(int code) __attribute__((noreturn));
void   abort(void) __attribute__((noreturn));
int    atexit(void (*fn)(void));

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

/* ধাপ ১৪: প্রোগ্রামের নাম (argv[0]-এর শেষ অংশ), crt0-এর পর থেকে */
extern char *__progname;
extern char *program_invocation_short_name;

#endif
