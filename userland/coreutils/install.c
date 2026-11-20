/* rozOS — install: নিজের প্রোগ্রাম বসানো, PATH আর মেনুতে আনা (ধাপ ১২)
 *
 *   install [-h] [-t "Title"] [-c Category] [-a "about"] [-w] [-n] PROGRAM
 *       PROGRAM → /usr/local/bin/<নাম> (-h হলে ~/bin = /home/bin), আর মেনুর জন্য
 *       /usr/share/apps/<নাম>.app (-n হলে মেনুতে নয়)। -w: শেষ হলে মেনু অপেক্ষা করবে
 *       (কমান্ড-লাইনের প্রোগ্রামের output পড়ার জন্য)।
 *   install -u NAME          সরানো (প্রোগ্রাম আর মেনুর এন্ট্রি)
 *   install -l               বসানো প্রোগ্রাম আর মেনুর এন্ট্রি */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#define SYS_BIN  "/usr/local/bin"
#define HOME_BIN "/home/bin"
#define APPS     "/usr/share/apps"

static int copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb"), *out;
    char buf[8192];
    size_t n;
    if (!in) return -1;
    out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { fclose(in); fclose(out); return -1; }
    }
    fclose(in);
    if (fclose(out) != 0) return -1;
    chmod(dst, 0755);
    return 0;
}

static int is_elf(const char *path)
{
    FILE *f = fopen(path, "rb");
    unsigned char h[4] = { 0, 0, 0, 0 };
    if (!f) return 0;
    fread(h, 1, 4, f);
    fclose(f);
    return h[0] == 0x7F && h[1] == 'E' && h[2] == 'L' && h[3] == 'F';
}

static int name_ok(const char *n)
{
    if (!*n || strlen(n) > 60) return 0;
    for (; *n; n++) if (!(isalnum((unsigned char)*n) || *n == '-' || *n == '_' || *n == '.')) return 0;
    return 1;
}

static int cmd_list(void)
{
    const char *dirs[2] = { SYS_BIN, HOME_BIN };
    int i, found = 0;
    DIR *d;
    struct dirent *e;
    for (i = 0; i < 2; i++) {
        if (!(d = opendir(dirs[i]))) continue;
        while ((e = readdir(d))) {
            char app[256];
            struct stat st;
            if (e->d_name[0] == '.') continue;
            snprintf(app, sizeof(app), APPS "/%s.app", e->d_name);
            printf("  %s/%-20s %s\n", dirs[i], e->d_name, stat(app, &st) == 0 ? "(in the menu)" : "");
            found++;
        }
        closedir(d);
    }
    if (!found) printf("nothing installed yet — e.g.: tcc calc.c -o calc && install calc\n");
    return 0;
}

static int cmd_remove(const char *name)
{
    char path[256];
    int removed = 0;
    if (!name_ok(name)) { fprintf(stderr, "install: bad name '%s'\n", name); return 2; }
    snprintf(path, sizeof(path), SYS_BIN "/%s", name);
    if (unlink(path) == 0) { printf("removed %s\n", path); removed++; }
    snprintf(path, sizeof(path), HOME_BIN "/%s", name);
    if (unlink(path) == 0) { printf("removed %s\n", path); removed++; }
    snprintf(path, sizeof(path), APPS "/%s.app", name);
    if (unlink(path) == 0) { printf("removed the menu entry %s\n", path); removed++; }
    if (!removed) { fprintf(stderr, "install: '%s' is not installed\n", name); return 1; }
    return 0;
}

static void usage(void)
{
    fprintf(stderr, "usage: install [-h] [-t \"Title\"] [-c Category] [-a \"about\"] [-w] [-n] PROGRAM\n"
                    "       install -u NAME      remove\n"
                    "       install -l           list\n"
                    "  -h  into ~/bin (/home/bin) instead of /usr/local/bin\n"
                    "  -t  menu title (default: the name)   -c  menu category (default: My programs)\n"
                    "  -a  one line about it   -w  the menu waits for a key when it ends   -n  no menu entry\n"
                    "see: help install\n");
}

int main(int argc, char **argv)
{
    const char *title = NULL, *cat = "My programs", *about = "", *src = NULL, *base;
    int home = 0, wait = 0, nomenu = 0, i;
    char name[64], dst[256], app[256];
    FILE *f;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l")) return cmd_list();
        if (!strcmp(argv[i], "-u") && i + 1 < argc) return cmd_remove(argv[i + 1]);
        if (!strcmp(argv[i], "-h")) home = 1;
        else if (!strcmp(argv[i], "-w")) wait = 1;
        else if (!strcmp(argv[i], "-n")) nomenu = 1;
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) title = argv[++i];
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) cat = argv[++i];
        else if (!strcmp(argv[i], "-a") && i + 1 < argc) about = argv[++i];
        else if (!strcmp(argv[i], "--help")) { usage(); return 0; }
        else if (argv[i][0] == '-') { usage(); return 2; }
        else src = argv[i];
    }
    if (!src) { usage(); return 2; }
    base = strrchr(src, '/');
    base = base ? base + 1 : src;
    snprintf(name, sizeof(name), "%s", base);
    if (strlen(name) > 2 && !strcmp(name + strlen(name) - 2, ".c")) {
        fprintf(stderr, "install: %s is C source — compile it first:\n  tcc %s -o %.*s && install %.*s\n",
                src, src, (int)strlen(name) - 2, name, (int)strlen(name) - 2, name);
        return 1;
    }
    if (!name_ok(name)) { fprintf(stderr, "install: bad program name '%s'\n", name); return 2; }
    if (!is_elf(src)) {
        fprintf(stderr, "install: %s: not a program (ELF file)%s\n", src, errno == ENOENT ? " — file not found" : "");
        return 1;
    }
    mkdir(home ? HOME_BIN : SYS_BIN, 0755);
    snprintf(dst, sizeof(dst), "%s/%s", home ? HOME_BIN : SYS_BIN, name);
    unlink(dst);
    if (copy_file(src, dst) < 0) {
        fprintf(stderr, "install: %s: %s\n", dst, strerror(errno));
        return 1;
    }
    printf("installed %s — run it with: %s\n", dst, name);
    if (!nomenu) {
        mkdir(APPS, 0755);
        snprintf(app, sizeof(app), APPS "/%s.app", name);
        f = fopen(app, "w");
        if (!f) {
            fprintf(stderr, "install: %s: %s\n", app, strerror(errno));
            return 1;
        }
        fprintf(f, "# rozOS menu entry (install লিখেছে) — help menu\n");
        fprintf(f, "name: %s\ncategory: %s\nrun: %s\nabout: %s\nwait: %s\n", title ? title : name, cat, dst,
                about, wait ? "yes" : "no");
        fclose(f);
        printf("added to the menu (%s → %s): %s\n", cat, title ? title : name, app);
    }
    return 0;
}
