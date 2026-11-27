/* rozlibc — sys/time.h */
#ifndef _SYS_TIME_H
#define _SYS_TIME_H
#ifdef __cplusplus
extern "C" {
#endif

#include <time.h>

#ifndef _ROZLIBC_TIMEVAL
#define _ROZLIBC_TIMEVAL
struct timeval {
    time_t      tv_sec;
    suseconds_t tv_usec;
};
#endif

int gettimeofday(struct timeval *tv, void *tz);

/* ধাপ ১৬ক */
struct timezone { int tz_minuteswest, tz_dsttime; };
#define timerisset(t)    ((t)->tv_sec || (t)->tv_usec)
#define timerclear(t)    ((t)->tv_sec = (t)->tv_usec = 0)
#define timercmp(a, b, op) ((a)->tv_sec == (b)->tv_sec ? (a)->tv_usec op (b)->tv_usec : (a)->tv_sec op (b)->tv_sec)
#define timeradd(a, b, r) do { (r)->tv_sec = (a)->tv_sec + (b)->tv_sec; \
    (r)->tv_usec = (a)->tv_usec + (b)->tv_usec; \
    if ((r)->tv_usec >= 1000000) { (r)->tv_sec++; (r)->tv_usec -= 1000000; } } while (0)
#define timersub(a, b, r) do { (r)->tv_sec = (a)->tv_sec - (b)->tv_sec; \
    (r)->tv_usec = (a)->tv_usec - (b)->tv_usec; \
    if ((r)->tv_usec < 0) { (r)->tv_sec--; (r)->tv_usec += 1000000; } } while (0)
int settimeofday(const struct timeval *tv, const void *tz);
/* ধাপ ১৬খ-১: setitimer — ITIMER_REAL (SIGALRM) */
struct itimerval {
    struct timeval it_interval;
    struct timeval it_value;
};
#define ITIMER_REAL    0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF    2
int setitimer(int which, const struct itimerval *nv, struct itimerval *old);
int getitimer(int which, struct itimerval *v);
int utimes(const char *path, const struct timeval tv[2]);
#include <sys/select.h>

#ifdef __cplusplus
}
#endif
#endif
