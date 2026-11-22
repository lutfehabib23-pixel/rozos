/* rozlibc — utime.h (ধাপ ১৬ক) */
#ifndef _UTIME_H
#define _UTIME_H
#include <_rozlibc.h>
struct utimbuf { time_t actime, modtime; };
int utime(const char *path, const struct utimbuf *t);
#endif
