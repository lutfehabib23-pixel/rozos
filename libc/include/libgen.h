/* rozlibc — libgen.h (ধাপ ১১: pdpmake-এর জন্য) */
#ifndef _LIBGEN_H
#define _LIBGEN_H

char *basename(char *path);   /* "/a/b/c" → "c"   (path বদলাতে পারে) */
char *dirname(char *path);    /* "/a/b/c" → "/a/b" */

#endif
