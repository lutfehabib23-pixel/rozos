/* rozOS — Files: ফাইল ম্যানেজার (ধাপ ১৪খ)
 *
 *   files [ফোল্ডার]
 *
 * ফোল্ডার ঘোরা (ডাবল-ক্লিক / Enter, Backspace = ওপরে), বাঁ পাশে জায়গার তালিকা (Home, /,
 * Programs ...), ওপরে ঠিকানা লেখার ঘর। ডাবল-ক্লিকে খোলা: ফোল্ডার → ভেতরে, .c/.h/লেখা →
 * Code, ছবি → Images, প্রোগ্রাম → টার্মিনাল-উইন্ডোতে চালানো।
 * নতুন ফোল্ডার/ফাইল, নাম বদল (F2), মোছা (Delete — আগে জিজ্ঞেস করে), কপি/কাট/পেস্ট
 * (Ctrl+C/X/V — ফোল্ডারও, ভেতরের সবসহ), F5 আবার পড়া, "Terminal here"।
 * আইকনে ছবি দেওয়া যায়: /usr/share/icons/folder.png, file.png, c-file.png, image-file.png,
 * program.png (help icons)।
 * ধাপ ১৯ছ: ডান-ক্লিক মেনু (Open, Copy, Cut, Paste, Rename, Delete, Lock/Unlock, Hide/Unhide,
 * Properties)। লক করা ফোল্ডার (NAME.rozlock) তালার আইকনে "NAME" নামে দেখায়; খুললে secret
 * চায় (lockdlg), মোছা/নাম বদল/কাট যায় না। লুকানো = ফোল্ডারের .hidden তালিকা (নাম বদলায় না)। */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <rozgfx.h>
#include <rozui.h>
#include <rozos.h>                              /* ধাপ ২৪গ: rozos_mounts */
#include <rozlock.h>
#include <signal.h>
#include <sys/wait.h>

#define MAXENT   2048
#define ROW_H    22
#define TOOL_H   34
#define ADDR_H   32
#define PLACES_W 150
#define STATUS_H 22

struct ent {
    char *name;
    int isdir;
    long size;
    unsigned mode;
    char kind;                  /* ui_icon চিহ্ন */
    const char *image;          /* /usr/share/icons/<image> */
    const char *type;
    int locked;                 /* ধাপ ১৯ছ: NAME.rozlock */
    int hidden;                 /* .hidden তালিকায় (Hidden files চালু থাকলে দেখায়, ঝাপসা) */
    char *disp;                 /* দেখানো নাম (লক করা: ".rozlock" ছাড়া) */
};

static struct ent E[MAXENT];
static int ne, sel = -1, top;
static char cwd[512], addr[512];
static char hist[16][512];
static int nhist;
static int show_hidden;
static char clip_path[512];
static int clip_cut;
static char status_msg[200];
static ui_ctx U;
static pid_t child_pid;                         /* ধাপ ১৯ছ: চলমান lockdlg */
static char child_keep[256];                    /* শেষ হলে কোন নাম বাছাই থাকবে */

/* ---------- পথ ---------- */
static void join(char *out, size_t size, const char *dir, const char *name)
{
    if (name[0] == '/') snprintf(out, size, "%s", name);
    else snprintf(out, size, "%s/%s", strcmp(dir, "/") ? dir : "", name);
}

static void parent_of(char *p)
{
    char *s = strrchr(p, '/');
    if (!s) return;
    if (s == p) p[1] = '\0';
    else *s = '\0';
}

static int has_ext(const char *n, const char *e)
{
    size_t a = strlen(n), b = strlen(e);
    return a > b && !strcasecmp(n + a - b, e);
}

static void classify(struct ent *e)
{
    const char *n = e->name;
    if (e->isdir) { e->kind = 'f'; e->image = "folder"; e->type = "Folder"; return; }
    if (e->locked) { e->kind = 'l'; e->image = "locked-folder"; e->type = "Locked folder"; return; }
    if (has_ext(n, ".c") || has_ext(n, ".h") || has_ext(n, ".cpp") || has_ext(n, ".cc")) {
        e->kind = 'c'; e->image = "c-file"; e->type = "C source"; return;
    }
    if (has_ext(n, ".png") || has_ext(n, ".jpg") || has_ext(n, ".jpeg") || has_ext(n, ".bmp") ||
        has_ext(n, ".gif") || has_ext(n, ".ppm") || has_ext(n, ".tga")) {
        e->kind = 'i'; e->image = "image-file"; e->type = "Image"; return;
    }
    if (has_ext(n, ".txt") || has_ext(n, ".md") || has_ext(n, ".conf") || has_ext(n, ".app") ||
        has_ext(n, ".mk") || has_ext(n, ".sh") || !strcmp(n, "Makefile") || has_ext(n, ".S") || has_ext(n, ".ld")) {
        e->kind = 'd'; e->image = "file"; e->type = "Text"; return;
    }
    if (e->mode & 0111) { e->kind = 'p'; e->image = "program"; e->type = "Program"; return; }
    e->kind = 'd'; e->image = "file"; e->type = "File";
}

static void free_ents(void)
{
    int i;
    for (i = 0; i < ne; i++) { free(E[i].name); free(E[i].disp); }
    ne = 0;
}

static int ent_cmp(const void *a, const void *b)
{
    const struct ent *x = (const struct ent *)a, *y = (const struct ent *)b;
    int fx = x->isdir || x->locked, fy = y->isdir || y->locked;   /* লক করা ফোল্ডারও ফোল্ডারের সাথে */
    if (fx != fy) return fy - fx;
    return strcasecmp(x->disp ? x->disp : x->name, y->disp ? y->disp : y->name);
}

static void read_dir(const char *keep)
{
    DIR *d = opendir(cwd);
    struct dirent *de;
    struct rozhide hid;
    int i;
    free_ents();
    sel = -1;
    if (!d) {
        snprintf(status_msg, sizeof(status_msg), "Cannot open %s: %s", cwd, strerror(errno));
        return;
    }
    rozhide_load(&hid, cwd);
    while ((de = readdir(d)) && ne < MAXENT) {
        char p[700];
        struct stat st;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..") || !de->d_name[0]) continue;
        if (de->d_name[0] == '.' && !show_hidden) continue;
        if (!strcmp(de->d_name, ".hidden")) continue;          /* ধাপ ১৯ছ: তালিকাটা নিজে নয় */
        if (rozhide_has(&hid, de->d_name) && !show_hidden) continue;
        join(p, sizeof(p), cwd, de->d_name);
        memset(&E[ne], 0, sizeof(E[ne]));
        E[ne].name = strdup(de->d_name);
        if (!E[ne].name) break;
        E[ne].hidden = de->d_name[0] == '.' || rozhide_has(&hid, de->d_name);
        if (stat(p, &st) == 0) {
            E[ne].isdir = S_ISDIR(st.st_mode);
            E[ne].size = (long)st.st_size;
            E[ne].mode = (unsigned)st.st_mode;
        }
        {
            size_t L = strlen(de->d_name), X = strlen(ROZLOCK_EXT);
            if (!E[ne].isdir && L > X && !strcmp(de->d_name + L - X, ROZLOCK_EXT) && rozlock_is_lockfile(p)) {
                E[ne].locked = 1;
                E[ne].disp = strdup(de->d_name);
                if (E[ne].disp) E[ne].disp[L - X] = '\0';
            }
        }
        classify(&E[ne]);
        ne++;
    }
    closedir(d);
    rozhide_free(&hid);
    qsort(E, (size_t)ne, sizeof(E[0]), ent_cmp);
    if (keep)
        for (i = 0; i < ne; i++) if (!strcmp(E[i].name, keep)) sel = i;
    if (top > ne) top = 0;
    snprintf(addr, sizeof(addr), "%s", cwd);
}

static void go(const char *dir, int remember)
{
    char nd[512];
    struct stat st;
    join(nd, sizeof(nd), cwd, dir);
    /* ".." আর শেষের "/" সাফ */
    while (strlen(nd) > 1 && nd[strlen(nd) - 1] == '/') nd[strlen(nd) - 1] = '\0';
    if (stat(nd, &st) != 0 || !S_ISDIR(st.st_mode)) {
        char m[600];
        snprintf(m, sizeof(m), "No such folder:\n%s", nd);
        ui_msgbox("Files", m, UI_MB_OK | UI_MB_ERROR);
        snprintf(addr, sizeof(addr), "%s", cwd);
        return;
    }
    if (remember && strcmp(nd, cwd)) {
        if (nhist == 16) { memmove(hist[0], hist[1], sizeof(hist[0]) * 15); nhist--; }
        snprintf(hist[nhist++], sizeof(hist[0]), "%s", cwd);
    }
    /* /a/b/.. → /a */
    {
        char *p;
        while ((p = strstr(nd, "/..")) && (p[3] == '/' || p[3] == '\0')) {
            char rest[512], *q;
            snprintf(rest, sizeof(rest), "%s", p + 3);
            *p = '\0';
            q = strrchr(nd, '/');
            if (q) *q = '\0';
            strncat(nd, rest, sizeof(nd) - strlen(nd) - 1);
            if (!nd[0]) strcpy(nd, "/");
        }
    }
    snprintf(cwd, sizeof(cwd), "%s", nd);
    top = 0;
    read_dir(NULL);
    status_msg[0] = '\0';
}

static void go_up(void)
{
    char p[512], name[256];
    const char *s;
    if (!strcmp(cwd, "/")) return;
    s = strrchr(cwd, '/');
    snprintf(name, sizeof(name), "%s", s ? s + 1 : "");
    snprintf(p, sizeof(p), "%s", cwd);
    parent_of(p);
    go(p, 1);
    read_dir(name);                              /* যে ফোল্ডার থেকে এলাম সেটা বাছাই করা */
}

static void go_back(void)
{
    if (nhist == 0) return;
    nhist--;
    go(hist[nhist], 0);
}

/* ---------- কাজ ---------- */
static void launch2(const char *prog, const char *arg)
{
    const char *argv[3];
    argv[0] = prog;
    argv[1] = arg;
    argv[2] = NULL;
    if (ui_launch(argv, cwd) < 0) snprintf(status_msg, sizeof(status_msg), "Cannot start %s", prog);
    else snprintf(status_msg, sizeof(status_msg), "Opening with %s", prog);
}

static void run_in_terminal(const char *p)
{
    const char *argv[5];
    argv[0] = "rozterm"; argv[1] = "-w"; argv[2] = "-e"; argv[3] = p; argv[4] = NULL;
    ui_launch(argv, cwd);
    snprintf(status_msg, sizeof(status_msg), "Running %s", p);
}

/* ধাপ ১৯ছ: lockdlg চালানো — Files চলতে থাকে, শেষ হলে তালিকা আবার পড়া (frame()-এ দেখা হয়) */
static void run_lockdlg(const char *mode, const char *path, const char *keep)
{
    pid_t pid;
    if (child_pid > 0) { snprintf(status_msg, sizeof(status_msg), "Please finish the other lock window first"); return; }
    pid = fork();
    if (pid < 0) { snprintf(status_msg, sizeof(status_msg), "Cannot start lockdlg"); return; }
    if (pid == 0) {
        int fd;
        for (fd = 3; fd < 64; fd++) close(fd);
        execlp("lockdlg", "lockdlg", mode, path, (char *)NULL);
        _exit(127);
    }
    child_pid = pid;
    snprintf(child_keep, sizeof(child_keep), "%s", keep ? keep : "");
    U.tick_ms = 300;
    snprintf(status_msg, sizeof(status_msg), "%s ...", mode[0] == 'l' ? "Locking" : "Unlocking");
}

static void check_child(void)
{
    int st;
    char keep[300];
    if (child_pid <= 0 || waitpid(child_pid, &st, WNOHANG) != child_pid) return;
    child_pid = 0;
    U.tick_ms = 0;
    status_msg[0] = '\0';
    snprintf(keep, sizeof(keep), "%s", child_keep);
    read_dir(NULL);
    {   /* আগের নাম (ফোল্ডার) বা NAME.rozlock — যেটা এখন আছে সেটা বাছাই */
        int i;
        for (i = 0; i < ne; i++)
            if (!strcmp(E[i].name, keep) || (E[i].disp && !strcmp(E[i].disp, keep))) sel = i;
    }
}

static void open_entry(int i, int with_code)
{
    char p[700];
    if (i < 0 || i >= ne) return;
    join(p, sizeof(p), cwd, E[i].name);
    if (E[i].locked) { run_lockdlg("unlock", p, E[i].disp); return; }      /* ধাপ ১৯ছ */
    if (E[i].isdir && !with_code) { go(E[i].name, 1); return; }
    if (with_code || E[i].kind == 'c' || E[i].kind == 'd') launch2("code", p);
    else if (E[i].kind == 'i') launch2("imgview", p);
    else if (E[i].kind == 'p') run_in_terminal(p);
}

static int remove_tree(const char *p)
{
    struct stat st;
    if (lstat(p, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(p);
        struct dirent *de;
        char names[64][256];
        int n, i, more = 1, rc = 0;
        /* ধাপে ধাপে: একবারে ৬৪টা নাম পড়ে মোছা (মুছতে মুছতে readdir নয়) */
        while (more && d) {
            n = 0;
            rewinddir(d);
            while ((de = readdir(d)) && n < 64) {
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
                snprintf(names[n++], 256, "%s", de->d_name);
            }
            more = n > 0;
            for (i = 0; i < n; i++) {
                char c[700];
                join(c, sizeof(c), p, names[i]);
                if (remove_tree(c) < 0) { rc = -1; more = 0; }
            }
        }
        if (d) closedir(d);
        if (rc < 0) return -1;
        return rmdir(p);
    }
    return unlink(p);
}

/* ধাপ ২৪ঘ: কপি চলার সময় নিচের status বারে অগ্রগতি (উইন্ডো "আটকে" না দেখায়) */
static long copy_last_draw;
static unsigned long long copy_done_total;

static void copy_progress(const char *name, unsigned long long done, unsigned long long size)
{
    char t[300];
    long now = gfx_ticks_ms();
    if (now - copy_last_draw < 250) return;
    copy_last_draw = now;
    if (size) snprintf(t, sizeof(t), "Copying %.60s ... %llu of %llu KB (%llu%%)", name, done / 1024, size / 1024,
                       done * 100 / size);
    else snprintf(t, sizeof(t), "Copying %.60s ...", name);
    ui_statusbar(U.s, 0, U.h - STATUS_H, U.w, STATUS_H, t);
    gfx_present_rect(0, U.h - STATUS_H, U.w, STATUS_H);
}

static int copy_file(const char *src, const char *dst)
{
    int in = open(src, O_RDONLY), out;
    static char *buf;
    const size_t bsz = 256 * 1024;                   /* ধাপ ২৪ঘ: বড় বাফার = কম, বড় লেখা */
    ssize_t n;
    struct stat st;
    unsigned long long done = 0;
    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;
    if (!buf) buf = (char *)malloc(bsz);
    if (!buf || in < 0) { if (in >= 0) close(in); return -1; }
    if (fstat(in, &st) != 0) { st.st_mode = 0644; st.st_size = 0; }
    out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, (st.st_mode & 0777) | 0200);
    if (out < 0) { close(in); return -1; }
    copy_progress(base, 0, (unsigned long long)st.st_size);
    while ((n = read(in, buf, bsz)) > 0) {
        if (write(out, buf, (size_t)n) != n) { n = -1; break; }
        done += (unsigned long long)n;
        copy_done_total += (unsigned long long)n;
        copy_progress(base, done, (unsigned long long)st.st_size);
    }
    {
        int e = errno;
        close(in);
        close(out);
        errno = e;
    }
    if (n >= 0 && rozlock_is_protected(src)) rozlock_protect(dst, 1);   /* ধাপ ১৯ছ: লক-ফাইলের কপিও মোছা-রোধী */
    return n < 0 ? -1 : 0;
}

static int copy_tree(const char *src, const char *dst)
{
    struct stat st;
    if (stat(src, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d;
        struct dirent *de;
        int rc = 0;
        if (!strncmp(dst, src, strlen(src)) && (dst[strlen(src)] == '/' || !dst[strlen(src)])) {
            errno = EINVAL;                      /* নিজের ভেতরে নিজেকে কপি নয় */
            return -1;
        }
        if (mkdir(dst, 0755) != 0 && errno != EEXIST) return -1;
        d = opendir(src);
        if (!d) return -1;
        while ((de = readdir(d))) {
            char a[700], b[700];
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            join(a, sizeof(a), src, de->d_name);
            join(b, sizeof(b), dst, de->d_name);
            if (copy_tree(a, b) < 0) rc = -1;
        }
        closedir(d);
        return rc;
    }
    return copy_file(src, dst);
}

/* ধাপ ১৫ক: আগে rename() (সঙ্গে সঙ্গে, ফোল্ডারও); অন্য ফাইলসিস্টেমে হলে কপি করে পুরোনোটা মোছা */
static int move_path(const char *a, const char *b)
{
    if (rename(a, b) == 0) return 0;
    if (errno != EXDEV && errno != ENOSYS) return -1;
    if (copy_tree(a, b) != 0) return -1;
    return remove_tree(a);
}

static void do_new(int folder)
{
    char name[256], p[700];
    snprintf(name, sizeof(name), "%s", folder ? "New folder" : "untitled.c");
    if (!ui_inputbox(folder ? "New folder" : "New file", "Name:", name, sizeof(name)) || !name[0]) return;
    join(p, sizeof(p), cwd, name);
    if (access(p, F_OK) == 0) { ui_msgbox("Files", "That name is already used.", UI_MB_OK | UI_MB_ERROR); return; }
    if (folder ? mkdir(p, 0755) : (close(open(p, O_WRONLY | O_CREAT, 0644)), access(p, F_OK))) {
        char m[600];
        snprintf(m, sizeof(m), "Cannot create %s:\n%s", name, strerror(errno));
        ui_msgbox("Files", m, UI_MB_OK | UI_MB_ERROR);
        return;
    }
    read_dir(name);
}

static void do_rename(void)
{
    char name[256], a[700], b[700];
    if (sel < 0) return;
    if (E[sel].locked) {                         /* ধাপ ১৯ছ */
        ui_msgbox("Rename", "A locked folder cannot be renamed.\nUnlock it first.", UI_MB_OK | UI_MB_WARN);
        return;
    }
    snprintf(name, sizeof(name), "%s", E[sel].name);
    if (!ui_inputbox("Rename", "New name:", name, sizeof(name)) || !name[0] || !strcmp(name, E[sel].name)) return;
    join(a, sizeof(a), cwd, E[sel].name);
    join(b, sizeof(b), cwd, name);
    if (access(b, F_OK) == 0) { ui_msgbox("Rename", "That name is already used.", UI_MB_OK | UI_MB_ERROR); return; }
    if (move_path(a, b) != 0) {
        char m[600];
        snprintf(m, sizeof(m), "Cannot rename:\n%s", strerror(errno));
        ui_msgbox("Rename", m, UI_MB_OK | UI_MB_ERROR);
        return;
    }
    read_dir(name);
}

static void do_delete(void)
{
    char m[600], p[700];
    if (sel < 0) return;
    if (E[sel].locked) {                         /* ধাপ ১৯ছ */
        ui_msgbox("Delete", "A locked folder cannot be deleted.\nUnlock it first.", UI_MB_OK | UI_MB_WARN);
        return;
    }
    snprintf(m, sizeof(m), E[sel].isdir ? "Delete the folder \"%s\"\nand everything in it?" : "Delete \"%s\"?",
             E[sel].name);
    if (ui_msgbox("Delete", m, UI_MB_YESNO | UI_MB_WARN) != UI_YES) return;
    join(p, sizeof(p), cwd, E[sel].name);
    if (remove_tree(p) != 0) {
        if (errno == EPERM) snprintf(m, sizeof(m), "Cannot delete everything:\nthere is a locked folder inside.\nUnlock it first.");
        else snprintf(m, sizeof(m), "Cannot delete:\n%s", strerror(errno));
        ui_msgbox("Delete", m, UI_MB_OK | UI_MB_ERROR);
    } else {
        snprintf(status_msg, sizeof(status_msg), "Deleted %s", E[sel].name);
    }
    {
        int keep = sel;
        read_dir(NULL);
        sel = keep < ne ? keep : ne - 1;
    }
}

static void do_copy(int cut)
{
    if (sel < 0) return;
    if (cut && E[sel].locked) {                  /* ধাপ ১৯ছ: সরানো নয় (কপি চলে — কপিও লক থাকে) */
        ui_msgbox("Cut", "A locked folder cannot be moved.\nUnlock it first, or use Copy.", UI_MB_OK | UI_MB_WARN);
        return;
    }
    join(clip_path, sizeof(clip_path), cwd, E[sel].name);
    clip_cut = cut;
    snprintf(status_msg, sizeof(status_msg), "%s %s - now Paste (Ctrl+V) in another folder",
             cut ? "Cut" : "Copied", E[sel].name);
}

static void do_paste(void)
{
    char dst[700], name[256], m[700];
    const char *b;
    int rc;
    if (!clip_path[0]) { snprintf(status_msg, sizeof(status_msg), "Nothing to paste"); return; }
    b = strrchr(clip_path, '/');
    snprintf(name, sizeof(name), "%s", b ? b + 1 : clip_path);
    join(dst, sizeof(dst), cwd, name);
    if (!strcmp(dst, clip_path)) {
        if (clip_cut) return;                    /* একই জায়গায় সরানো = কিছু নয় */
        snprintf(name, sizeof(name), "copy of %s", b ? b + 1 : clip_path);
        join(dst, sizeof(dst), cwd, name);
    }
    if (access(dst, F_OK) == 0) {
        snprintf(m, sizeof(m), "\"%s\" already exists here.\nReplace it?", name);
        if (ui_msgbox("Paste", m, UI_MB_YESNO | UI_MB_WARN) != UI_YES) return;
        remove_tree(dst);
    }
    if (clip_cut) {
        rc = move_path(clip_path, dst);
        if (rc == 0) clip_path[0] = '\0';
    } else {
        rc = copy_tree(clip_path, dst);
    }
    if (rc != 0) {
        snprintf(m, sizeof(m), "Paste failed:\n%s", strerror(errno));
        ui_msgbox("Paste", m, UI_MB_OK | UI_MB_ERROR);
    }
    read_dir(name);
}

static void size_text(char *out, size_t n, const struct ent *e)
{
    if (e->isdir) snprintf(out, n, "-");
    else if (e->size < 1024) snprintf(out, n, "%ld B", e->size);
    else if (e->size < 1024 * 1024) snprintf(out, n, "%ld KB", (e->size + 1023) / 1024);
    else snprintf(out, n, "%ld.%ld MB", e->size / (1024 * 1024), (e->size % (1024 * 1024)) * 10 / (1024 * 1024));
}

static void do_props(void)
{
    char m[900], sz[32];
    if (sel < 0) return;
    size_text(sz, sizeof(sz), &E[sel]);
    snprintf(m, sizeof(m), "Name:   %s\nFolder: %s\nType:   %s\nSize:   %s (%ld bytes)\nMode:   %03o\nHidden: %s%s",
             E[sel].name, cwd, E[sel].type, sz, E[sel].size, E[sel].mode & 0777, E[sel].hidden ? "yes" : "no",
             E[sel].locked ? "\nLocked: yes (encrypted, cannot be deleted)" : "");
    ui_msgbox("Properties", m, UI_MB_OK | UI_MB_INFO);
}

/* ধাপ ১৯ছ */
static void do_lock(void)
{
    char p[700];
    if (sel < 0) return;
    join(p, sizeof(p), cwd, E[sel].name);
    if (E[sel].locked) run_lockdlg("unlock", p, E[sel].disp);
    else if (E[sel].isdir) run_lockdlg("lock", p, E[sel].name);
    else ui_msgbox("Lock", "Only folders can be locked.\nPut the file in a folder, then lock the folder.", UI_MB_OK | UI_MB_INFO);
}

static void do_hide(void)
{
    char p[700];
    int r;
    if (sel < 0) return;
    if (E[sel].name[0] == '.') {
        ui_msgbox("Hide", "Names starting with a dot are always hidden.\nRename it to show it.", UI_MB_OK | UI_MB_INFO);
        return;
    }
    join(p, sizeof(p), cwd, E[sel].name);
    r = rozhide_set(p, !E[sel].hidden);
    if (r < 0) {
        char m[300];
        snprintf(m, sizeof(m), "Cannot change:\n%s", rozlock_strerror(r));
        ui_msgbox("Hide", m, UI_MB_OK | UI_MB_ERROR);
        return;
    }
    snprintf(status_msg, sizeof(status_msg), "%s %s%s", E[sel].hidden ? "Showing" : "Hidden:",
             E[sel].disp ? E[sel].disp : E[sel].name, E[sel].hidden || show_hidden ? "" : "  (View > Hidden files shows it)");
    {
        char keep[256];
        snprintf(keep, sizeof(keep), "%s", E[sel].name);
        read_dir(keep);
    }
}

static void terminal_here(void)
{
    const char *argv[2];
    argv[0] = "rozterm";
    argv[1] = NULL;
    ui_launch(argv, cwd);
}

/* ---------- মেনু ---------- */
enum {
    A_OPEN = 1, A_CODE, A_RUN, A_NEWDIR, A_NEWFILE, A_RENAME, A_DELETE, A_PROPS, A_QUIT,
    A_COPY, A_CUT, A_PASTE, A_REFRESH, A_HIDDEN, A_BACK, A_UP, A_HOME, A_ROOT, A_TERM, A_ABOUT,
    A_LOCK, A_HIDE                               /* ধাপ ১৯ছ */
};
static const struct ui_menuitem file_items[] = {
    { "Open", NULL, A_OPEN }, { "Open with Code", NULL, A_CODE }, { "Run in terminal", NULL, A_RUN },
    { "-", NULL, 0 }, { "New folder...", "Ctrl+N", A_NEWDIR }, { "New file...", NULL, A_NEWFILE },
    { "Rename...", "F2", A_RENAME }, { "Delete", "Del", A_DELETE }, { "Properties", NULL, A_PROPS },
    { "Lock / Unlock...", "Ctrl+L", A_LOCK }, { "Hide / Unhide", NULL, A_HIDE },
    { "-", NULL, 0 }, { "Terminal here", "Ctrl+T", A_TERM }, { "Close", "Ctrl+Q", A_QUIT }
};
static const struct ui_menuitem edit_items[] = {
    { "Copy", "Ctrl+C", A_COPY }, { "Cut", "Ctrl+X", A_CUT }, { "Paste", "Ctrl+V", A_PASTE }
};
static struct ui_menuitem view_items[] = {
    { "Refresh", "F5", A_REFRESH }, { "[ ] Hidden files", NULL, A_HIDDEN }
};
static const struct ui_menuitem go_items[] = {
    { "Back", NULL, A_BACK }, { "Up", NULL, A_UP }, { "Home", NULL, A_HOME }, { "Root /", NULL, A_ROOT }
};
static const struct ui_menuitem help_items[] = { { "About Files", "F1", A_ABOUT } };
static const struct ui_menu menus[] = {
    { "File", file_items, 14 }, { "Edit", edit_items, 3 }, { "View", view_items, 2 },
    { "Go", go_items, 4 }, { "Help", help_items, 1 }
};

static void action(int a)
{
    switch (a) {
    case A_OPEN: open_entry(sel, 0); break;
    case A_CODE: if (sel >= 0 && !E[sel].isdir) open_entry(sel, 1); break;
    case A_RUN:
        if (sel >= 0 && !E[sel].isdir) { char p[700]; join(p, sizeof(p), cwd, E[sel].name); run_in_terminal(p); }
        break;
    case A_NEWDIR: do_new(1); break;
    case A_NEWFILE: do_new(0); break;
    case A_RENAME: do_rename(); break;
    case A_DELETE: do_delete(); break;
    case A_PROPS: do_props(); break;
    case A_QUIT: ui_quit(&U); break;
    case A_COPY: do_copy(0); break;
    case A_CUT: do_copy(1); break;
    case A_PASTE: do_paste(); break;
    case A_REFRESH: read_dir(sel >= 0 && sel < ne ? E[sel].name : NULL); break;
    case A_HIDDEN: show_hidden = !show_hidden; read_dir(NULL); break;
    case A_BACK: go_back(); break;
    case A_UP: go_up(); break;
    case A_HOME: go(getenv("HOME") ? getenv("HOME") : "/home", 1); break;
    case A_ROOT: go("/", 1); break;
    case A_TERM: terminal_here(); break;
    case A_LOCK: do_lock(); break;
    case A_HIDE: do_hide(); break;
    case A_ABOUT:
        ui_msgbox("About Files",
                  "Files - the rozOS file manager (phase 14b)\n\n"
                  "Double-click or Enter: open   Backspace: up\n"
                  "F2 rename   Del delete   Ctrl+C/X/V copy/cut/paste\n"
                  "Ctrl+N new folder   Ctrl+T terminal here   F5 refresh\n"
                  "Right-click: menu (Lock/Unlock, Hide/Unhide ...)   Ctrl+L lock/unlock\n"
                  "Source: /usr/src/gfx/files.c",
                  UI_MB_OK | UI_MB_INFO);
        break;
    }
}

/* ---------- আঁকা ---------- */
static const struct { const char *label; const char *path; char kind; } places[] = {
    { "Home", NULL, 'h' }, { "Root /", "/", 'f' }, { "Programs", "/bin", 'p' },
    { "Sources", "/usr/src", 'c' }, { "Images", "/usr/share/images", 'i' },
    { "Help pages", "/usr/share/help", 'e' }, { "Icons", "/usr/share/icons", 'g' }, { "Temp", "/tmp", 'f' },
    { "Disks /mnt", "/mnt", 'n' }                 /* ধাপ ১৫ক: mount করা ডিস্ক */
};
#define NPLACES ((int)(sizeof(places) / sizeof(places[0])))

static void frame(void)
{
    const struct ui_theme *t = ui_theme_get();
    gfx_surface *s = U.s;
    int W = U.w, H = U.h, a, x, y, ly, lh, lx, lw, rows, i, saved_key = 0, saved_mods = 0;
    if (U.pressed || U.key) status_msg[0] = '\0';    /* নতুন কাজে পুরোনো বার্তা মুছে যায় */
    /* লেখার ঘরে টাইপ চলছে: কী মেনুর শর্টকাটে নয়, ঘরেই যাবে (Delete যেন ফাইল না মোছে) */
    if (U.focus) { saved_key = U.key; saved_mods = U.mods; U.key = 0; }
    view_items[1].label = show_hidden ? "[x] Hidden files" : "[ ] Hidden files";
    a = ui_menubar(&U, 0, 0, W, menus, 5);
    if (U.focus && !U.mb_open) { U.key = saved_key; U.mods = saved_mods; }
    if (a) action(a);
    gfx_fill_rect(s, 0, UI_MENUBAR_H, W, H - UI_MENUBAR_H, t->window_bg);
    /* টুলবার */
    ui_panel(s, 0, UI_MENUBAR_H, W, TOOL_H, t->face, 1);
    x = 4;
    y = UI_MENUBAR_H + 3;
    if (ui_do_tool(&U, x, y, 64, TOOL_H - 6, "back", 'b', "Back")) go_back();
    x += 66;
    if (ui_do_tool(&U, x, y, 50, TOOL_H - 6, "up", 'u', "Up")) go_up();
    x += 52;
    if (ui_do_tool(&U, x, y, 64, TOOL_H - 6, "home", 'h', "Home")) action(A_HOME);
    x += 70;
    gfx_vline(s, x, y + 2, TOOL_H - 10, t->dark); gfx_vline(s, x + 1, y + 2, TOOL_H - 10, t->light2);
    x += 6;
    if (ui_do_tool(&U, x, y, 112, TOOL_H - 6, "new-folder", 'k', "New folder")) do_new(1);
    x += 114;
    if (ui_do_tool(&U, x, y, 84, TOOL_H - 6, "rename", 'e', "Rename")) do_rename();
    x += 86;
    if (ui_do_tool(&U, x, y, 80, TOOL_H - 6, "delete", 'm', "Delete")) do_delete();
    x += 86;
    gfx_vline(s, x, y + 2, TOOL_H - 10, t->dark); gfx_vline(s, x + 1, y + 2, TOOL_H - 10, t->light2);
    x += 6;
    if (ui_do_tool(&U, x, y, 64, TOOL_H - 6, "copy", 'd', "Copy")) do_copy(0);
    x += 66;
    if (ui_do_tool(&U, x, y, 70, TOOL_H - 6, "paste", 'v', "Paste")) do_paste();
    x += 76;
    if (x + 100 < W && ui_do_tool(&U, x, y, 96, TOOL_H - 6, "terminal", 't', "Terminal")) terminal_here();
    /* ঠিকানা */
    y = UI_MENUBAR_H + TOOL_H;
    ui_panel(s, 0, y, W, ADDR_H, t->face, 1);
    gfx_text(s, NULL, 10, y + 8, "Location:", t->text);
    if (ui_do_textbox(&U, 90, y + 4, W - 100, addr, sizeof(addr)) == 2) { U.focus = 0; go(addr, 1); }
    /* জায়গার তালিকা */
    ly = y + ADDR_H;
    lh = H - ly - STATUS_H;
    ui_sunken(s, 0, ly, PLACES_W, lh, t->face);
    for (i = 0; i < NPLACES; i++) {
        int py = ly + 6 + i * 26;
        const char *pp = places[i].path ? places[i].path : (getenv("HOME") ? getenv("HOME") : "/home");
        int cur = !strcmp(pp, cwd), hov = ui_hover(&U, 2, py, PLACES_W - 4, 24);
        if (cur || hov) gfx_fill_rect(s, 3, py, PLACES_W - 6, 24, cur ? t->select : t->face_hover);
        ui_icon(s, 10, py + 4, places[i].kind, t->text);
        gfx_text(s, NULL, 32, py + 4, places[i].label, t->text);
        if (hov && U.pressed) go(pp, 1);
    }
    /* ধাপ ২৪গ: Disks — mount করা প্রতিটা ডিস্ক (/mnt/...) আলাদা লাইনে, ক্লিকে খোলে */
    {
        static char mbuf[4096];
        static long last_ms;
        static int nd;
        static char dpath[12][64], dlabel[12][48];
        long now = gfx_ticks_ms();
        int py = ly + 6 + NPLACES * 26 + 6, k;
        if (!last_ms || now - last_ms > 2000) {             /* ২ সেকেন্ড পরপর তালিকা (পেন ড্রাইভ আসা-যাওয়া) */
            long n = rozos_mounts(mbuf, sizeof(mbuf) - 1);
            char *line, *save;
            last_ms = now;
            nd = 0;
            if (n > 0) {
                mbuf[n] = '\0';
                for (line = strtok_r(mbuf, "\n", &save); line && nd < 12; line = strtok_r(NULL, "\n", &save)) {
                    char dev[32], mp[64], fs[16], kind[16] = "-", label[48] = "-";
                    const char *k;
                    if (sscanf(line, "%31s %63s %15s %15s %47[^\n]", dev, mp, fs, kind, label) < 3 ||
                        strncmp(mp, "/mnt/", 5)) continue;
                    /* ধাপ ২৪ঘ: নাম থাকলে নাম ("My Passport"), নইলে ধরন; পাশে USB/NVMe/SATA */
                    k = !strcmp(kind, "nvme") ? "NVMe" : !strcmp(kind, "sata") ? "SATA" : !strcmp(kind, "usb") ? "USB" : fs;
                    snprintf(dpath[nd], sizeof(dpath[nd]), "%s", mp);
                    if (strcmp(label, "-")) snprintf(dlabel[nd], sizeof(dlabel[nd]), "%s (%s)", label, k);
                    else snprintf(dlabel[nd], sizeof(dlabel[nd]), "%s %s (%s)", k, mp + 5, fs);
                    nd++;
                }
            }
        }
        if (nd && py + 20 < ly + lh) {
            gfx_text(s, NULL, 10, py, "Disks", t->text_dim);
            py += 20;
        }
        for (k = 0; k < nd && py + 24 < ly + lh; k++, py += 26) {
            int cur = !strncmp(cwd, dpath[k], strlen(dpath[k])) &&
                      (cwd[strlen(dpath[k])] == '\0' || cwd[strlen(dpath[k])] == '/');
            int hov = ui_hover(&U, 2, py, PLACES_W - 4, 24);
            if (cur || hov) gfx_fill_rect(s, 3, py, PLACES_W - 6, 24, cur ? t->select : t->face_hover);
            ui_icon(s, 10, py + 4, 'n', t->text);
            gfx_set_clip(s, 32, py, PLACES_W - 36, 24);
            gfx_text(s, NULL, 32, py + 4, dlabel[k], t->text);
            gfx_reset_clip(s);
            if (hov && U.pressed) go(dpath[k], 1);
        }
    }
    /* ফাইলের তালিকা (কলাম: নাম, মাপ, ধরন) */
    lx = PLACES_W + 2;
    lw = W - lx - 16;
    ui_panel(s, lx, ly, lw, 22, t->face, 1);
    gfx_text(s, NULL, lx + 30, ly + 3, "Name", t->text);
    gfx_text(s, NULL, lx + lw - 210, ly + 3, "Size", t->text);
    gfx_text(s, NULL, lx + lw - 110, ly + 3, "Type", t->text);
    ly += 22;
    lh -= 22;
    rows = (lh - 4) / ROW_H;
    if (rows < 1) rows = 1;
    if (ui_hover(&U, lx, ly, lw, lh)) {
        if (U.wheel) top += U.wheel * 3;
        if (U.pressed) {
            int r = (U.my - ly - 2) / ROW_H + top;
            sel = r < ne ? r : -1;
            U.focus = 0;                         /* লেখার ঘর থেকে ফোকাস সরানো */
            if (U.dclick && sel >= 0) open_entry(sel, 0);
        }
        if (U.rpressed) {                        /* ধাপ ১৯ছ: ডান-ক্লিক মেনু (আগে সরাসরি Properties) */
            int r = (U.my - ly - 2) / ROW_H + top, a;
            sel = r >= 0 && r < ne ? r : -1;
            if (sel >= 0) {
                struct ui_menuitem m[16];
                int n = 0, lk = E[sel].locked, dir = E[sel].isdir;
                m[n].label = lk ? "Unlock..." : "Open"; m[n].shortcut = "Enter"; m[n++].id = A_OPEN;
                if (!dir && !lk) { m[n].label = "Open with Code"; m[n].shortcut = NULL; m[n++].id = A_CODE; }
                m[n].label = "-"; m[n].shortcut = NULL; m[n++].id = 0;
                m[n].label = "Copy"; m[n].shortcut = "Ctrl+C"; m[n++].id = A_COPY;
                m[n].label = "Cut"; m[n].shortcut = "Ctrl+X"; m[n++].id = lk ? -1 : A_CUT;
                m[n].label = "Paste"; m[n].shortcut = "Ctrl+V"; m[n++].id = clip_path[0] ? A_PASTE : -1;
                m[n].label = "-"; m[n].shortcut = NULL; m[n++].id = 0;
                m[n].label = "Rename..."; m[n].shortcut = "F2"; m[n++].id = lk ? -1 : A_RENAME;
                m[n].label = "Delete"; m[n].shortcut = "Del"; m[n++].id = lk ? -1 : A_DELETE;
                m[n].label = "-"; m[n].shortcut = NULL; m[n++].id = 0;
                if (dir) { m[n].label = "Lock..."; m[n].shortcut = NULL; m[n++].id = A_LOCK; }
                if (lk) { m[n].label = "Unlock..."; m[n].shortcut = NULL; m[n++].id = A_LOCK; }
                m[n].label = E[sel].hidden ? "Unhide" : "Hide"; m[n].shortcut = NULL; m[n++].id = A_HIDE;
                m[n].label = "Properties"; m[n].shortcut = NULL; m[n++].id = A_PROPS;
                a = ui_popup_menu(m, n, U.mx, U.my);
            } else {
                static const struct ui_menuitem bg[] = {
                    { "New folder...", "Ctrl+N", A_NEWDIR }, { "New file...", NULL, A_NEWFILE },
                    { "Paste", "Ctrl+V", A_PASTE }, { "-", NULL, 0 }, { "Refresh", "F5", A_REFRESH },
                    { "Hidden files (show/hide)", NULL, A_HIDDEN }, { "Terminal here", "Ctrl+T", A_TERM }
                };
                a = ui_popup_menu(bg, 7, U.mx, U.my);
            }
            if (a > 0) action(a);
        }
    }
    /* কী (লেখার ঘরে ফোকাস না থাকলে) */
    if (U.key && !U.focus) {
        int k = U.key;
        U.key = 0;
        if (k == GFX_KEY_DOWN) sel = sel + 1 < ne ? sel + 1 : sel;
        else if (k == GFX_KEY_UP) sel = sel > 0 ? sel - 1 : 0;
        else if (k == GFX_KEY_PAGE_DOWN) sel = sel + rows < ne ? sel + rows : ne - 1;
        else if (k == GFX_KEY_PAGE_UP) sel = sel - rows > 0 ? sel - rows : 0;
        else if (k == GFX_KEY_HOME) sel = ne ? 0 : -1;
        else if (k == GFX_KEY_END) sel = ne - 1;
        else if (k == '\n') open_entry(sel, 0);
        else if (k == '\b') go_up();
        else if (k == 14) do_new(1);             /* Ctrl+N */
        else if (k == 20) terminal_here();       /* Ctrl+T */
        else if (k == GFX_KEY_F1) action(A_ABOUT);
        else if (k > 32 && k < 127) {            /* অক্ষর চাপলে সেই অক্ষরের প্রথম নাম */
            int j;
            for (j = 1; j <= ne; j++) {
                int c = (sel + j) % ne;
                if (tolower((unsigned char)E[c].name[0]) == tolower(k)) { sel = c; break; }
            }
        }
        if (sel >= 0 && sel < top) top = sel;
        if (sel >= top + rows) top = sel - rows + 1;
    }
    if (top > ne - rows) top = ne - rows;
    if (top < 0) top = 0;
    ui_sunken(s, lx, ly, lw, lh, t->field);
    gfx_set_clip(s, lx + 2, ly + 2, lw - 4, lh - 4);
    for (i = 0; i <= rows && top + i < ne; i++) {
        struct ent *e = &E[top + i];
        int ry = ly + 2 + i * ROW_H;
        char sz[32];
        if (top + i == sel) gfx_fill_rect(s, lx + 2, ry, lw - 4, ROW_H, t->select);
        ui_draw_icon(s, lx + 8, ry + 3, 16, e->image, e->kind);
        gfx_set_clip(s, lx + 2, ly + 2, lw - 220, lh - 4);
        gfx_text(s, NULL, lx + 30, ry + 3, e->disp ? e->disp : e->name,
                 top + i == sel ? GFX_WHITE : e->hidden ? t->text_dim : t->field_text);   /* ধাপ ১৯ছ */
        gfx_set_clip(s, lx + 2, ly + 2, lw - 4, lh - 4);
        size_text(sz, sizeof(sz), e);
        gfx_text(s, NULL, lx + lw - 210, ry + 3, sz, t->text_dim);
        gfx_text(s, NULL, lx + lw - 110, ry + 3, e->type, t->text_dim);
    }
    if (ne == 0) gfx_text(s, NULL, lx + 30, ly + 10, "(empty folder)", t->text_dim);
    gfx_reset_clip(s);
    ui_do_vscroll(&U, lx + lw, ly - 22, 16, lh + 22, &top, ne, rows);
    /* অবস্থা-লাইন */
    {
        char st[300], sz[32];
        if (status_msg[0]) snprintf(st, sizeof(st), "%s", status_msg);
        else if (sel >= 0 && sel < ne) {
            size_text(sz, sizeof(sz), &E[sel]);
            snprintf(st, sizeof(st), "%d items  |  %s  -  %s  %s", ne, E[sel].disp ? E[sel].disp : E[sel].name, E[sel].type,
                     E[sel].isdir ? "" : sz);
        } else snprintf(st, sizeof(st), "%d items", ne);
        ui_statusbar(s, 0, H - STATUS_H, W, STATUS_H, st);
    }
}

int main(int argc, char **argv)
{
    const char *start = argc > 1 ? argv[1] : NULL;
    char title[96];
    if (gfx_init_window("Files", 880, 600) < 0) {
        perror("files: no screen");
        return 1;
    }
    if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "/");
    if (!start) start = getenv("HOME") ? getenv("HOME") : "/home";
    ui_init(&U);
    go(start, 0);
    while (ui_run(&U)) {
        check_child();                           /* ধাপ ১৯ছ */
        frame();
        snprintf(title, sizeof(title), "%s - Files", cwd);
        {
            static char last[96];
            if (strcmp(last, title)) { gfx_set_title(title); snprintf(last, sizeof(last), "%s", title); }
        }
    }
    free_ents();
    gfx_quit();
    return 0;
}
