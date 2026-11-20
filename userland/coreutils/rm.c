/* rozOS — rm [-r] [-f] path...: ফাইল মোছা (-r: ডিরেক্টরি সহ ভেতরের সব, -f: না থাকলে চুপ) */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>

static int recursive, force;

static int remove_path(const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0) {
        if (force && errno == ENOENT) return 0;
        fprintf(stderr, "rm: %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR *d;
        struct dirent *e;
        int r = 0;
        if (!recursive) {
            fprintf(stderr, "rm: %s: is a directory (use -r)\n", path);
            return 1;
        }
        d = opendir(path);
        if (!d) { fprintf(stderr, "rm: %s: %s\n", path, strerror(errno)); return 1; }
        /* মোছার সময় তালিকা বদলায় — প্রতিবার নতুন করে প্রথম থেকে পড়া */
        for (;;) {
            char child[1024];
            int found = 0;
            rewinddir(d);
            while ((e = readdir(d))) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
                found = 1;
                break;
            }
            if (!found) break;
            if (remove_path(child)) { r = 1; break; }
        }
        closedir(d);
        if (r) return r;
        if (rmdir(path) < 0) { fprintf(stderr, "rm: %s: %s\n", path, strerror(errno)); return 1; }
        return 0;
    }
    if (unlink(path) < 0) {
        fprintf(stderr, "rm: %s: %s\n", path, strerror(errno));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int c, r = 0, i;
    while ((c = getopt(argc, argv, "rRf")) != -1) {
        if (c == 'r' || c == 'R') recursive = 1;
        else if (c == 'f') force = 1;
        else return 2;
    }
    if (optind >= argc) { fprintf(stderr, "usage: rm [-r] [-f] <path>...\n"); return 2; }
    for (i = optind; i < argc; i++) r |= remove_path(argv[i]);
    return r;
}
