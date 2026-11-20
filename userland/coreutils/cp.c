/* rozOS — cp src dst, cp src... dir: ফাইল কপি */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

static int copy_file(const char *src, const char *dst)
{
    char buf[16384];
    ssize_t n;
    int in, out, r = 0;
    struct stat st;

    if (stat(src, &st) == 0 && S_ISDIR(st.st_mode)) {
        fprintf(stderr, "cp: %s: is a directory\n", src);
        return 1;
    }
    in = open(src, O_RDONLY);
    if (in < 0) { fprintf(stderr, "cp: %s: %s\n", src, strerror(errno)); return 1; }
    out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { fprintf(stderr, "cp: %s: %s\n", dst, strerror(errno)); close(in); return 1; }
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, (size_t)n) != n) {
            fprintf(stderr, "cp: %s: %s\n", dst, strerror(errno));
            r = 1;
            break;
        }
    }
    if (n < 0) { fprintf(stderr, "cp: %s: %s\n", src, strerror(errno)); r = 1; }
    close(in);
    close(out);
    return r;
}

int main(int argc, char **argv)
{
    struct stat st;
    const char *dst;
    int i, r = 0, to_dir;

    if (argc < 3) { fprintf(stderr, "usage: cp <src> <dst>  |  cp <src>... <dir>\n"); return 2; }
    dst = argv[argc - 1];
    to_dir = stat(dst, &st) == 0 && S_ISDIR(st.st_mode);
    if (argc > 3 && !to_dir) { fprintf(stderr, "cp: %s: not a directory\n", dst); return 1; }
    for (i = 1; i < argc - 1; i++) {
        if (to_dir) {
            char path[1024];
            const char *base = strrchr(argv[i], '/');
            snprintf(path, sizeof(path), "%s/%s", dst, base ? base + 1 : argv[i]);
            r |= copy_file(argv[i], path);
        } else {
            r |= copy_file(argv[i], dst);
        }
    }
    return r;
}
