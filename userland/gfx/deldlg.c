/* rozOS — deldlg: ফাইল বা ফোল্ডার মোছার আগে "Are you sure?" জিজ্ঞাসা (ধাপ ২৪ক)
 *
 *   deldlg PATH      Yes চাপলে PATH মোছে (ফোল্ডার হলে ভেতরের সবকিছুসহ)
 *
 * ডেস্কটপের ডান-ক্লিক মেনুর "Delete..." এটা চালায় (Files-এর Delete-এর মতোই কাজ)।
 * তালা দেওয়া ফোল্ডার (.rozlock) মোছা যায় না — আগে Unlock করতে হয়।
 * ফেরত: 0 = মোছা হয়েছে, 1 = বাতিল, 2 = ভুল। */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <rozgfx.h>
#include <rozui.h>
#include <rozlock.h>

static int remove_tree(const char *p)
{
    struct stat st;
    if (lstat(p, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(p);
        struct dirent *de;
        static char names[64][256];
        int n, i, more = 1, rc = 0;
        /* Files-এর মতো: একবারে ৬৪টা নাম পড়ে মোছা (মুছতে মুছতে readdir নয়) */
        while (more && d) {
            n = 0;
            rewinddir(d);
            while ((de = readdir(d)) && n < 64) {
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
                snprintf(names[n++], 256, "%s", de->d_name);
            }
            more = n > 0;
            for (i = 0; i < n; i++) {
                char c[1300];
                snprintf(c, sizeof(c), "%s/%s", p, names[i]);
                if (rozlock_is_lockfile(c)) { errno = EPERM; rc = -1; more = 0; break; }
                if (remove_tree(c) < 0) { rc = -1; more = 0; break; }
            }
        }
        if (d) closedir(d);
        if (rc < 0) return -1;
        return rmdir(p);
    }
    return unlink(p);
}

int main(int argc, char **argv)
{
    char m[700], shown[256];
    const char *path, *base;
    struct stat st;
    int isdir, r;

    if (argc < 2) {
        fprintf(stderr, "usage: deldlg PATH\n");
        return 2;
    }
    path = argv[1];
    if (lstat(path, &st) != 0) {
        fprintf(stderr, "deldlg: %s: %s\n", path, strerror(errno));
        return 2;
    }
    isdir = S_ISDIR(st.st_mode);
    base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(shown, sizeof(shown), "%.60s", base);
    /* ধাপ ২৪খ: আলাদা উইন্ডোর ভেতরে বাক্স নয় — বার্তাটাই নিজের উইন্ডো (ui_msgbox_window) */
    if (rozlock_is_lockfile(path)) {
        rozlock_display_name(path, shown, sizeof(shown));
        ui_msgbox_window("Delete", "A locked folder cannot be deleted.\nUnlock it first.", UI_MB_OK | UI_MB_WARN);
        return 1;
    }
    snprintf(m, sizeof(m), isdir ? "Are you sure you want to delete the folder\n\"%s\" and everything in it?"
                                 : "Are you sure you want to delete\n\"%s\"?", shown);
    if (ui_msgbox_window("Delete", m, UI_MB_YESNO | UI_MB_WARN) != UI_YES) {
        return 1;
    }
    r = remove_tree(path);
    if (r != 0) {
        if (errno == EPERM) snprintf(m, sizeof(m), "Cannot delete everything:\nthere is a locked folder inside.\nUnlock it first.");
        else snprintf(m, sizeof(m), "Cannot delete \"%s\":\n%s", shown, strerror(errno));
        ui_msgbox_window("Delete", m, UI_MB_OK | UI_MB_ERROR);
        return 2;
    }
    return 0;
}
