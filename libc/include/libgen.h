/* rozlibc — libgen.h (ধাপ ১১: pdpmake-এর জন্য) */
#ifndef _LIBGEN_H
#define _LIBGEN_H
#ifdef __cplusplus
extern "C" {
#endif

char *basename(char *path);   /* "/a/b/c" → "c"   (path বদলাতে পারে) */
char *dirname(char *path);    /* "/a/b/c" → "/a/b" */

#ifdef __cplusplus
}
#endif
#endif
