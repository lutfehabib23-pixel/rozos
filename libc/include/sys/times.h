/* rozlibc — sys/times.h (ধাপ ১৬ক) */
#ifndef _SYS_TIMES_H
#define _SYS_TIMES_H
#include <_rozlibc.h>
struct tms { clock_t tms_utime, tms_stime, tms_cutime, tms_cstime; };
clock_t times(struct tms *t);
#endif
