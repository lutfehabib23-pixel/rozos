/* rozlibc — sys/un.h: Unix domain socket-এর ঠিকানা (ধাপ ১৪)
 *   struct sockaddr_un a = { AF_UNIX, "/tmp/.rozwm" };
 *   int fd = socket(AF_UNIX, SOCK_STREAM, 0);
 *   connect(fd, (struct sockaddr *)&a, sizeof(a));
 * নামগুলো কার্নেলের তালিকায় থাকে (ডিস্কে ফাইল হয় না); listen করা socket বন্ধ হলে মুছে যায়। */
#ifndef _SYS_UN_H
#define _SYS_UN_H
#ifdef __cplusplus
extern "C" {
#endif

#include <sys/socket.h>

struct sockaddr_un {
    sa_family_t sun_family;          /* AF_UNIX */
    char sun_path[108];
};

#ifdef __cplusplus
}
#endif
#endif
