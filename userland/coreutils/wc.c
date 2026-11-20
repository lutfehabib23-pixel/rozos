/* rozOS — wc [file...]: লাইন, শব্দ, বাইট গোনা */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>

static long tl, tw, tb;

static int count(int fd, const char *name)
{
    char buf[4096];
    ssize_t n;
    long l = 0, w = 0, b = 0;
    int in_word = 0, i;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (i = 0; i < n; i++) {
            if (buf[i] == '\n') l++;
            if (isspace((unsigned char)buf[i])) in_word = 0;
            else if (!in_word) { in_word = 1; w++; }
        }
        b += n;
    }
    if (n < 0) { fprintf(stderr, "wc: %s: %s\n", name, strerror(errno)); return 1; }
    printf("%7ld %7ld %7ld %s\n", l, w, b, name);
    tl += l; tw += w; tb += b;
    return 0;
}

int main(int argc, char **argv)
{
    int i, r = 0;
    if (argc < 2) return count(0, "");
    for (i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) { fprintf(stderr, "wc: %s: %s\n", argv[i], strerror(errno)); r = 1; continue; }
        r |= count(fd, argv[i]);
        close(fd);
    }
    if (argc > 2) printf("%7ld %7ld %7ld total\n", tl, tw, tb);
    return r;
}
