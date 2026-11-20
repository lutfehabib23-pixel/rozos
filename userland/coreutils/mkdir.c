/* rozOS — mkdir [-p] dir...: ডিরেক্টরি তৈরি (-p: মাঝের সবও, আগে থাকলে ত্রুটি নয়) */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

static int make_parents(char *path)
{
    char *p = path + 1;
    for (;; p++) {
        if (*p == '/' || *p == '\0') {
            char save = *p;
            *p = '\0';
            if (mkdir(path, 0755) < 0 && errno != EEXIST) return -1;
            *p = save;
            if (!save) return 0;
        }
    }
}

int main(int argc, char **argv)
{
    int i = 1, parents = 0, r = 0;
    if (argc > 1 && !strcmp(argv[1], "-p")) { parents = 1; i = 2; }
    if (i >= argc) { fprintf(stderr, "usage: mkdir [-p] <dir>...\n"); return 2; }
    for (; i < argc; i++) {
        int e = parents ? make_parents(argv[i]) : mkdir(argv[i], 0755);
        if (e < 0) {
            fprintf(stderr, "mkdir: %s: %s\n", argv[i], strerror(errno));
            r = 1;
        }
    }
    return r;
}
