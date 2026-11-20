/* rozlibc — time.h */
#ifndef _TIME_H
#define _TIME_H

#include <_rozlibc.h>

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1
#define CLOCKS_PER_SEC  1000000L

typedef int clockid_t;

struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
};

time_t     time(time_t *t);
clock_t    clock(void);
int        clock_gettime(clockid_t clk, struct timespec *ts);
int        nanosleep(const struct timespec *req, struct timespec *rem);
struct tm *gmtime(const time_t *t);
struct tm *gmtime_r(const time_t *t, struct tm *out);
struct tm *localtime(const time_t *t);        /* rozOS-এ সময় অঞ্চল নেই: UTC */
time_t     mktime(struct tm *tm);
size_t     strftime(char *buf, size_t size, const char *fmt, const struct tm *tm);
char      *ctime(const time_t *t);
double     difftime(time_t a, time_t b);

#endif
