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
