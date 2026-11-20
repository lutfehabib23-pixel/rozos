/* rozOS — wc [-l] [-w] [-c] [file...]: লাইন, শব্দ, বাইট গোনা
 * ধাপ ১৩: -l / -w / -c (একটা বা কয়েকটা) — শুধু সেগুলো ছাপে; pipe থেকে পড়লে নাম ছাড়া
 * (ls /bin | wc -l) */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>

static long tl, tw, tb;
static int show_l = 1, show_w = 1, show_c = 1;

static void print_counts(long l, long w, long b, const char *name)
{
    int first = 1;
    int only = (show_l + show_w + show_c) == 1;
    if (show_l) { printf(only ? "%ld" : "%7ld", l); first = 0; }
    if (show_w) { printf(only ? "%s%ld" : "%s%7ld", first ? "" : " ", w); first = 0; }
    if (show_c) { printf(only ? "%s%ld" : "%s%7ld", first ? "" : " ", b); }
    if (name && name[0]) printf(" %s", name);
    printf("\n");
}

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
    print_counts(l, w, b, name);
    tl += l; tw += w; tb += b;
    return 0;
}

int main(int argc, char **argv)
{
    int i, r = 0, nfiles = 0, first_file = 0;
    int any = 0, l = 0, w = 0, c = 0;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || !a[1]) break;
        if (!strcmp(a, "--")) { i++; break; }
        for (a++; *a; a++) {
            if (*a == 'l') l = 1;
            else if (*a == 'w') w = 1;
            else if (*a == 'c') c = 1;
            else {
                fprintf(stderr, "usage: wc [-l] [-w] [-c] [file...]\n");
                return 2;
            }
            any = 1;
        }
    }
    if (any) { show_l = l; show_w = w; show_c = c; }
    first_file = i;
    nfiles = argc - first_file;
    if (nfiles == 0) return count(0, "");
    for (i = first_file; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) { fprintf(stderr, "wc: %s: %s\n", argv[i], strerror(errno)); r = 1; continue; }
        r |= count(fd, argv[i]);
        close(fd);
    }
    if (nfiles > 1) print_counts(tl, tw, tb, "total");
    return r;
}
