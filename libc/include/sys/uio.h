/* rozlibc — sys/uio.h (ধাপ ১৬ক): readv/writev */
#ifndef _SYS_UIO_H
#define _SYS_UIO_H
#ifdef __cplusplus
extern "C" {
#endif
#include <_rozlibc.h>

#ifndef _ROZLIBC_IOVEC
#define _ROZLIBC_IOVEC
struct iovec {
    void  *iov_base;
    size_t iov_len;
};
#endif
#define UIO_MAXIOV 1024

ssize_t readv(int fd, const struct iovec *iov, int cnt);
ssize_t writev(int fd, const struct iovec *iov, int cnt);
ssize_t preadv(int fd, const struct iovec *iov, int cnt, off_t off);
ssize_t pwritev(int fd, const struct iovec *iov, int cnt, off_t off);

#ifdef __cplusplus
}
#endif
#endif
