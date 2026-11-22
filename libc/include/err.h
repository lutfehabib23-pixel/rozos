/* rozlibc — err.h (ধাপ ১৬ক, BSD): "প্রোগ্রাম: বার্তা: strerror" লিখে (err) বেরিয়ে যাওয়া */
#ifndef _ERR_H
#define _ERR_H
#include <stdarg.h>

void err(int code, const char *fmt, ...) __attribute__((noreturn));
void errx(int code, const char *fmt, ...) __attribute__((noreturn));
void warn(const char *fmt, ...);
void warnx(const char *fmt, ...);
void verr(int code, const char *fmt, va_list ap) __attribute__((noreturn));
void verrx(int code, const char *fmt, va_list ap) __attribute__((noreturn));
void vwarn(const char *fmt, va_list ap);
void vwarnx(const char *fmt, va_list ap);

#endif
