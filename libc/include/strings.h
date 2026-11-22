/* rozlibc — strings.h */
#ifndef _STRINGS_H
#define _STRINGS_H
#include <string.h>
/* ধাপ ১৬ক */
#include <stddef.h>
void  bzero(void *s, size_t n);
void  bcopy(const void *src, void *dst, size_t n);
int   bcmp(const void *a, const void *b, size_t n);
char *index(const char *s, int c);
char *rindex(const char *s, int c);
int   ffs(int v);
int   ffsl(long v);
int   ffsll(long long v);

#endif
