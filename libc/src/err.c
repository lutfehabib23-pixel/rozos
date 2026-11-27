/* rozlibc — err.h: warn, warnx, err, errx, v* (আগে extra.c-তে; ধাপ ১৭ক-এ আলাদা ফাইল)
 * আলাদা object যাতে যে প্রোগ্রাম নিজের warn()/err() সংজ্ঞা দেয় (binutils) তার সাথে না বাধে। */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <err.h>

extern char *__progname;

void vwarn(const char *fmt, va_list ap)
{
    int e = errno;
    fprintf(stderr, "%s: ", __progname);
    if (fmt) { vfprintf(stderr, fmt, ap); fputs(": ", stderr); }
    fprintf(stderr, "%s\n", strerror(e));
}

void vwarnx(const char *fmt, va_list ap)
{
    fprintf(stderr, "%s: ", __progname);
    if (fmt) vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

void verr(int code, const char *fmt, va_list ap) { vwarn(fmt, ap); exit(code); }
void verrx(int code, const char *fmt, va_list ap) { vwarnx(fmt, ap); exit(code); }
void warn(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vwarn(fmt, ap); va_end(ap); }
void warnx(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vwarnx(fmt, ap); va_end(ap); }
void err(int code, const char *fmt, ...) { va_list ap; va_start(ap, fmt); vwarn(fmt, ap); va_end(ap); exit(code); }
void errx(int code, const char *fmt, ...) { va_list ap; va_start(ap, fmt); vwarnx(fmt, ap); va_end(ap); exit(code); }
