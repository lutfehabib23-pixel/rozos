/* rozlibc — utime.h (ধাপ ১৬ক) */
#ifndef _UTIME_H
#define _UTIME_H
#ifdef __cplusplus
extern "C" {
#endif
#include <_rozlibc.h>
struct utimbuf { time_t actime, modtime; };
int utime(const char *path, const struct utimbuf *t);
#ifdef __cplusplus
}
#endif
#endif
