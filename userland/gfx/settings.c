/* rozOS — settings: ডেস্কটপ আর সিস্টেমের সেটিং ও তথ্য (ধাপ ১৯চ)
 *
 *   settings [wallpaper|icons|system|processes|disks|network|fonts]
 *
 *   Wallpaper      /usr/share/images-এর ছবি থাম্বনেইলে, ক্লিকেই বসে; Browse..., None;
 *                  ধরন Stretch / Fill; শুরুতে লোগো (splash) চালু/বন্ধ → /etc/rozos.conf
 *                  (rozwm প্রতি সেকেন্ডে দেখে, তাই সাথে সাথে বদলায়)
 *   Desktop icons  Start মেনুর প্রোগ্রাম ডেস্কটপে যোগ, নিজের কমান্ড যোগ, মোছা — /home/Desktop-এর .app ফাইল
 *   System         rozOS সংস্করণ, চালু থাকার সময়, RAM (sysinfo), প্রসেস
 *   Processes      চলমান প্রসেস (rozos_proc_list), প্রতি সেকেন্ডে হালনাগাদ, End process
 *   Disks          mount করা ডিস্ক — মাপ, ব্যবহার, খালি (statvfs)
 *   Network        নেটওয়ার্ক কার্ড — ঠিকানা, MAC, প্যাকেট (rozos_netinfo)
 *   Fonts          ধাপ ২৩ক: UI ফন্ট আর টার্মিনাল/Code ফন্ট, মাপ, নমুনা → /etc/rozos.conf;
 *                  ধাপ ২৩খ: বাংলা ফন্ট (bengali_font) বোতামে বদলায়
 *                  (ui_font, ui_font_size, mono_font, mono_font_size; নতুন খোলা জানালায় লাগে)
 * ডেস্কটপে ডান-ক্লিক → "Change wallpaper..." / "Add icon..." / "Settings..." এটা খোলে। */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/sysinfo.h>
#include <sys/statvfs.h>
#include <arpa/inet.h>
#include <rozos.h>
#include <rozui.h>

#define CONF        "/etc/rozos.conf"
#define DESKTOP_DIR "/home/Desktop"
#define IMAGES_DIR  "/usr/share/images"
#define APPS_DIR    "/usr/share/apps"
#define NAV_W       150
#define MAXIMG      64
#define MAXAPP      96
#define MAXROW      128

enum { S_WALL, S_ICONS, S_SYSTEM, S_PROC, S_DISKS, S_NET, S_FONTS, S_COUNT };   /* ধাপ ২৩ক: Fonts */
static const char *const sections[S_COUNT] = { "Wallpaper", "Desktop icons", "System", "Processes", "Disks", "Network",
                                               "Fonts" };
static int section, nav_top;
static char status[200];

/* ================= /etc/rozos.conf ================= */

static void conf_get(const char *key, char *out, size_t size, const char *def)
{
    FILE *f = fopen(CONF, "r");
    char line[300];
    size_t kl = strlen(key);
    snprintf(out, size, "%s", def);
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, key, kl) && line[kl] == '=') {
            line[strcspn(line, "\r\n")] = '\0';
            snprintf(out, size, "%s", line + kl + 1);
        }
    }
    fclose(f);
}

/* key=value বসানো/বদলানো; অন্য সব লাইন যেমন ছিল তেমন */
static int conf_set(const char *key, const char *value)
{
    FILE *f = fopen(CONF, "r"), *o;
    char line[300];
    size_t kl = strlen(key);
    int done = 0;
    o = fopen(CONF ".new", "w");
    if (!o) { if (f) fclose(f); return -1; }
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, key, kl) && line[kl] == '=') {
                if (!done) fprintf(o, "%s=%s\n", key, value);
                done = 1;
            } else {
                fputs(line, o);
            }
        }
        fclose(f);
    }
    if (!done) fprintf(o, "%s=%s\n", key, value);
    if (fclose(o) != 0) return -1;
    unlink(CONF);
    return rename(CONF ".new", CONF);
}

/* ================= Wallpaper ================= */

static char *imgs[MAXIMG];
static gfx_surface *thumbs[MAXIMG];
static int nimgs, thumbs_done;
static char cur_wall[200];
static int cur_fill, cur_splash;

static int is_picture(const char *n)
{
    static const char *const ext[] = { ".jpg", ".jpeg", ".png", ".bmp", ".gif", ".tga", ".ppm" };
    const char *dot = strrchr(n, '.');
    size_t i;
    if (!dot) return 0;
    for (i = 0; i < sizeof(ext) / sizeof(ext[0]); i++) if (!strcasecmp(dot, ext[i])) return 1;
    return 0;
}

static int cmp_str(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }

static void read_wall_conf(void)
{
    char v[200];
    conf_get("wallpaper", cur_wall, sizeof(cur_wall), "");
    conf_get("wallpaper_style", v, sizeof(v), "stretch");
    cur_fill = !strcmp(v, "fill");
    conf_get("splash", v, sizeof(v), "/usr/share/images/rozos-boot.jpg");
    cur_splash = v[0] != '\0';
}

static void load_images(void)
{
    DIR *d = opendir(IMAGES_DIR);
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d)) && nimgs < MAXIMG) {
        if (!is_picture(e->d_name)) continue;
        imgs[nimgs] = (char *)malloc(strlen(IMAGES_DIR) + strlen(e->d_name) + 2);
        if (!imgs[nimgs]) break;
        sprintf(imgs[nimgs], "%s/%s", IMAGES_DIR, e->d_name);
        nimgs++;
    }
    closedir(d);
    qsort(imgs, (size_t)nimgs, sizeof(imgs[0]), cmp_str);
}

static gfx_surface *small_copy(const gfx_surface *img, int bw, int bh)
{
    int w = bw, h = bh;
    gfx_surface *t;
    if (img->w * bh > img->h * bw) h = img->h * bw / img->w; else w = img->w * bh / img->h;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    t = gfx_surface_new(w, h);
    if (t) gfx_blit_scaled(t, 0, 0, w, h, img);
    return t;
}

static int load_next_thumb(void)
{
    gfx_surface *img;
    if (thumbs_done >= nimgs) return 0;
    img = gfx_load_image(imgs[thumbs_done]);
    if (img) {
        thumbs[thumbs_done] = small_copy(img, 112, 70);
        gfx_surface_free(img);
    }
    thumbs_done++;
    return 1;
}

static void apply_wallpaper(const char *path)
{
    if (conf_set("wallpaper", path) == 0) {
        snprintf(cur_wall, sizeof(cur_wall), "%s", path);
        snprintf(status, sizeof(status), path[0] ? "Wallpaper: %s" : "Wallpaper: none (plain colour)", path);
    } else {
        snprintf(status, sizeof(status), "Cannot write %s: %s", CONF, strerror(errno));
    }
}

static void page_wallpaper(ui_ctx *u, int x, int y, int w, int h)
{
    const struct ui_theme *T = ui_theme_get();
    int cols = (w - 10) / 124, i, gy = y + 36, rows;
    if (cols < 1) cols = 1;
    if (load_next_thumb()) ui_redraw(u);
    ui_label(u->s, x, y + 4, "Click a picture to use it as the desktop background:", T->text);
    for (i = 0; i < nimgs; i++) {
        int cx = x + (i % cols) * 124, cy = gy + (i / cols) * 104, sel = !strcmp(cur_wall, imgs[i]);
        const char *b = strrchr(imgs[i], '/');
        char name[16];
        if (cy + 100 > y + h - 120) break;
        if (sel) gfx_fill_rect(u->s, cx, cy, 120, 100, T->select);
        else if (ui_hover(u, cx, cy, 120, 100)) gfx_fill_rect(u->s, cx, cy, 120, 100, T->face_hover);
        ui_sunken(u->s, cx + 2, cy + 4, 116, 74, GFX_RGB(20, 20, 24));
        if (i < thumbs_done && thumbs[i])
            gfx_blit(u->s, cx + 4 + (112 - thumbs[i]->w) / 2, cy + 6 + (70 - thumbs[i]->h) / 2, thumbs[i], 0, 0, -1, -1);
        else ui_label_center(u->s, cx, cy + 6, 120, 70, i < thumbs_done ? "?" : "...", T->text_dim);
        snprintf(name, sizeof(name), "%s", b ? b + 1 : imgs[i]);
        if (strlen(b ? b + 1 : imgs[i]) > 14) { name[12] = '.'; name[13] = '.'; name[14] = '\0'; }
        ui_label_center(u->s, cx, cy + 80, 120, 18, name, T->text);
        if (ui_clicked(u, cx, cy, 120, 100)) apply_wallpaper(imgs[i]);
    }
    rows = y + h - 110;
    ui_separator(u->s, x, rows, w - 10);
    if (ui_do_button(u, x, rows + 10, 100, 26, "Browse...")) {
        char p[256];
        snprintf(p, sizeof(p), "%s", cur_wall[0] ? cur_wall : "/home");
        if (ui_filedlg("Choose a picture", p, sizeof(p), 0)) apply_wallpaper(p);
    }
    if (ui_do_button(u, x + 110, rows + 10, 140, 26, "None (plain)")) apply_wallpaper("");
    ui_label(u->s, x, rows + 48, "Style:", T->text);
    {
        int fill = cur_fill;
        if (ui_do_radio(u, x + 60, rows + 46, "Stretch (fill the screen)", &fill, 0) ||
            ui_do_radio(u, x + 320, rows + 46, "Fill (keep the shape)", &fill, 1)) {
            cur_fill = fill;
            conf_set("wallpaper_style", fill ? "fill" : "stretch");
            snprintf(status, sizeof(status), "Style: %s", fill ? "fill" : "stretch");
        }
    }
    {
        int sp = cur_splash;
        if (ui_do_check(u, x, rows + 76, "Show the rozOS logo before the desktop starts", &sp)) {
            cur_splash = sp;
            conf_set("splash", sp ? "/usr/share/images/rozos-boot.jpg" : "");
            snprintf(status, sizeof(status), "Start logo %s", sp ? "on" : "off");
        }
    }
}

/* ================= Desktop icons ================= */

struct appent { char name[64], file[128]; };
static struct appent apps[MAXAPP], mine[MAXAPP];
static const char *app_names[MAXAPP], *mine_names[MAXAPP];
static int napps, nmine, app_sel = -1, app_top, mine_sel = -1, mine_top;
static char cust_name[48], cust_cmd[160];
static int cust_window = 1;

static void app_name_of(const char *path, char *out, size_t size)
{
    FILE *f = fopen(path, "r");
    char line[256];
    const char *b = strrchr(path, '/');
    snprintf(out, size, "%s", b ? b + 1 : path);
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "name:", 5)) {
            char *v = line + 5;
            while (*v == ' ') v++;
            v[strcspn(v, "\r\n")] = '\0';
            snprintf(out, size, "%s", v);
            break;
        }
    }
    fclose(f);
}

static int cmp_app(const void *a, const void *b) { return strcasecmp(((const struct appent *)a)->name, ((const struct appent *)b)->name); }

static int scan_apps(const char *dir, struct appent *list, const char **names)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int n = 0, i;
    if (!d) return 0;
    while ((e = readdir(d)) && n < MAXAPP) {
        size_t l = strlen(e->d_name);
        if (l < 5 || strcmp(e->d_name + l - 4, ".app")) continue;
        snprintf(list[n].file, sizeof(list[n].file), "%s/%s", dir, e->d_name);
        app_name_of(list[n].file, list[n].name, sizeof(list[n].name));
        n++;
    }
    closedir(d);
    qsort(list, (size_t)n, sizeof(list[0]), cmp_app);
    for (i = 0; i < n; i++) names[i] = list[i].name;
    return n;
}

static void rescan_icons(void)
{
    napps = scan_apps(APPS_DIR, apps, app_names);
    nmine = scan_apps(DESKTOP_DIR, mine, mine_names);
    if (mine_sel >= nmine) mine_sel = nmine - 1;
}

static int copy_file(const char *src, const char *dst)
{
    char buf[1024];
    size_t n;
    FILE *a = fopen(src, "rb"), *b;
    if (!a) return -1;
    b = fopen(dst, "wb");
    if (!b) { fclose(a); return -1; }
    while ((n = fread(buf, 1, sizeof(buf), a)) > 0) fwrite(buf, 1, n, b);
    fclose(a);
    return fclose(b);
}

static void page_icons(ui_ctx *u, int x, int y, int w, int h)
{
    const struct ui_theme *T = ui_theme_get();
    int lw = (w - 30) / 2, lh = h - 200;
    ui_label(u->s, x, y + 4, "Programs (Start menu):", T->text);
    ui_label(u->s, x + lw + 20, y + 4, "Your icons on the desktop:", T->text);
    ui_do_list(u, x, y + 24, lw, lh, app_names, napps, &app_sel, &app_top);
    ui_do_list(u, x + lw + 20, y + 24, lw, lh, mine_names, nmine, &mine_sel, &mine_top);
    if (ui_do_button(u, x, y + 30 + lh, 150, 26, "Add to desktop") && app_sel >= 0) {
        char dst[256];
        const char *b = strrchr(apps[app_sel].file, '/');
        mkdir("/home", 0755);
        mkdir(DESKTOP_DIR, 0755);
        snprintf(dst, sizeof(dst), "%s/%s", DESKTOP_DIR, b ? b + 1 : apps[app_sel].file);
        if (copy_file(apps[app_sel].file, dst) == 0) snprintf(status, sizeof(status), "Added %s to the desktop", apps[app_sel].name);
        else snprintf(status, sizeof(status), "Cannot write %s", dst);
        rescan_icons();
    }
    if (ui_do_button(u, x + lw + 20, y + 30 + lh, 150, 26, "Remove") && mine_sel >= 0) {
        snprintf(status, sizeof(status), "Removed %s", mine[mine_sel].name);
        unlink(mine[mine_sel].file);
        rescan_icons();
    }
    {
        int cy = y + 70 + lh;
        ui_separator(u->s, x, cy - 6, w - 10);
        ui_label(u->s, x, cy + 4, "Your own program:   name", T->text);
        ui_do_textbox(u, x + 200, cy, 200, cust_name, sizeof(cust_name));
        ui_label(u->s, x, cy + 36, "command (e.g. snake, or /home/bin/hello)", T->text);
        ui_do_textbox(u, x + 330, cy + 32, w - 350, cust_cmd, sizeof(cust_cmd));
        ui_do_check(u, x, cy + 66, "It opens its own window (otherwise it runs in a terminal)", &cust_window);
        if (ui_do_button(u, x, cy + 94, 150, 26, "Add own icon")) {
            if (!cust_name[0] || !cust_cmd[0]) {
                snprintf(status, sizeof(status), "Give a name and a command first");
            } else {
                char dst[256], safe[48];
                int i;
                FILE *f;
                for (i = 0; cust_name[i] && i < (int)sizeof(safe) - 1; i++)
                    safe[i] = (cust_name[i] == '/' || cust_name[i] == ' ') ? '-' : cust_name[i];
                safe[i] = '\0';
                mkdir("/home", 0755);
                mkdir(DESKTOP_DIR, 0755);
                snprintf(dst, sizeof(dst), "%s/%s.app", DESKTOP_DIR, safe);
                f = fopen(dst, "w");
                if (f) {
                    fprintf(f, "# rozOS desktop icon (Settings)\nname: %s\nrun: %s\nwindow: %s\n", cust_name, cust_cmd,
                            cust_window ? "yes" : "no");
                    fclose(f);
                    snprintf(status, sizeof(status), "Added %s to the desktop", cust_name);
                    cust_name[0] = cust_cmd[0] = '\0';
                } else {
                    snprintf(status, sizeof(status), "Cannot write %s", dst);
                }
                rescan_icons();
            }
        }
    }
}

/* ================= System ================= */

static void human(char *out, size_t size, unsigned long long bytes)
{
    if (bytes >= 1024ULL * 1024 * 1024) snprintf(out, size, "%.1f GiB", bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024ULL * 1024) snprintf(out, size, "%.1f MiB", bytes / (1024.0 * 1024));
    else snprintf(out, size, "%llu KiB", bytes / 1024);
}

static void bar(gfx_surface *s, int x, int y, int w, int h, unsigned long long used, unsigned long long total)
{
    const struct ui_theme *T = ui_theme_get();
    int fw = total ? (int)((double)used * (w - 4) / (double)total) : 0;
    ui_sunken(s, x, y, w, h, T->field);
    gfx_fill_rect(s, x + 2, y + 2, fw, h - 4, used * 10 > total * 9 ? GFX_RGB(210, 70, 60) : GFX_RGB(70, 150, 220));
}

static void page_system(ui_ctx *u, int x, int y, int w, int h)
{
    const struct ui_theme *T = ui_theme_get();
    struct utsname un;
    struct sysinfo si;
    char line[200], a[32], b[32], c[32];
    int yy = y + 4;
    time_t now = time(NULL);
    struct tm tm;
    char off[16], when[64];
    (void)h;
    /* ডেস্কটপের ঘড়ির মতো /etc/rozos.conf-এর utc_offset অনুযায়ী স্থানীয় সময় */
    conf_get("utc_offset", off, sizeof(off), "+00:00");
    {
        int sign = off[0] == '-' ? -1 : 1, hh = 0, mm = 0;
        sscanf(off + (off[0] == '+' || off[0] == '-'), "%d:%d", &hh, &mm);
        now += (time_t)sign * (hh * 3600 + mm * 60);
    }
    gmtime_r(&now, &tm);
    strftime(when, sizeof(when), "%A %d %B %Y, %H:%M:%S", &tm);
    if (uname(&un) == 0) {
        snprintf(line, sizeof(line), "%s %s (%s) on %s", un.sysname, un.release, un.machine, un.nodename);
        ui_label(u->s, x, yy, line, T->text);
        yy += 24;
    }
    snprintf(line, sizeof(line), "%s  (UTC%s)", when, off);
    ui_label(u->s, x, yy, line, T->text_dim);
    yy += 30;
    if (sysinfo(&si) == 0) {
        unsigned long long total = (unsigned long long)si.totalram * si.mem_unit,
                           freeb = (unsigned long long)si.freeram * si.mem_unit;
        snprintf(line, sizeof(line), "Running for %ldh %02ldm %02lds", si.uptime / 3600, si.uptime / 60 % 60, si.uptime % 60);
        ui_label(u->s, x, yy, line, T->text);
        yy += 34;
        ui_groupbox(u->s, x, yy, w - 10, 92, "Memory (RAM)");
        human(a, sizeof(a), total - freeb);
        human(b, sizeof(b), freeb);
        human(c, sizeof(c), total);
        snprintf(line, sizeof(line), "used %s   free %s   of %s  (%d%%)", a, b, c,
                 total ? (int)((total - freeb) * 100 / total) : 0);
        ui_label(u->s, x + 12, yy + 24, line, T->text);
        bar(u->s, x + 12, yy + 50, w - 34, 22, total - freeb, total);
        yy += 108;
        snprintf(line, sizeof(line), "Processes: %u", (unsigned)si.procs);
        ui_label(u->s, x, yy, line, T->text);
        yy += 24;
    }
    snprintf(line, sizeof(line), "CPU: %s, rozOS uses one core (more cores: planned)", un.machine);
    ui_label(u->s, x, yy, line, T->text);
    yy += 34;
    /* ধাপ ২১: ভার্সন, বিল্ড নম্বর, CPU-র নাম — About উইন্ডোতে */
    if (ui_do_button(u, x, yy, 150, 28, "About rozOS...")) {
        const char *argv[2];
        argv[0] = "about";
        argv[1] = NULL;
        ui_launch(argv, NULL);
    }
}

/* ================= Processes ================= */

struct prow { int pid, ppid, threads; char state[16], name[40]; unsigned long long ms, kib; };
static struct prow procs[MAXROW];
static char proc_lines[MAXROW][96];
static const char *proc_ptrs[MAXROW];
static int nprocs, proc_sel = -1, proc_top;
static long last_proc_read;

static void read_procs(void)
{
    static char buf[16384];
    char *line, *save;
    long n = rozos_proc_list(buf, sizeof(buf) - 1);
    int sel_pid = proc_sel >= 0 && proc_sel < nprocs ? procs[proc_sel].pid : -1, i;
    nprocs = 0;
    if (n < 0) return;
    buf[n] = '\0';
    for (line = strtok_r(buf, "\n", &save); line && nprocs < MAXROW; line = strtok_r(NULL, "\n", &save)) {
        struct prow *p = &procs[nprocs];
        if (sscanf(line, "%d %d %15s %llu %d %llu %39[^\n]", &p->pid, &p->ppid, p->state, &p->ms, &p->threads,
                   &p->kib, p->name) != 7) continue;
        snprintf(proc_lines[nprocs], sizeof(proc_lines[0]), "%5d  %-16.16s %-9s %7llu.%01llu s %3d %7llu KiB",
                 p->pid, p->name, p->state, p->ms / 1000, p->ms / 100 % 10, p->threads, p->kib);
        proc_ptrs[nprocs] = proc_lines[nprocs];
        nprocs++;
    }
    proc_sel = -1;
    for (i = 0; i < nprocs; i++) if (procs[i].pid == sel_pid) proc_sel = i;
}

static void page_processes(ui_ctx *u, int x, int y, int w, int h)
{
    const struct ui_theme *T = ui_theme_get();
    if (u->now - last_proc_read >= 1000 || !nprocs) {
        read_procs();
        last_proc_read = u->now;
    }
    /* ধাপ ২৩ক: কলামগুলো স্পেস দিয়ে সাজানো — সমান-চওড়া PSF ফন্টে */
    gfx_text(u->s, gfx_font_default(), x, y + 4, "  PID  NAME             STATE        CPU time THR      MEMORY", T->text_dim);
    ui_list_font(gfx_font_default());
    ui_do_list(u, x, y + 24, w - 10, h - 80, proc_ptrs, nprocs, &proc_sel, &proc_top);
    ui_list_font(NULL);
    if (ui_do_button(u, x, y + h - 46, 140, 26, "End process") && proc_sel >= 0) {
        struct prow *p = &procs[proc_sel];
        char q[160];
        if (p->pid == 1) {
            ui_msgbox("Settings", "init (pid 1) keeps rozOS running - it cannot be ended.", UI_MB_OK | UI_MB_WARN);
        } else {
            snprintf(q, sizeof(q), "End %s (pid %d)?\nUnsaved work in it is lost.", p->name, p->pid);
            if (ui_msgbox("End process", q, UI_MB_YESNO | UI_MB_QUESTION) == UI_YES) {
                if (kill(p->pid, SIGTERM) == 0) snprintf(status, sizeof(status), "Sent SIGTERM to %s (%d)", p->name, p->pid);
                else snprintf(status, sizeof(status), "Cannot end %d: %s", p->pid, strerror(errno));
                last_proc_read = 0;
            }
        }
    }
    {
        char t[64];
        snprintf(t, sizeof(t), "%d processes (refreshed every second)", nprocs);
        ui_label(u->s, x + 160, y + h - 40, t, T->text_dim);
    }
}

/* ================= Disks ================= */

static void page_disks(ui_ctx *u, int x, int y, int w, int h)
{
    const struct ui_theme *T = ui_theme_get();
    static char buf[4096];
    char *line, *save;
    long n = rozos_mounts(buf, sizeof(buf) - 1);
    int yy = y + 4;
    if (n < 0) { ui_label(u->s, x, yy, "cannot read the mount list", T->text); return; }
    buf[n] = '\0';
    for (line = strtok_r(buf, "\n", &save); line && yy + 70 < y + h; line = strtok_r(NULL, "\n", &save)) {
        char dev[64], path[128], fs[32], kind[16] = "ram", label[64] = "-", t[260], a[32], b[32], c[32];
        const char *kname;
        struct statvfs v;
        unsigned long long total, freeb;
        if (sscanf(line, "%63s %127s %31s %15s %63[^\n]", dev, path, fs, kind, label) < 3) continue;
        if (statvfs(path, &v) < 0) continue;
        total = (unsigned long long)v.f_blocks * v.f_frsize;
        freeb = (unsigned long long)v.f_bfree * v.f_frsize;
        /* ধাপ ২৪ঘ: Windows-এর মতো পরিচয় — ভলিউমের নাম আর ডিস্কের ধরন */
        kname = !strcmp(kind, "nvme") ? "NVMe SSD" : !strcmp(kind, "sata") ? "SATA disk" :
                !strcmp(kind, "usb") ? "USB drive" : !strcmp(kind, "rozos-usb") ? "rozOS pen drive - your files" :
                "memory";
        snprintf(t, sizeof(t), "%s%s%s  -  %s  (%s, %s)%s", strcmp(label, "-") ? "\"" : "",
                 strcmp(label, "-") ? label : "", strcmp(label, "-") ? "\"  " : "", kname, path,
                 strcmp(dev, "-") ? dev : fs, v.f_flag & ST_RDONLY ? "  read-only" : "");
        ui_label(u->s, x, yy, t, T->text);
        /* ধাপ ২৪গ: ডিস্কে লেখা চালু/বন্ধ (NTFS সবসময় শুধু পড়া; "/" বদলানো যায় না) */
        if (strcmp(dev, "-") && strcmp(path, "/") && strcmp(path, "/boot") && strcmp(fs, "ntfs") && strcmp(fs, "tmpfs") && strcmp(fs, "devfs")) {
            int ro = (v.f_flag & ST_RDONLY) != 0;
            if (ui_do_button(u, x + w - 170, yy - 4, 150, 24, ro ? "Allow writing" : "Make read-only")) {
                char q[200];
                snprintf(q, sizeof(q), ro ? "Allow writing to the whole disk of /dev/%s?\n"
                                            "Only do this if you are sure - it may belong to another OS."
                                          : "Make the whole disk of /dev/%s read-only?", dev);
                if (ui_msgbox("Disks", q, UI_MB_YESNO | (ro ? UI_MB_WARN : UI_MB_QUESTION)) == UI_YES &&
                    rozos_diskwrite(dev, ro) < 0) {
                    snprintf(q, sizeof(q), "Could not change /dev/%s:\n%s", dev,
                             errno == EBUSY ? "files are open on it - close them first" : strerror(errno));
                    ui_msgbox("Disks", q, UI_MB_OK | UI_MB_ERROR);
                }
            }
        } else if (!strcmp(fs, "ntfs")) {
            ui_label(u->s, x + w - 170, yy, "NTFS: read-only", T->text_dim);
        }
        human(a, sizeof(a), total - freeb);
        human(b, sizeof(b), freeb);
        human(c, sizeof(c), total);
        if (total) {
            snprintf(t, sizeof(t), "used %s, free %s of %s", a, b, c);
            ui_label(u->s, x + 16, yy + 20, t, T->text_dim);
            bar(u->s, x + 16, yy + 40, w - 40, 16, total - freeb, total);
            yy += 70;
        } else {
            ui_label(u->s, x + 16, yy + 20, "(no size - a special file system)", T->text_dim);
            yy += 46;
        }
    }
}

/* ================= Network ================= */

static void page_network(ui_ctx *u, int x, int y, int w, int h)
{
    const struct ui_theme *T = ui_theme_get();
    struct rozos_netinfo ni;
    int i, yy = y + 4, any = 0;
    (void)w;
    for (i = 0; i < 4 && yy + 120 < y + h; i++) {
        struct in_addr a;
        char t[160], ip[20], gw[20], dns[20], mask[20];
        if (rozos_netinfo(i, &ni) < 0) break;
        any = 1;
        a.s_addr = ni.ip; snprintf(ip, sizeof(ip), "%s", inet_ntoa(a));
        a.s_addr = ni.netmask; snprintf(mask, sizeof(mask), "%s", inet_ntoa(a));
        a.s_addr = ni.gateway; snprintf(gw, sizeof(gw), "%s", inet_ntoa(a));
        a.s_addr = ni.dns; snprintf(dns, sizeof(dns), "%s", inet_ntoa(a));
        snprintf(t, sizeof(t), "%s  (%s)   link %s", ni.name, ni.driver, ni.link_up ? "up" : "down");
        ui_label(u->s, x, yy, t, T->text);
        snprintf(t, sizeof(t), "address %s / %s   %s", ip, mask, ni.dhcp ? "(DHCP)" : "(fixed)");
        ui_label(u->s, x + 16, yy + 22, t, T->text);
        snprintf(t, sizeof(t), "gateway %s   DNS %s", gw, dns);
        ui_label(u->s, x + 16, yy + 42, t, T->text);
        snprintf(t, sizeof(t), "MAC %02x:%02x:%02x:%02x:%02x:%02x", ni.mac[0], ni.mac[1], ni.mac[2], ni.mac[3], ni.mac[4], ni.mac[5]);
        ui_label(u->s, x + 16, yy + 62, t, T->text);
        snprintf(t, sizeof(t), "received %lu packets (%lu KiB), sent %lu packets (%lu KiB)", ni.rx_packets,
                 ni.rx_bytes / 1024, ni.tx_packets, ni.tx_bytes / 1024);
        ui_label(u->s, x + 16, yy + 82, t, T->text_dim);
        yy += 116;
    }
    if (!any) ui_label(u->s, x, yy, "No network card found.", T->text);
}

/* ================= Fonts (ধাপ ২৩ক) ================= */

#define FONT_DIR "/usr/share/fonts"
#define MAXFONT  48
static char *ui_files[MAXFONT], *mono_files[MAXFONT];
static char ui_names[MAXFONT][48], mono_names[MAXFONT][48];
static const char *ui_ptrs[MAXFONT], *mono_ptrs[MAXFONT];
static int n_ui, n_mono, ui_sel = -1, ui_top, mono_sel = -1, mono_top, ui_px = 14, mono_px = 15;
static int fonts_scanned;
static gfx_font *prev_ui, *prev_mono;
static int prev_ui_key = -1, prev_mono_key = -1;

static void font_label(const char *file, char *out, size_t n)
{
    char *dash;
    snprintf(out, n, "%s", file);
    if ((dash = strrchr(out, '.')) != NULL) *dash = '\0';
    if ((dash = strstr(out, "-Regular")) != NULL) *dash = '\0';
}

static void scan_fonts(void)
{
    DIR *d = opendir(FONT_DIR);
    struct dirent *e;
    char cur_ui[200], cur_mono[200], v[16];
    int i;
    fonts_scanned = 1;
    if (!d) return;
    while ((e = readdir(d)) != NULL) {
        const char *n = e->d_name, *dot = strrchr(n, '.');
        char *path;
        int mono;
        if (!dot || (strcasecmp(dot, ".ttf") && strcasecmp(dot, ".otf"))) continue;
        /* মোটা/তেরছা রূপ আলাদা দেখানো হয় না; বাংলা ফন্ট ধাপ ২৩খ-এর পরে */
        if (strchr(n, '-') && !strstr(n, "-Regular")) continue;
        if (strstr(n, "Bengali") || strstr(n, "Bangla") || strstr(n, "Siliguri")) continue;
        mono = strstr(n, "Mono") || strstr(n, "Code");
        if ((mono ? n_mono : n_ui) >= MAXFONT) continue;
        path = (char *)malloc(strlen(FONT_DIR) + strlen(n) + 2);
        if (!path) break;
        sprintf(path, "%s/%s", FONT_DIR, n);
        if (mono) { mono_files[n_mono] = path; font_label(n, mono_names[n_mono], 48); n_mono++; }
        else { ui_files[n_ui] = path; font_label(n, ui_names[n_ui], 48); n_ui++; }
    }
    closedir(d);
    conf_get("ui_font", cur_ui, sizeof(cur_ui), FONT_DIR "/Inter-Regular.ttf");
    conf_get("mono_font", cur_mono, sizeof(cur_mono), FONT_DIR "/JetBrainsMono-Regular.ttf");
    conf_get("ui_font_size", v, sizeof(v), "14");
    ui_px = atoi(v) >= 11 && atoi(v) <= 16 ? atoi(v) : 14;
    conf_get("mono_font_size", v, sizeof(v), "15");
    mono_px = atoi(v) >= 10 && atoi(v) <= 24 ? atoi(v) : 15;
    for (i = 0; i < n_ui; i++) { ui_ptrs[i] = ui_names[i]; if (!strcmp(ui_files[i], cur_ui)) ui_sel = i; }
    for (i = 0; i < n_mono; i++) { mono_ptrs[i] = mono_names[i]; if (!strcmp(mono_files[i], cur_mono)) mono_sel = i; }
}

/* − মাপ + */
static void size_box(ui_ctx *u, int x, int y, int *px, int lo, int hi)
{
    const struct ui_theme *T = ui_theme_get();
    char t[16];
    ui_label(u->s, x, y + 5, "Size", T->text_dim);
    if (ui_do_button(u, x + 44, y, 28, 26, "-") && *px > lo) (*px)--;
    snprintf(t, sizeof(t), "%d px", *px);
    ui_label(u->s, x + 80, y + 5, t, T->text);
    if (ui_do_button(u, x + 132, y, 28, 26, "+") && *px < hi) (*px)++;
}

static void preview(ui_ctx *u, gfx_font **f, int *key, const char *path, int px, int mono,
                    int x, int y, int w, int h, const char *text)
{
    const struct ui_theme *T = ui_theme_get();
    int k = px * 1000 + (path ? (int)(strlen(path) * 31 + path[strlen(path) - 5]) : 0);
    if (k != *key) {
        if (*f) gfx_font_free(*f);
        *f = path ? gfx_font_load_ttf(path, px, mono) : NULL;
        *key = k;
    }
    ui_sunken(u->s, x, y, w, h, T->field);
    gfx_set_clip(u->s, x + 2, y + 2, w - 4, h - 4);
    if (*f) gfx_text(u->s, *f, x + 10, y + 8, text, T->field_text);
    else ui_label(u->s, x + 10, y + 10, path ? "Cannot load this font." : "No TrueType support (FreeType).", T->text_dim);
    gfx_reset_clip(u->s);
}

static void page_fonts(ui_ctx *u, int x, int y, int w, int h)
{
    const struct ui_theme *T = ui_theme_get();
    int lw = 210, half = (h - 50) / 2;
    if (!fonts_scanned) scan_fonts();
    ui_label(u->s, x, y + 4, "Menus, buttons and titles", T->text);
    ui_do_list(u, x, y + 24, lw, half - 30, ui_ptrs, n_ui, &ui_sel, &ui_top);
    size_box(u, x + lw + 16, y + 24, &ui_px, 11, 16);
    preview(u, &prev_ui, &prev_ui_key, ui_sel >= 0 ? ui_files[ui_sel] : NULL, ui_px, 0,
            x + lw + 16, y + 58, w - lw - 26, half - 64,
            "The quick brown fox jumps over the lazy dog.\nFiles  Settings  Shut down  0123456789\n"
            "বাংলা: আমি, ক্ষমা, শিক্ষা, স্ত্রী, রাষ্ট্র, কার্তিক");

    y += half;
    ui_label(u->s, x, y + 4, "Terminal and Code", T->text);
    ui_do_list(u, x, y + 24, lw, half - 30, mono_ptrs, n_mono, &mono_sel, &mono_top);
    size_box(u, x + lw + 16, y + 24, &mono_px, 10, 24);
    preview(u, &prev_mono, &prev_mono_key, mono_sel >= 0 ? mono_files[mono_sel] : NULL, mono_px, 1,
            x + lw + 16, y + 58, w - lw - 26, half - 64,
            "int main(void) {\n    puts(\"Hello, rozOS!\");\n}   // 0O 1lI {}[]");

    y += half;
    if (ui_do_button(u, x, y + 6, 120, 28, "Apply")) {
        char v[16];
        int ok = 1;
        if (ui_sel >= 0) ok &= conf_set("ui_font", ui_files[ui_sel]) == 0;
        snprintf(v, sizeof(v), "%d", ui_px);
        ok &= conf_set("ui_font_size", v) == 0;
        if (mono_sel >= 0) ok &= conf_set("mono_font", mono_files[mono_sel]) == 0;
        snprintf(v, sizeof(v), "%d", mono_px);
        ok &= conf_set("mono_font_size", v) == 0;
        snprintf(status, sizeof(status), ok ? "Saved. New windows use it; Start > Restart for the whole desktop."
                                            : "Could not write " CONF ": %s", strerror(errno));
    }
    {
        /* ধাপ ২৩খ: বাংলা লেখার ফন্ট — বোতাম চাপলে পরেরটা, সাথে সাথে /etc/rozos.conf-এ */
        static const char *const bn_file[] = { "NotoSansBengali.ttf", "NotoSerifBengali.ttf",
                                               "HindSiliguri-Regular.ttf", "TiroBangla-Regular.ttf" };
        static const char *const bn_name[] = { "Noto Sans Bengali", "Noto Serif Bengali", "Hind Siliguri",
                                               "Tiro Bangla" };
        static int bn = -1;
        char lab[64], cur[200];
        int i;
        if (bn < 0) {
            conf_get("bengali_font", cur, sizeof(cur), FONT_DIR "/NotoSansBengali.ttf");
            bn = 0;
            for (i = 0; i < 4; i++) if (strstr(cur, bn_file[i])) bn = i;
        }
        snprintf(lab, sizeof(lab), "Bengali: %s", bn_name[bn]);
        if (ui_do_button(u, x + 260, y + 6, w - 270 > 300 ? 300 : w - 270, 28, lab)) {
            char path[200];
            bn = (bn + 1) % 4;
            snprintf(path, sizeof(path), FONT_DIR "/%s", bn_file[bn]);
            snprintf(status, sizeof(status), conf_set("bengali_font", path) == 0
                     ? "Bengali text now uses %s in new windows." : "Could not write " CONF, bn_name[bn]);
        }
    }
    if (ui_do_button(u, x + 130, y + 6, 120, 28, "Defaults")) {
        int i;
        ui_px = 14;
        mono_px = 15;
        for (i = 0; i < n_ui; i++) if (strstr(ui_files[i], "/Inter-Regular.ttf")) ui_sel = i;
        for (i = 0; i < n_mono; i++) if (strstr(mono_files[i], "/JetBrainsMono-Regular.ttf")) mono_sel = i;
        snprintf(status, sizeof(status), "Inter 14 and JetBrains Mono 15 - press Apply to keep them.");
    }
}

/* ================= main ================= */

int main(int argc, char **argv)
{
    ui_ctx ui;
    int i;
    if (argc > 1) {
        static const char *const keys[S_COUNT] = { "wallpaper", "icons", "system", "processes", "disks", "network",
                                                   "fonts" };
        for (i = 0; i < S_COUNT; i++) if (!strcmp(argv[1], keys[i])) section = i;
    }
    if (gfx_init_window("Settings", 780, 540) < 0) {
        perror("settings: /dev/fb0");
        return 1;
    }
    ui_init(&ui);
    read_wall_conf();
    load_images();
    rescan_icons();
    snprintf(status, sizeof(status), "rozOS settings");
    while (ui_run(&ui)) {
        const struct ui_theme *T = ui_theme_get();
        int W = ui.w, H = ui.h, px = NAV_W + 16, py = 12, pw = W - NAV_W - 26, ph = H - 12 - 34;
        gfx_clear(ui.s, T->window_bg);
        ui_panel(ui.s, 0, 0, NAV_W, H - 26, T->face, 1);
        for (i = 0; i < S_COUNT; i++) {
            int by = 10 + i * 34;
            if (ui_clicked(&ui, 6, by, NAV_W - 12, 30)) section = i;
            if (i == section) gfx_fill_rect(ui.s, 6, by, NAV_W - 12, 30, T->select);
            else if (ui_hover(&ui, 6, by, NAV_W - 12, 30)) gfx_fill_rect(ui.s, 6, by, NAV_W - 12, 30, T->face_hover);
            ui_label(ui.s, 16, by + 7, sections[i], T->text);
        }
        (void)nav_top;
        /* প্রসেস/সিস্টেম পাতায় প্রতি সেকেন্ডে নতুন ছবি */
        ui.tick_ms = section == S_PROC || section == S_SYSTEM || section == S_NET ? 1000 : 0;
        switch (section) {
        case S_WALL: page_wallpaper(&ui, px, py, pw, ph); break;
        case S_ICONS: page_icons(&ui, px, py, pw, ph); break;
        case S_SYSTEM: page_system(&ui, px, py, pw, ph); break;
        case S_PROC: page_processes(&ui, px, py, pw, ph); break;
        case S_DISKS: page_disks(&ui, px, py, pw, ph); break;
        case S_NET: page_network(&ui, px, py, pw, ph); break;
        case S_FONTS: page_fonts(&ui, px, py, pw, ph); break;
        }
        ui_statusbar(ui.s, 0, H - 26, W, 26, status);
        /* লেখার ঘর কী নিলে ui.key = 0 হয়ে যায় — তাই শেষে দেখা */
        if (ui.key == 'q' || ui.key == GFX_KEY_ESC) ui_quit(&ui);
    }
    gfx_quit();
    return 0;
}
