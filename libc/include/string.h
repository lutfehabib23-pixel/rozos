/* rozlibc — string.h */
#ifndef _STRING_H
#define _STRING_H

#include <stddef.h>

void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
void  *memchr(const void *s, int c, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
char  *strcpy(char *dst, const char *src);
char  *stpcpy(char *dst, const char *src);           /* ধাপ ১১: শেষের \0-এর ঠিকানা ফেরত */
char  *strncpy(char *dst, const char *src, size_t n);
char  *strcat(char *dst, const char *src);
char  *strncat(char *dst, const char *src, size_t n);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
int    strcoll(const char *a, const char *b);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *hay, const char *needle);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char  *strpbrk(const char *s, const char *accept);
char  *strtok(char *s, const char *delim);
char  *strtok_r(char *s, const char *delim, char **save);
char  *strdup(const char *s);
char  *strndup(const char *s, size_t n);
char  *strerror(int err);
int    strcasecmp(const char *a, const char *b);
int    strncasecmp(const char *a, const char *b, size_t n);

/* ধাপ ১৬ক */
void  *memrchr(const void *s, int c, size_t n);
void  *memmem(const void *hay, size_t hn, const void *needle, size_t nn);
void  *mempcpy(void *dst, const void *src, size_t n);
void  *memccpy(void *dst, const void *src, int c, size_t n);
char  *strchrnul(const char *s, int c);
char  *strcasestr(const char *hay, const char *needle);
char  *stpncpy(char *dst, const char *src, size_t n);
char  *strsep(char **sp, const char *delim);
size_t strlcpy(char *dst, const char *src, size_t size);
size_t strlcat(char *dst, const char *src, size_t size);
int    strerror_r(int err, char *buf, size_t len);
char  *strsignal(int sig);
size_t strxfrm(char *dst, const char *src, size_t n);
int    strverscmp(const char *a, const char *b);
void   explicit_bzero(void *s, size_t n);

#endif
