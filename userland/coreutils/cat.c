/* rozOS — cat [file...]: ফাইল (বা stdin) stdout-এ */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static int copy(int fd, const char *name)
{
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        if (write(1, buf, (size_t)n) != n) return 1;
    }
    if (n < 0) {
        fprintf(stderr, "cat: %s: %s\n", name, strerror(errno));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int i, r = 0;
    if (argc < 2) return copy(0, "stdin");
    for (i = 1; i < argc; i++) {
        int fd = strcmp(argv[i], "-") ? open(argv[i], O_RDONLY) : 0;
        if (fd < 0) {
            fprintf(stderr, "cat: %s: %s\n", argv[i], strerror(errno));
            r = 1;
            continue;
        }
        r |= copy(fd, argv[i]);
        if (fd) close(fd);
    }
    return r;
}
