/* rozlibc — time.h */
#ifndef _TIME_H
#define _TIME_H

#include <_rozlibc.h>

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1
#define CLOCK_PROCESS_CPUTIME_ID 2      /* ধাপ ১৬ক: rozOS-এ MONOTONIC-এর সমান */
#define CLOCK_THREAD_CPUTIME_ID  3
#define CLOCK_MONOTONIC_RAW      4
#define CLOCK_REALTIME_COARSE    5
#define CLOCK_MONOTONIC_COARSE   6
#define CLOCK_BOOTTIME           7
#define TIMER_ABSTIME            1
#define TIME_UTC                 1
#define CLOCKS_PER_SEC  1000000L

typedef int clockid_t;

struct timespec {
    time_t tv_sec;
    long   tv_nsec;
};

struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
    long tm_gmtoff;              /* ধাপ ১৬ক: UTC থেকে কত সেকেন্ড পূর্বে */
    const char *tm_zone;         /* ধাপ ১৬ক: অঞ্চলের সংক্ষিপ্ত নাম */
};

time_t     time(time_t *t);
clock_t    clock(void);
int        clock_gettime(clockid_t clk, struct timespec *ts);
int        nanosleep(const struct timespec *req, struct timespec *rem);
struct tm *gmtime(const time_t *t);
struct tm *gmtime_r(const time_t *t, struct tm *out);
struct tm *localtime(const time_t *t);        /* ধাপ ১৬ক: TZ মেনে (না থাকলে /etc/TZ, তারপর UTC) */
time_t     mktime(struct tm *tm);
size_t     strftime(char *buf, size_t size, const char *fmt, const struct tm *tm);
char      *ctime(const time_t *t);
double     difftime(time_t a, time_t b);

/* ---------- ধাপ ১৬ক ---------- */
struct tm *localtime_r(const time_t *t, struct tm *out);
time_t     timegm(struct tm *tm);
char      *asctime(const struct tm *tm);
char      *asctime_r(const struct tm *tm, char *buf);
char      *ctime_r(const time_t *t, char *buf);
char      *strptime(const char *s, const char *fmt, struct tm *tm);
void       tzset(void);
extern char *tzname[2];
extern long timezone;
extern int  daylight;
int        clock_getres(clockid_t clk, struct timespec *ts);
int        clock_settime(clockid_t clk, const struct timespec *ts);
int        clock_nanosleep(clockid_t clk, int flags, const struct timespec *req, struct timespec *rem);
int        timespec_get(struct timespec *ts, int base);

#endif
