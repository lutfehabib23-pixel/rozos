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
#include <sys/stat.h>   /* ধাপ ১৯ছ: fdopendir */
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
    /* ধাপ ১৬ক */
    case ENOTBLK: return "block device required";
    case ETXTBSY: return "text file busy";
    case EMLINK: return "too many links";
    case EDEADLK: return "resource deadlock would occur";
    case ENOLCK: return "no locks available";
    case ELOOP: return "too many symbolic links";
    case ENOMSG: return "no message of desired type";
    case EIDRM: return "identifier removed";
    case ENOSTR: return "device not a stream";
    case ENODATA: return "no data available";
    case ETIME: return "timer expired";
    case ENOSR: return "out of streams resources";
    case ENOLINK: return "link has been severed";
    case EPROTO: return "protocol error";
    case EBADMSG: return "bad message";
    case EOVERFLOW: return "value too large for data type";
    case EILSEQ: return "invalid or incomplete multibyte character";
    case EUSERS: return "too many users";
    case ESOCKTNOSUPPORT: return "socket type not supported";
    case EPFNOSUPPORT: return "protocol family not supported";
    case ENETRESET: return "connection reset by network";
    case ESHUTDOWN: return "cannot send after shutdown";
    case ETOOMANYREFS: return "too many references";
    case EHOSTDOWN: return "host is down";
    case ESTALE: return "stale file handle";
    case EDQUOT: return "disk quota exceeded";
    case ECANCELED: return "operation canceled";
    case EOWNERDEAD: return "owner died";
    case ENOTRECOVERABLE: return "state not recoverable";
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

/* ধাপ ১৯ছ: fd থেকে DIR — closedir() fd-ও বন্ধ করে (POSIX) */
DIR *fdopendir(int fd)
{
    DIR *d;
    struct stat st;
    if (fstat(fd, &st) != 0) return NULL;
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return NULL; }
    d = (DIR *)malloc(sizeof(*d));
    if (!d) { errno = ENOMEM; return NULL; }
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

/* ---------- সময়ের ফাংশন ধাপ ১৬ক থেকে libc/src/time.c-তে (TZ সহ) ---------- */

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
