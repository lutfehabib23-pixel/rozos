/* rozlibc — sys/socket.h (BSD socket, Linux-এর মান; ধাপ ৯: শুধু IPv4) */
#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H
#ifdef __cplusplus
extern "C" {
#endif

#include <_rozlibc.h>

typedef unsigned int socklen_t;
typedef unsigned short sa_family_t;

struct sockaddr {
    sa_family_t sa_family;
    char sa_data[14];
};

struct sockaddr_storage {
    sa_family_t ss_family;
    char __pad[126];
};

#define AF_UNSPEC 0
#define AF_UNIX   1                  /* ধাপ ১৪: একই কম্পিউটারের প্রোগ্রামের মধ্যে (<sys/un.h>) */
#define AF_LOCAL  AF_UNIX
#define PF_UNIX   AF_UNIX
#define AF_INET   2
#define PF_INET   AF_INET
#define PF_UNSPEC AF_UNSPEC
/* ধাপ ১৭খ: IPv6-এর নাম (GCC-র libcody চায়); কার্নেলে IPv6 নেই — socket() EAFNOSUPPORT দেয় */
#define AF_INET6  10
#define PF_INET6  AF_INET6

#define SOCK_STREAM   1
#define SOCK_DGRAM    2
#define SOCK_RAW      3
#define SOCK_NONBLOCK 0x800
#define SOCK_CLOEXEC  0x80000

#define SOL_SOCKET    1
#define SO_REUSEADDR  2
#define SO_TYPE       3
#define SO_ERROR      4
#define SO_BROADCAST  6
#define SO_KEEPALIVE  9
#define SO_RCVTIMEO   20
#define SO_SNDTIMEO   21

#define MSG_PEEK      0x2
#define MSG_DONTWAIT  0x40
#define MSG_NOSIGNAL  0x4000

#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

#define SOMAXCONN 16

int     socket(int domain, int type, int protocol);
int     connect(int fd, const struct sockaddr *addr, socklen_t len);
int     bind(int fd, const struct sockaddr *addr, socklen_t len);
int     listen(int fd, int backlog);
int     accept(int fd, struct sockaddr *addr, socklen_t *len);
ssize_t send(int fd, const void *buf, size_t len, int flags);
ssize_t sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *to, socklen_t tolen);
ssize_t recv(int fd, void *buf, size_t len, int flags);
ssize_t recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen);
int     shutdown(int fd, int how);
int     getsockname(int fd, struct sockaddr *addr, socklen_t *len);
int     getpeername(int fd, struct sockaddr *addr, socklen_t *len);
int     setsockopt(int fd, int level, int name, const void *val, socklen_t len);
int     getsockopt(int fd, int level, int name, void *val, socklen_t *len);

#ifdef __cplusplus
}
#endif
#endif
