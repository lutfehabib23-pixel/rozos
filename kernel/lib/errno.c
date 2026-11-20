/* rozOS — ত্রুটি কোডের বার্তা */

#include <rozos/errno.h>

const char *kstrerror(int err)
{
    if (err < 0) {
        err = -err;
    }
    switch (err) {
    case 0:            return "success";
    case EPERM:        return "operation not permitted";
    case ESRCH:        return "no such process";
    case EINTR:        return "interrupted";
    case E2BIG:        return "argument list too long";
    case ENOEXEC:      return "exec format error";
    case ECHILD:       return "no child processes";
    case EFAULT:       return "bad address";
    case ENOTTY:       return "not a terminal";
    case ERANGE:       return "result out of range";
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
    case ENOENT:       return "no such file or directory";
    case EIO:          return "I/O error";
    case EBADF:        return "bad file descriptor";
    case EAGAIN:       return "try again";
    case ENOMEM:       return "out of memory";
    case EACCES:       return "permission denied";
    case EBUSY:        return "device or resource busy";
    case EEXIST:       return "file exists";
    case EXDEV:        return "cross-device link";
    case ENODEV:       return "no such device";
    case ENOTDIR:      return "not a directory";
    case EISDIR:       return "is a directory";
    case EINVAL:       return "invalid argument";
    case ENFILE:       return "file table overflow";
    case EMFILE:       return "too many open files";
    case EFBIG:        return "file too large";
    case ENOSPC:       return "no space left";
    case ESPIPE:       return "illegal seek";
    case EROFS:        return "read-only file system";
    case ENAMETOOLONG: return "file name too long";
    case ENOSYS:       return "function not implemented";
    case ENOTEMPTY:    return "directory not empty";
    default:           return "unknown error";
    }
}
