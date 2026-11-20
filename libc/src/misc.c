/* rozlibc — strerror, dirent, সময়ের রূপান্তর */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <rozos.h>
#include <limits.h>
#include <inttypes.h>
#include <sys/stat.h>

char *strerror(int err)
{
    static char unknown[32];
    switch (err) {
    case 0:            return "success";
    case EPERM:        return "operation not permitted";
    case ENOENT:       return "no such file or directory";
    case ESRCH:        return "no such process";
    case EINTR:        return "interrupted";
    case EIO:          return "I/O error";
    case E2BIG:        return "argument list too long";
    case ENOEXEC:      return "exec format error";
    case EBADF:        return "bad file descriptor";
    case ECHILD:       return "no child processes";
    case EAGAIN:       return "try again";
    case ENOMEM:       return "out of memory";
    case EACCES:       return "permission denied";
    case EFAULT:       return "bad address";
    case EBUSY:        return "device or resource busy";
    case EEXIST:       return "file exists";
    case EXDEV:        return "cross-device link";
    case ENODEV:       return "no such device";
    case ENOTDIR:      return "not a directory";
    case EISDIR:       return "is a directory";
    case EINVAL:       return "invalid argument";
    case EPIPE:        return "broken pipe";
    case ENFILE:       return "too many open files in system";
    case EMFILE:       return "too many open files";
    case ENOTTY:       return "not a terminal";
    case EFBIG:        return "file too large";
    case ENOSPC:       return "no space left on device";
    case ESPIPE:       return "illegal seek";
    case EROFS:        return "read-only file system";
    case EDOM:         return "math argument out of domain";
    case ERANGE:       return "result out of range";
    case ENAMETOOLONG: return "file name too long";
    case ENOSYS:       return "function not implemented";
    case ENOTEMPTY:    return "directory not empty";
    case ETIMEDOUT:    return "timed out";
    case ENOTSOCK:      return "not a socket";
    case EDESTADDRREQ:  return "destination address required";
    case EMSGSIZE:      return "message too long";
    case EPROTOTYPE:    return "wrong protocol type";
    case ENOPROTOOPT:   return "protocol option not available";
    case EPROTONOSUPPORT: return "protocol not supported";
    case EOPNOTSUPP:    return "operation not supported";
    case EAFNOSUPPORT:  return "address family not supported";
    case EADDRINUSE:    return "address already in use";
    case EADDRNOTAVAIL: return "address not available";
    case ENETDOWN:      return "network is down";
    case ENETUNREACH:   return "network unreachable";
    case ECONNABORTED:  return "connection aborted";
    case ECONNRESET:    return "connection reset by peer";
    case ENOBUFS:       return "no buffer space";
    case EISCONN:       return "already connected";
    case ENOTCONN:      return "not connected";
    case ECONNREFUSED:  return "connection refused";
    case EHOSTUNREACH:  return "host unreachable";
    case EALREADY:      return "operation already in progress";
    case EINPROGRESS:   return "operation in progress";
    }
    snprintf(unknown, sizeof(unknown), "error %d", err);
    return unknown;
}

/* ---------- dirent ---------- */

struct _rozlibc_dir {
    int fd;
    long pos, len;
    char buf[2048];
    struct dirent ent;
};

DIR *opendir(const char *path)
{
    DIR *d;
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return NULL;
    d = (DIR *)malloc(sizeof(*d));
    if (!d) { close(fd); errno = ENOMEM; return NULL; }
    d->fd = fd;
    d->pos = d->len = 0;
    return d;
}

struct dirent *readdir(DIR *d)
{
    unsigned short reclen;
    if (d->pos >= d->len) {
        long n = __syscall_ret(__syscall3(217, d->fd, d->buf, sizeof(d->buf)));
        if (n <= 0) return NULL;
        d->len = n;
        d->pos = 0;
    }
    memcpy(&d->ent.d_ino, d->buf + d->pos, 8);
    memcpy(&d->ent.d_off, d->buf + d->pos + 8, 8);
    memcpy(&reclen, d->buf + d->pos + 16, 2);
    d->ent.d_reclen = reclen;
    d->ent.d_type = (unsigned char)d->buf[d->pos + 18];
    strncpy(d->ent.d_name, d->buf + d->pos + 19, sizeof(d->ent.d_name) - 1);
    d->ent.d_name[sizeof(d->ent.d_name) - 1] = '\0';
    d->pos += reclen;
    return &d->ent;
}

int closedir(DIR *d)
{
    int r = close(d->fd);
    free(d);
    return r;
}

void rewinddir(DIR *d)
{
    lseek(d->fd, 0, SEEK_SET);
    d->pos = d->len = 0;
}

int dirfd(DIR *d) { return d->fd; }

/* ---------- সময় (UTC; rozOS-এ সময় অঞ্চল নেই) ---------- */

static int is_leap(long y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

struct tm *gmtime_r(const time_t *t, struct tm *tm)
{
    long days = *t / 86400, rem = *t % 86400, y = 1970;
    int m;
    if (rem < 0) { rem += 86400; days--; }
    tm->tm_hour = (int)(rem / 3600);
    tm->tm_min = (int)(rem % 3600 / 60);
    tm->tm_sec = (int)(rem % 60);
    tm->tm_wday = (int)((days + 4) % 7);
    if (tm->tm_wday < 0) tm->tm_wday += 7;
    for (;;) {
        long n = is_leap(y) ? 366 : 365;
        if (days >= n) { days -= n; y++; }
        else if (days < 0) { y--; days += is_leap(y) ? 366 : 365; }
        else break;
    }
    tm->tm_year = (int)(y - 1900);
    tm->tm_yday = (int)days;
    for (m = 0; m < 12; m++) {
        int n = mdays[m] + (m == 1 && is_leap(y));
        if (days < n) break;
        days -= n;
    }
    tm->tm_mon = m;
    tm->tm_mday = (int)days + 1;
    tm->tm_isdst = 0;
    return tm;
}

struct tm *gmtime(const time_t *t)
{
    static struct tm tm;
    return gmtime_r(t, &tm);
}

struct tm *localtime(const time_t *t) { return gmtime(t); }

time_t mktime(struct tm *tm)
{
    long y, days = 0;
    int m;
    for (y = 1970; y < 1900 + tm->tm_year; y++) days += is_leap(y) ? 366 : 365;
    for (m = 0; m < tm->tm_mon; m++) days += mdays[m] + (m == 1 && is_leap(1900 + tm->tm_year));
    days += tm->tm_mday - 1;
    return (time_t)days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
}

double difftime(time_t a, time_t b) { return (double)(a - b); }

static const char *const wday_name[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const mon_name[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

/* %Y %m %d %H %M %S %a %b %j %y %e %F %T %% */
size_t strftime(char *buf, size_t size, const char *fmt, const struct tm *tm)
{
    size_t n = 0;
    char tmp[32];
    for (; *fmt && n + 1 < size; fmt++) {
        const char *add = tmp;
        if (*fmt != '%') { buf[n++] = *fmt; continue; }
        fmt++;
        switch (*fmt) {
        case 'Y': snprintf(tmp, sizeof(tmp), "%d", tm->tm_year + 1900); break;
        case 'y': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_year % 100); break;
        case 'm': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mon + 1); break;
        case 'd': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mday); break;
        case 'e': snprintf(tmp, sizeof(tmp), "%2d", tm->tm_mday); break;
        case 'H': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_hour); break;
        case 'M': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_min); break;
        case 'S': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_sec); break;
        case 'j': snprintf(tmp, sizeof(tmp), "%03d", tm->tm_yday + 1); break;
        case 'a': add = wday_name[tm->tm_wday % 7]; break;
        case 'b': add = mon_name[tm->tm_mon % 12]; break;
        case 'F': snprintf(tmp, sizeof(tmp), "%d-%02d-%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday); break;
        case 'T': snprintf(tmp, sizeof(tmp), "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec); break;
        case '%': add = "%"; break;
        default: tmp[0] = '%'; tmp[1] = *fmt; tmp[2] = '\0'; break;
        }
        while (*add && n + 1 < size) buf[n++] = *add++;
        if (!*fmt) break;
    }
    if (size) buf[n] = '\0';
    return n;
}

char *ctime(const time_t *t)
{
    static char buf[32];
    struct tm tm;
    gmtime_r(t, &tm);
    snprintf(buf, sizeof(buf), "%s %s %2d %02d:%02d:%02d %d\n", wday_name[tm.tm_wday],
             mon_name[tm.tm_mon], tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, tm.tm_year + 1900);
    return buf;
}

/* ---------- realpath: পরম পাথ, "." ".." বাদ দিয়ে (symlink নেই) ---------- */

char *realpath(const char *path, char *resolved)
{
    char tmp[PATH_MAX], *out = resolved;
    size_t o = 0;
    const char *p;
    struct stat st;

    if (!path) { errno = EINVAL; return NULL; }
    if (stat(path, &st) < 0) return NULL;
    if (path[0] == '/') {
        tmp[0] = '\0';
    } else if (!getcwd(tmp, sizeof(tmp))) {
        return NULL;
    }
    if (strlen(tmp) + strlen(path) + 2 > sizeof(tmp)) { errno = ENAMETOOLONG; return NULL; }
    strcat(tmp, "/");
    strcat(tmp, path);
    if (!out) {
        out = (char *)malloc(PATH_MAX);
        if (!out) return NULL;
    }
    out[0] = '\0';
    for (p = tmp; *p; ) {
        const char *s;
        size_t n;
        while (*p == '/') p++;
        s = p;
        while (*p && *p != '/') p++;
        n = (size_t)(p - s);
        if (n == 0 || (n == 1 && s[0] == '.')) continue;
        if (n == 2 && s[0] == '.' && s[1] == '.') {
            while (o > 0 && out[o - 1] != '/') o--;
            if (o > 0) o--;
            out[o] = '\0';
            continue;
        }
        out[o++] = '/';
        memcpy(out + o, s, n);
        o += n;
        out[o] = '\0';
    }
    if (o == 0) strcpy(out, "/");
    return out;
}

/* ---------- inttypes ---------- */

intmax_t strtoimax(const char *s, char **end, int base) { return strtoll(s, end, base); }
uintmax_t strtoumax(const char *s, char **end, int base) { return strtoull(s, end, base); }
