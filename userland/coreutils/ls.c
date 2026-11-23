/* rozOS — ls [-l] [-a] [-1] [path...]: ডিরেক্টরির তালিকা (ডিরেক্টরি নীল, প্রোগ্রাম সবুজ)
 * ধাপ ১৩: -1 = প্রতি লাইনে একটা নাম; আউটপুট টার্মিনাল না হলে (pipe/ফাইল) নিজে থেকেই তাই,
 * শুধু নাম (রং বা '/' ছাড়া) — যেমন ls /bin | wc -l
 * ধাপ ১৯ছ: ফোল্ডারের .hidden তালিকার নামও লুকানো থাকে (-a দিলে দেখায়) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <rozlock.h>       /* ধাপ ১৯ছ: rozhide_* */

static int opt_long, opt_all, opt_one, color;

static int cmp_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void print_entry(const char *dir, const char *name)
{
    char path[1024];
    struct stat st;
    const char *pre = "", *post = "";
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (stat(path, &st) < 0) memset(&st, 0, sizeof(st));
    if (color) {
        if (S_ISDIR(st.st_mode)) { pre = "\x1b[94m"; post = "\x1b[0m"; }
        else if (S_ISCHR(st.st_mode)) { pre = "\x1b[93m"; post = "\x1b[0m"; }
        else if (st.st_mode & 0111) { pre = "\x1b[92m"; post = "\x1b[0m"; }
    }
    if (opt_long) {
        char when[32];
        time_t t = st.st_mtime;
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M", gmtime(&t));
        printf("%c%c%c%c%c%c%c%c%c%c %3lu %9ld %s %s%s%s%s\n",
               S_ISDIR(st.st_mode) ? 'd' : S_ISCHR(st.st_mode) ? 'c' : '-',
               st.st_mode & 0400 ? 'r' : '-', st.st_mode & 0200 ? 'w' : '-', st.st_mode & 0100 ? 'x' : '-',
               st.st_mode & 040 ? 'r' : '-', st.st_mode & 020 ? 'w' : '-', st.st_mode & 010 ? 'x' : '-',
               st.st_mode & 04 ? 'r' : '-', st.st_mode & 02 ? 'w' : '-', st.st_mode & 01 ? 'x' : '-',
               (unsigned long)st.st_nlink, (long)st.st_size, when, pre, name, post,
               S_ISDIR(st.st_mode) ? "/" : "");
    } else if (opt_one) {
        if (color) printf("%s%s%s%s\n", pre, name, post, S_ISDIR(st.st_mode) ? "/" : "");
        else printf("%s\n", name);
    } else {
        printf("%s%s%s%s  ", pre, name, post, S_ISDIR(st.st_mode) ? "/" : "");
    }
}

static int list(const char *path, int header)
{
    struct stat st;
    DIR *d;
    struct dirent *e;
    char **names = NULL;
    struct rozhide hid = { NULL, NULL, 0 };   /* ধাপ ১৯ছ */
    int n = 0, cap = 0, i;

    if (stat(path, &st) < 0) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (!S_ISDIR(st.st_mode)) {
        const char *slash = strrchr(path, '/');
        char dir[1024];
        if (slash) { snprintf(dir, sizeof(dir), "%.*s", (int)(slash - path), path); if (!dir[0]) strcpy(dir, "/"); }
        else strcpy(dir, ".");
        print_entry(dir, slash ? slash + 1 : path);
        if (!opt_long) printf("\n");
        return 0;
    }
    d = opendir(path);
    if (!d) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (!opt_all) rozhide_load(&hid, path);
    while ((e = readdir(d))) {
        if (!opt_all && e->d_name[0] == '.') continue;
        if (!opt_all && rozhide_has(&hid, e->d_name)) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            names = (char **)realloc(names, sizeof(char *) * (size_t)cap);
        }
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    rozhide_free(&hid);
    qsort(names, (size_t)n, sizeof(char *), cmp_name);
    if (header) printf("%s:\n", path);
    for (i = 0; i < n; i++) {
        print_entry(path, names[i]);
        free(names[i]);
    }
    if (!opt_long && !opt_one && n) printf("\n");
    free(names);
    return 0;
}

int main(int argc, char **argv)
{
    int c, r = 0, i;
    while ((c = getopt(argc, argv, "la1")) != -1) {
        if (c == 'l') opt_long = 1;
        else if (c == 'a') opt_all = 1;
        else if (c == '1') opt_one = 1;
        else return 2;
    }
    color = isatty(1);
    if (!color) opt_one = 1;              /* ধাপ ১৩: pipe/ফাইলে প্রতি লাইনে একটা */
    if (optind >= argc) return list(".", 0);
    for (i = optind; i < argc; i++) {
        r |= list(argv[i], argc - optind > 1);
        if (argc - optind > 1 && i + 1 < argc) printf("\n");
    }
    return r;
}
