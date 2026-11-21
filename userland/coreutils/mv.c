/* rozOS — mv src dst: সরানো / নাম বদল
 * ধাপ ১৫ক: আগে rename() — একই ফাইলসিস্টেমে সঙ্গে সঙ্গে, ফোল্ডারও। অন্য ফাইলসিস্টেমে
 * (EXDEV, যেমন /tmp → /home) আগের মতো কপি করে পুরোনোটা মোছা — শুধু ফাইল। */

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
    int in = open(src, O_RDONLY), out;
    if (in < 0) { fprintf(stderr, "mv: %s: %s\n", src, strerror(errno)); return 1; }
    out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { fprintf(stderr, "mv: %s: %s\n", dst, strerror(errno)); close(in); return 1; }
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, (size_t)n) != n) { close(in); close(out); return 1; }
    }
    close(in);
    close(out);
    return n < 0;
}

int main(int argc, char **argv)
{
    struct stat st;
    char path[1024];
    const char *dst;
    int is_dir;

    if (argc != 3) { fprintf(stderr, "usage: mv <file> <dst>\n"); return 2; }
    if (stat(argv[1], &st) < 0) { fprintf(stderr, "mv: %s: %s\n", argv[1], strerror(errno)); return 1; }
    is_dir = S_ISDIR(st.st_mode);
    dst = argv[2];
    if (stat(dst, &st) == 0 && S_ISDIR(st.st_mode)) {
        const char *base = strrchr(argv[1], '/');
        snprintf(path, sizeof(path), "%s/%s", dst, base ? base + 1 : argv[1]);
        dst = path;
    }
    if (rename(argv[1], dst) == 0) return 0;          /* ধাপ ১৫ক */
    if (errno != EXDEV && errno != ENOSYS) { fprintf(stderr, "mv: %s -> %s: %s\n", argv[1], dst, strerror(errno)); return 1; }
    if (is_dir) { fprintf(stderr, "mv: %s: moving a folder to another file system is not supported (use cp -r)\n", argv[1]); return 1; }
    if (copy_file(argv[1], dst)) return 1;
    if (unlink(argv[1]) < 0) { fprintf(stderr, "mv: %s: %s\n", argv[1], strerror(errno)); return 1; }
    return 0;
}
