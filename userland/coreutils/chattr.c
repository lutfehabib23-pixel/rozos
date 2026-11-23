/* rozOS — chattr: ফাইলের flag বদলানো (ধাপ ১৯ছ)
 *
 *   chattr +i FILE...    immutable — মোছা, নাম বদল, লেখা যায় না (root-ও নয়)
 *   chattr -i FILE...    আবার সাধারণ
 *
 * Linux-এর chattr-এর মতো, কিন্তু শুধু 'i'। ext2 আর tmpfs-এ চলে; FAT-এ "not supported"।
 * লক করা ফোল্ডারের .rozlock ফাইল rozlock নিজেই +i করে রাখে। */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

int main(int argc, char **argv)
{
    int i, on, rc = 0;
    if (argc < 3 || (argv[1][0] != '+' && argv[1][0] != '-') || strcmp(argv[1] + 1, "i")) {
        fprintf(stderr, "usage: chattr +i|-i FILE...\n");
        return 2;
    }
    on = argv[1][0] == '+';
    for (i = 2; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY), fl = 0;
        if (fd < 0 || ioctl(fd, FS_IOC_GETFLAGS, &fl) < 0 ||
            (fl = on ? fl | FS_IMMUTABLE_FL : fl & ~FS_IMMUTABLE_FL, ioctl(fd, FS_IOC_SETFLAGS, &fl) < 0)) {
            fprintf(stderr, "chattr: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
        }
        if (fd >= 0) close(fd);
    }
    return rc;
}
