/* rozlibc — sys/times.h (ধাপ ১৬ক) */
#ifndef _SYS_TIMES_H
#define _SYS_TIMES_H
#ifdef __cplusplus
extern "C" {
#endif
#include <_rozlibc.h>
struct tms { clock_t tms_utime, tms_stime, tms_cutime, tms_cstime; };
clock_t times(struct tms *t);
#ifdef __cplusplus
}
#endif
#endif
