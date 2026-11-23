/* rozOS — rozwm: window server আর ডেস্কটপ (ধাপ ১৪)
 *
 *   rozwm [-t] [-k]   ডেস্কটপ চালু (-t: শুরুতেই একটা টার্মিনাল; -k: কী শুধু কিবোর্ড থেকে,
 *                     serial বাদ — init বুটের সময় এভাবে চালায়, serial-এ তখন আলাদা shell)।
 *                     টেক্সট মোড থেকে হাতে: `desktop`; বন্ধ: `textmode` বা Start > Text mode।
 *
 * পুরো পর্দার মালিক rozwm (/dev/fb0)। প্রতিটা অ্যাপ (librozgfx) Unix socket ROZWM_SOCKET-এ
 * যুক্ত হয়ে উইন্ডো চায়; rozwm তাকে একটা shared memory বাফার দেয়, অ্যাপ সেখানে আঁকে আর
 * "এই অংশ বদলেছে" (WM_DAMAGE) জানায়। rozwm নিজের back buffer-এ পটভূমি, উইন্ডো (3D ফ্রেম,
 * শিরোনাম, [_] [□] [X]), প্যানেল, মেনু আর মাউস-তীর সাজিয়ে শুধু বদলানো আয়তক্ষেত্রগুলো
 * পর্দায় কপি করে — তাই আধা-আঁকা ছবি কখনো দেখা যায় না (ঝিকমিক নেই)।
 *
 * পর্দা: ওপরে প্যানেল (rozOS, সামনের উইন্ডোর নাম, ঘড়ি), নিচে taskbar (Start, খোলা
 * উইন্ডোর বোতাম), মাঝে ডেস্কটপ (আইকনে ডাবল-ক্লিক)। রং: rozui-র গাঢ় থিম।
 * উইন্ডো: শিরোনাম টেনে সরানো, ডান-নিচের কোণ টেনে মাপ বদল, শিরোনামে ডাবল-ক্লিক = বড়/আগের
 * মাপ, [X] = অ্যাপকে বন্ধ হতে বলা (৩ সেকেন্ডে না গেলে বন্ধ করে দেওয়া)।
 * Start মেনু: /usr/share/apps-এর .app ফাইল (বিভাগ ধরে); টেক্সট প্রোগ্রাম টার্মিনাল-উইন্ডোতে
 * চলে (rozterm -e), "window: yes" থাকলে সরাসরি। নিচে Terminal, Text mode, Restart, Shut down।
 * Text mode: rozwm বন্ধ হয় — init তখন আগের মতো টেক্সট কনসোলে shell চালায়।
 * সেটিং: /etc/rozos.conf (utc_offset=+06:00, wallpaper=/path.jpg)। */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>
#include <time.h>
#include <poll.h>
#include <termios.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <rozgfx.h>
#include <rozui.h>
#include <rozwm_proto.h>
#include <rozlock.h>          /* ধাপ ১৯ছ: লক করা ফোল্ডার, .hidden */
#include <rozos.h>            /* ধাপ ২১: rozos_netinfo (tray) */

#define MAXWIN    32
#define MAXCLI    32
#define BORDER    4
#define TITLE_H   24
#define PANEL_H   27            /* ওপরের প্যানেল + নিচের রেখা */
#define TASK_H    30            /* নিচের taskbar (ওপরের রেখাসহ) */
#define MIN_W     120
#define MIN_H     40

struct client {
    int fd, pid, used;
    unsigned char rbuf[sizeof(struct wm_msg) * 4];
    size_t rlen;
};

struct window {
    int used, id;
    struct client *cl;
    char title[64];
    int x, y, w, h;                 /* ভেতরের (content) অংশ, পর্দার স্থানাঙ্কে */
    int minimized, maximized;
    int sx, sy, sw, sh;             /* বড় করার আগের মাপ */
    uint32_t *px;
    size_t size;
    char shm[48];
    int fixed;
    long close_at;                  /* [X]-এর পর কখন জোর করে বন্ধ */
};

static gfx_surface *S;
static int W, H;
static const struct ui_theme *T;
static struct client clients[MAXCLI];
static struct window wins[MAXWIN];
static int zorder[MAXWIN], nz;      /* নিচ থেকে ওপরে */
static int focus = -1;              /* wins[]-এর index */
static int next_id = 1;
static int listen_fd = -1;
static int mx, my, mbuttons;
static int running = 1;
static int utc_offset_min;
static gfx_surface *wallpaper;
static char exit_action;            /* 't' text, 'r' restart, 'p' poweroff */

/* ---------- damage ---------- */
#define MAXDMG 48
static struct { int x, y, w, h; } dmg[MAXDMG];
static int ndmg, dmg_full;

static void damage(int x, int y, int w, int h)
{
    int i;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0 || dmg_full) return;
    for (i = 0; i < ndmg; i++) {           /* ছোঁয়া/কাছের আয়তক্ষেত্র জোড়া লাগানো */
        int x0 = dmg[i].x < x ? dmg[i].x : x, y0 = dmg[i].y < y ? dmg[i].y : y;
        int x1 = dmg[i].x + dmg[i].w > x + w ? dmg[i].x + dmg[i].w : x + w;
        int y1 = dmg[i].y + dmg[i].h > y + h ? dmg[i].y + dmg[i].h : y + h;
        long area = (long)(x1 - x0) * (y1 - y0);
        if (area <= (long)dmg[i].w * dmg[i].h + (long)w * h + 4096) {
            dmg[i].x = x0; dmg[i].y = y0; dmg[i].w = x1 - x0; dmg[i].h = y1 - y0;
            return;
        }
    }
    if (ndmg == MAXDMG) { dmg_full = 1; return; }
    dmg[ndmg].x = x; dmg[ndmg].y = y; dmg[ndmg].w = w; dmg[ndmg].h = h;
    ndmg++;
}

static void damage_all(void) { dmg_full = 1; }

/* ---------- উইন্ডোর জ্যামিতি ---------- */
static void outer_rect(const struct window *w, int *x, int *y, int *ww, int *hh)
{
    *x = w->x - BORDER;
    *y = w->y - TITLE_H - BORDER;
    *ww = w->w + 2 * BORDER;
    *hh = w->h + TITLE_H + 2 * BORDER;
}

static void damage_window(const struct window *w)
{
    int x, y, ww, hh;
    outer_rect(w, &x, &y, &ww, &hh);
    damage(x, y, ww, hh);
}

static int work_top(void) { return PANEL_H; }
static int work_bottom(void) { return H - TASK_H; }

/* শিরোনামের বোতাম: 0 [X], 1 [□], 2 [_] */
static void title_button_rect(const struct window *w, int which, int *bx, int *by, int *bw, int *bh)
{
    *bw = 22;
    *bh = 18;
    *bx = w->x + w->w - (which + 1) * 24 + 2;
    *by = w->y - TITLE_H + (TITLE_H - 2 - 18) / 2;     /* ধাপ ১৪খ: ফিতার ঠিক মাঝখানে */
}

/* ---------- সময় ---------- */
static long now_ms(void) { return gfx_ticks_ms(); }

/* ---------- ক্লায়েন্টকে বার্তা (কখনো আটকে নয়) ---------- */
static void send_msg(struct client *c, uint32_t type, uint32_t win, int a, int b, int cc, int d,
                     uint32_t flags, const char *text)
{
    struct wm_msg m;
    if (!c || !c->used) return;
    memset(&m, 0, sizeof(m));
    m.type = type; m.win = win;
    m.a = a; m.b = b; m.c = cc; m.d = d;
    m.flags = flags;
    if (text) strncpy(m.text, text, sizeof(m.text) - 1);
    send(c->fd, &m, sizeof(m), MSG_DONTWAIT);     /* ভরা থাকলে বাদ — rozwm থামে না */
}

static void set_focus(int idx)
{
    int i, j;
    if (idx == focus) {
        if (idx < 0) return;
    } else {
        if (focus >= 0 && wins[focus].used) {
            send_msg(wins[focus].cl, WM_FOCUS, (uint32_t)wins[focus].id, 0, 0, 0, 0, 0, NULL);
            damage_window(&wins[focus]);
        }
        focus = idx;
        if (idx >= 0) send_msg(wins[idx].cl, WM_FOCUS, (uint32_t)wins[idx].id, 1, 0, 0, 0, 0, NULL);
    }
    if (idx < 0) {
        damage(0, 0, W, PANEL_H);
        damage(0, H - TASK_H, W, TASK_H);
        return;
    }
    /* z-ক্রমে সবার ওপরে */
    for (i = 0; i < nz; i++) {
        if (zorder[i] == idx) {
            for (j = i; j < nz - 1; j++) zorder[j] = zorder[j + 1];
            zorder[nz - 1] = idx;
            break;
        }
    }
    damage_window(&wins[idx]);
    damage(0, 0, W, PANEL_H);
    damage(0, H - TASK_H, W, TASK_H);
}

static void focus_top_visible(void)
{
    int i;
    for (i = nz - 1; i >= 0; i--) {
        if (!wins[zorder[i]].minimized) {
            set_focus(zorder[i]);
            return;
        }
    }
    set_focus(-1);
}

static void destroy_window(int idx)
{
    struct window *w = &wins[idx];
    int i, j;
    if (!w->used) return;
    if (!w->minimized) damage_window(w);
    if (w->px) munmap(w->px, w->size);
    shm_unlink(w->shm);
    w->used = 0;
    w->px = NULL;
    for (i = 0; i < nz; i++) {
        if (zorder[i] == idx) {
            for (j = i; j < nz - 1; j++) zorder[j] = zorder[j + 1];
            nz--;
            break;
        }
    }
    damage(0, H - TASK_H, W, TASK_H);
    damage(0, 0, W, PANEL_H);
    if (focus == idx) {
        focus = -1;
        focus_top_visible();
    }
}

static void drop_client(struct client *c)
{
    int i;
    for (i = 0; i < MAXWIN; i++) if (wins[i].used && wins[i].cl == c) destroy_window(i);
    close(c->fd);
    c->used = 0;
}

static void clamp_window(struct window *w)
{
    int top = work_top() + TITLE_H + BORDER, bottom = work_bottom();
    if (w->w > W - 2 * BORDER) w->w = W - 2 * BORDER;
    if (w->h > bottom - top - BORDER) w->h = bottom - top - BORDER;
    if (w->w < MIN_W) w->w = MIN_W;
    if (w->h < MIN_H) w->h = MIN_H;
    if (w->x < BORDER - w->w + 60) w->x = BORDER - w->w + 60;      /* অন্তত একটু দেখা যাক */
    if (w->x > W - 60) w->x = W - 60;
    if (w->y < top) w->y = top;
    if (w->y > bottom - 20) w->y = bottom - 20;
}

static void create_window(struct client *c, const struct wm_msg *m)
{
    static int cascade;
    struct window *w = NULL;
    int i, fd, idx = -1;
    for (i = 0; i < MAXWIN; i++) if (!wins[i].used) { w = &wins[i]; idx = i; break; }
    if (!w) return;
    memset(w, 0, sizeof(*w));
    w->id = next_id++;
    w->cl = c;
    strncpy(w->title, m->text[0] ? m->text : "window", sizeof(w->title) - 1);
    w->w = m->a > 0 ? m->a : W * 3 / 4;
    w->h = m->b > 0 ? m->b : (work_bottom() - work_top()) * 3 / 4;
    w->fixed = (m->flags & WMF_FIXED_SIZE) != 0;
    if (m->c >= 0 && m->d >= 0) {
        w->x = m->c;
        w->y = m->d;
    } else {
        w->x = 120 + cascade * 28;                   /* ধাপ ১৪খ: ডেস্কটপ-আইকনের ডানে */
        w->y = work_top() + TITLE_H + BORDER + 20 + cascade * 28;
        cascade = (cascade + 1) % 8;
        if ((m->flags & WMF_CENTER) || w->x + w->w > W - 10) {
            w->x = (W - w->w) / 2;
            w->y = work_top() + (work_bottom() - work_top() - w->h - TITLE_H) / 2 + TITLE_H;
        }
    }
    clamp_window(w);
    snprintf(w->shm, sizeof(w->shm), "/rozwm-%d-%d", c->pid, w->id);
    w->size = (size_t)W * (size_t)H * 4;
    shm_unlink(w->shm);
    fd = shm_open(w->shm, O_CREAT | O_RDWR, 0600);
    if (fd < 0 || ftruncate(fd, (off_t)w->size) < 0) {
        if (fd >= 0) close(fd);
        return;
    }
    w->px = (uint32_t *)mmap(NULL, w->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (w->px == (uint32_t *)MAP_FAILED) {
        w->px = NULL;
        shm_unlink(w->shm);
        return;
    }
    for (i = 0; i < W * H; i++) w->px[i] = T->window_bg;
    w->used = 1;
    zorder[nz++] = idx;
    send_msg(c, WM_CREATED, (uint32_t)w->id, w->w, w->h, W, H, 0, w->shm);
    set_focus(idx);
}

static int find_win(struct client *c, uint32_t id)
{
    int i;
    for (i = 0; i < MAXWIN; i++) if (wins[i].used && wins[i].cl == c && (uint32_t)wins[i].id == id) return i;
    return -1;
}

static void handle_msg(struct client *c, const struct wm_msg *m)
{
    int i;
    switch (m->type) {
    case WM_HELLO:
        c->pid = m->a;
        break;
    case WM_CREATE:
        create_window(c, m);
        break;
    case WM_DAMAGE:
        i = find_win(c, m->win);
        if (i >= 0 && !wins[i].minimized) {
            int x = m->a, y = m->b, ww = m->c, hh = m->d;
            if (x < 0) { ww += x; x = 0; }
            if (y < 0) { hh += y; y = 0; }
            if (x + ww > wins[i].w) ww = wins[i].w - x;
            if (y + hh > wins[i].h) hh = wins[i].h - y;
            damage(wins[i].x + x, wins[i].y + y, ww, hh);
        }
        break;
    case WM_TITLE:
        i = find_win(c, m->win);
        if (i >= 0) {
            strncpy(wins[i].title, m->text, sizeof(wins[i].title) - 1);
            wins[i].title[sizeof(wins[i].title) - 1] = '\0';
            damage_window(&wins[i]);
            damage(0, 0, W, PANEL_H);
            damage(0, H - TASK_H, W, TASK_H);
        }
        break;
    case WM_DESTROY:
        i = find_win(c, m->win);
        if (i >= 0) destroy_window(i);
        break;
    case WM_QUIT:
        exit_action = (char)m->a;
        running = 0;
        break;
    case WM_KEEP_OPEN:                               /* ধাপ ১৪খ: অ্যাপ "Save?" জিজ্ঞেস করছে */
        i = find_win(c, m->win);
        if (i >= 0) wins[i].close_at = 0;
        break;
    }
}

static void read_client(struct client *c)
{
    ssize_t n = recv(c->fd, c->rbuf + c->rlen, sizeof(c->rbuf) - c->rlen, MSG_DONTWAIT);
    if (n == 0 || (n < 0 && errno != EAGAIN)) {
        drop_client(c);
        return;
    }
    if (n < 0) return;
    c->rlen += (size_t)n;
    while (c->rlen >= sizeof(struct wm_msg)) {
        struct wm_msg m;
        memcpy(&m, c->rbuf, sizeof(m));
        memmove(c->rbuf, c->rbuf + sizeof(m), c->rlen - sizeof(m));
        c->rlen -= sizeof(m);
        handle_msg(c, &m);
        if (!c->used) return;
    }
}

/* ---------- অ্যাপ চালানো ---------- */

static void spawn(const char *cmd)
{
    pid_t pid = fork();
    if (pid == 0) {
        int fd;
        setsid();
        for (fd = 3; fd < 64; fd++) close(fd);
        setenv("ROZWM_SERVER", "0", 1);
        chdir(getenv("HOME") ? getenv("HOME") : "/home");
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
}

static void spawn_terminal(const char *cmd, int wait_key)
{
    char buf[600];
    if (!cmd) {
        spawn("exec rozterm");
        return;
    }
    snprintf(buf, sizeof(buf), "exec rozterm %s-e '%s'", wait_key ? "-w " : "", cmd);
    spawn(buf);
}

/* ---------- Start মেনু ---------- */
#define MAXAPPS 64
#define MAXCATS 12
struct app {
    char name[48], category[24], run[160], about[96];
    char icon[64];                    /* ধাপ ১৪খ: icon: নাম (/usr/share/icons/নাম.png) বা পুরো পথ */
    int wait, window;
};
static struct app apps[MAXAPPS];
static int napps;
static char cats[MAXCATS][24];
static int ncats;

static void load_apps(void)
{
    DIR *d = opendir("/usr/share/apps");
    struct dirent *e;
    napps = ncats = 0;
    if (!d) return;
    while ((e = readdir(d)) && napps < MAXAPPS) {
        char path[300], line[256];
        FILE *f;
        struct app *a = &apps[napps];
        size_t n = strlen(e->d_name);
        if (n < 5 || strcmp(e->d_name + n - 4, ".app")) continue;
        snprintf(path, sizeof(path), "/usr/share/apps/%s", e->d_name);
        f = fopen(path, "r");
        if (!f) continue;
        memset(a, 0, sizeof(*a));
        while (fgets(line, sizeof(line), f)) {
            char *v = strchr(line, ':');
            if (line[0] == '#' || !v) continue;
            *v++ = '\0';
            while (*v == ' ') v++;
            v[strcspn(v, "\r\n")] = '\0';
            if (!strcmp(line, "name")) strncpy(a->name, v, sizeof(a->name) - 1);
            else if (!strcmp(line, "category")) strncpy(a->category, v, sizeof(a->category) - 1);
            else if (!strcmp(line, "run")) strncpy(a->run, v, sizeof(a->run) - 1);
            else if (!strcmp(line, "about")) strncpy(a->about, v, sizeof(a->about) - 1);
            else if (!strcmp(line, "wait")) a->wait = (v[0] == 'y');
            else if (!strcmp(line, "window")) a->window = (v[0] == 'y');
            else if (!strcmp(line, "icon")) strncpy(a->icon, v, sizeof(a->icon) - 1);
        }
        fclose(f);
        if (!a->name[0] || !a->run[0]) continue;
        if (!a->category[0]) strcpy(a->category, "Other");
        if (!strcmp(a->category, "Graphics")) a->window = 1;
        {
            int i, found = 0;
            for (i = 0; i < ncats; i++) if (!strcmp(cats[i], a->category)) found = 1;
            if (!found && ncats < MAXCATS) strcpy(cats[ncats++], a->category);
        }
        napps++;
    }
    closedir(d);
    /* বিভাগ আর অ্যাপ নাম ধরে সাজানো (ছোট তালিকা — সাধারণ বাছাই) */
    {
        int i, j;
        for (i = 0; i < ncats; i++)
            for (j = i + 1; j < ncats; j++)
                if (strcmp(cats[i], cats[j]) > 0) {
                    char t[24];
                    strcpy(t, cats[i]); strcpy(cats[i], cats[j]); strcpy(cats[j], t);
                }
        for (i = 0; i < napps; i++)
            for (j = i + 1; j < napps; j++)
                if (strcmp(apps[i].name, apps[j].name) > 0) {
                    struct app t = apps[i]; apps[i] = apps[j]; apps[j] = t;
                }
    }
}

#define MENU_W   200
#define SUB_W    260
#define ITEM_H   24
enum { FIXED_TERM = 0, FIXED_ABOUT, FIXED_TEXT, FIXED_RESTART, FIXED_OFF, NFIXED };   /* ধাপ ২১: About */
static const char *const fixed_names[NFIXED] = { "Terminal", "About rozOS", "Text mode", "Restart", "Shut down" };
static int menu_open, menu_hover = -1, sub_cat = -1, sub_hover = -1;
static int menu_x, menu_y, menu_h, sub_x, sub_y, sub_h, sub_n;

/* প্রধান মেনুর সারি: 0..ncats-1 বিভাগ, তারপর বিভাজক, তারপর NFIXED */
static int menu_rows(void) { return ncats + NFIXED; }

static void menu_geometry(void)
{
    menu_h = menu_rows() * ITEM_H + 8 + 10 + 30;     /* +বিভাজক +শিরোনাম-ফিতা */
    menu_x = 2;
    menu_y = H - TASK_H - menu_h;
    if (menu_y < PANEL_H) menu_y = PANEL_H;
}

static void sub_geometry(void)
{
    int i;
    sub_n = 0;
    if (sub_cat < 0) return;
    for (i = 0; i < napps; i++) if (!strcmp(apps[i].category, cats[sub_cat])) sub_n++;
    sub_h = sub_n * ITEM_H + 8;
    sub_x = menu_x + MENU_W - 2;
    sub_y = menu_y + 30 + 4 + sub_cat * ITEM_H - 4;
    if (sub_y + sub_h > H - TASK_H) sub_y = H - TASK_H - sub_h;
    if (sub_y < PANEL_H) sub_y = PANEL_H;
}

static int sub_app(int row)
{
    int i, k = 0;
    for (i = 0; i < napps; i++) {
        if (!strcmp(apps[i].category, cats[sub_cat])) {
            if (k == row) return i;
            k++;
        }
    }
    return -1;
}

static void damage_menu(void)
{
    damage(menu_x, menu_y, MENU_W, menu_h);
    if (sub_cat >= 0) damage(sub_x, sub_y, SUB_W, sub_h);
}

static void open_menu(int on)
{
    if (menu_open) damage_menu();
    menu_open = on;
    menu_hover = sub_hover = -1;
    sub_cat = -1;
    if (on) {
        load_apps();
        menu_geometry();
        damage_menu();
    }
    damage(0, H - TASK_H, 110, TASK_H);
}

static void draw_menu(void)
{
    int i, y;
    if (!menu_open) return;
    ui_panel(S, menu_x, menu_y, MENU_W, menu_h, T->face, 1);
    gfx_gradient_v(S, menu_x + 3, menu_y + 3, MENU_W - 6, 26, T->title_a1, T->title_a2);
    gfx_text(S, NULL, menu_x + 12, menu_y + 8, "rozOS", GFX_WHITE);
    y = menu_y + 34;
    for (i = 0; i < ncats; i++, y += ITEM_H) {
        ui_menu_item(S, menu_x + 4, y, MENU_W - 8, ITEM_H, cats[i], i == menu_hover || i == sub_cat);
        gfx_text(S, NULL, menu_x + MENU_W - 20, y + 4, ">", T->text_dim);
    }
    ui_separator(S, menu_x + 6, y + 4, MENU_W - 12);
    y += 10;
    for (i = 0; i < NFIXED; i++, y += ITEM_H) {
        ui_menu_item(S, menu_x + 4, y, MENU_W - 8, ITEM_H, fixed_names[i], ncats + i == menu_hover);
    }
    if (sub_cat >= 0 && sub_n > 0) {
        ui_panel(S, sub_x, sub_y, SUB_W, sub_h, T->face, 1);
        for (i = 0; i < sub_n; i++) {
            int a = sub_app(i);
            if (a >= 0) {
                int iy = sub_y + 4 + i * ITEM_H;
                char lbl[64];
                /* ধাপ ১৪খ: নামের আগে ছোট আইকন (ছবি থাকলে ছবি) */
                snprintf(lbl, sizeof(lbl), "   %s", apps[a].name);
                ui_menu_item(S, sub_x + 4, iy, SUB_W - 8, ITEM_H, lbl, i == sub_hover);
                ui_draw_icon(S, sub_x + 9, iy + (ITEM_H - 16) / 2, 16, apps[a].icon,
                             !strcmp(apps[a].icon, "code") ? 'e' : !strcmp(apps[a].icon, "files") ? 'f'
                             : apps[a].window ? 'g' : 't');
            }
        }
    }
}

/* মেনুতে মাউস: 1 = মেনুর ওপরে ছিল (ঘটনা শেষ) */
static int menu_mouse(int type, int x, int y)
{
    int row = -1, srow = -1, in_main, in_sub;
    if (!menu_open) return 0;
    in_main = x >= menu_x && x < menu_x + MENU_W && y >= menu_y && y < menu_y + menu_h;
    in_sub = sub_cat >= 0 && x >= sub_x && x < sub_x + SUB_W && y >= sub_y && y < sub_y + sub_h;
    if (in_sub) {
        srow = (y - sub_y - 4) / ITEM_H;
        if (srow < 0 || srow >= sub_n) srow = -1;
        if (srow != sub_hover) { sub_hover = srow; damage_menu(); }
        if (type == GFX_EV_MOUSE_UP && srow >= 0) {
            int a = sub_app(srow);
            open_menu(0);
            if (a >= 0) {
                if (apps[a].window) spawn(apps[a].run);
                else spawn_terminal(apps[a].run, apps[a].wait);
            }
        }
        return 1;
    }
    if (in_main) {
        int yy = y - menu_y - 34;
        if (yy >= 0 && yy < ncats * ITEM_H) row = yy / ITEM_H;
        else if (yy >= ncats * ITEM_H + 10) {
            int f = (yy - ncats * ITEM_H - 10) / ITEM_H;
            if (f < NFIXED) row = ncats + f;
        }
        if (row != menu_hover) {
            damage_menu();
            menu_hover = row;
            if (row >= 0 && row < ncats && row != sub_cat) {
                sub_cat = row;
                sub_hover = -1;
                sub_geometry();
            } else if (row >= ncats) {
                sub_cat = -1;
            }
            damage_menu();
        }
        if (type == GFX_EV_MOUSE_UP && row >= ncats) {
            int f = row - ncats;
            open_menu(0);
            if (f == FIXED_TERM) spawn_terminal(NULL, 0);
            else if (f == FIXED_ABOUT) spawn("about");
            else { exit_action = f == FIXED_TEXT ? 't' : f == FIXED_RESTART ? 'r' : 'p'; running = 0; }
        }
        return 1;
    }
    if (type == GFX_EV_MOUSE_DOWN) {
        open_menu(0);
        return y >= H - TASK_H && x < 100;          /* Start-এ আবার চাপা = শুধু বন্ধ */
    }
    return 0;
}

/* ---------- ডেস্কটপের আইকন ---------- */
/* ধাপ ১৪খ: image = /usr/share/icons/<image>.png (বা .bmp/.jpg) থাকলে সেই ছবি, না থাকলে আঁকা চিহ্ন */
struct dicon { const char *label; char kind; const char *cmd; int term; const char *image; };
static const struct dicon icons[] = {
    { "Terminal",   't', NULL, 1, "terminal" },
    { "Files",      'f', "files", 0, "files" },
    { "Code",       'e', "code", 0, "code" },
    { "Calculator", 'g', "gcalc", 0, "calculator" },
    { "Paint",      'g', "paint", 0, "paint" },
    { "Images",     'g', "imgview", 0, "images" },
    { "Help",       'e', "help", 1, "help" },
};
#define NICONS ((int)(sizeof(icons) / sizeof(icons[0])))
static int icon_sel = -1;

/* ধাপ ১৯চ: নিজের আইকন — /home/Desktop-এর ফাইল। .app = প্রোগ্রামের শর্টকাট (Start মেনুর মতো
 * name/run/icon/window), ফোল্ডার → Files, ছবি → imgview, অন্য ফাইল → Code। আগের ৭টার পরে বসে। */
#define DESKTOP_DIR "/home/Desktop"
#define MAXUSER 48
enum { UK_APP, UK_DIR, UK_IMAGE, UK_FILE, UK_LOCKED };   /* ধাপ ১৯ছ: UK_LOCKED = NAME.rozlock */
struct uicon { char label[40], path[200], run[200], image[64]; int kind, window, hidden; };
static struct uicon uicons[MAXUSER];
static int nuser;
static unsigned long desk_sig;         /* ফোল্ডারের নামগুলোর ছাপ — বদলালে আবার পড়া */
static int desk_show_hidden;           /* ধাপ ১৯ছ: ডান-ক্লিক > Show hidden icons */

static int nicons_total(void) { return NICONS + nuser; }

static int is_picture(const char *n)
{
    static const char *const ext[] = { ".jpg", ".jpeg", ".png", ".bmp", ".gif", ".tga", ".ppm" };
    const char *dot = strrchr(n, '.');
    size_t i;
    if (!dot) return 0;
    for (i = 0; i < sizeof(ext) / sizeof(ext[0]); i++) if (!strcasecmp(dot, ext[i])) return 1;
    return 0;
}

static int cmp_uicon(const void *a, const void *b)
{
    return strcasecmp(((const struct uicon *)a)->label, ((const struct uicon *)b)->label);
}

static void read_app_file(struct uicon *u)
{
    FILE *f = fopen(u->path, "r");
    char line[256];
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        char *v = strchr(line, ':');
        if (line[0] == '#' || !v) continue;
        *v++ = '\0';
        while (*v == ' ') v++;
        v[strcspn(v, "\r\n")] = '\0';
        if (!strcmp(line, "name")) snprintf(u->label, sizeof(u->label), "%s", v);
        else if (!strcmp(line, "run")) snprintf(u->run, sizeof(u->run), "%s", v);
        else if (!strcmp(line, "icon")) snprintf(u->image, sizeof(u->image), "%s", v);
        else if (!strcmp(line, "window")) u->window = !strcmp(v, "yes");
    }
    fclose(f);
}

/* /home/Desktop পড়া; বদলেছে কিনা ফেরত (তাহলে আইকন আবার আঁকা লাগে) */
static int load_user_icons(void)
{
    DIR *d = opendir(DESKTOP_DIR);
    struct dirent *e;
    unsigned long sig = 5381;
    int n = 0, i;
    struct uicon tmp[MAXUSER];
    struct rozhide hid;
    rozhide_load(&hid, DESKTOP_DIR);             /* ধাপ ১৯ছ: লুকানো নামের তালিকা */
    for (i = 0; i < hid.n; i++) {
        const char *c;
        for (c = hid.names[i]; *c; c++) sig = sig * 31 + (unsigned char)*c;
    }
    sig = sig * 7 + (unsigned long)desk_show_hidden;
    if (d) {
        while ((e = readdir(d)) && n < MAXUSER) {
            struct uicon *u = &tmp[n];
            const char *c;
            size_t L = strlen(e->d_name);
            int hidden = rozhide_has(&hid, e->d_name);
            if (e->d_name[0] == '.') continue;
            if (hidden && !desk_show_hidden) continue;
            for (c = e->d_name; *c; c++) sig = sig * 33 + (unsigned char)*c;
            sig = sig * 33 + (unsigned long)e->d_type;
            memset(u, 0, sizeof(*u));
            u->hidden = hidden;
            snprintf(u->path, sizeof(u->path), "%s/%s", DESKTOP_DIR, e->d_name);
            snprintf(u->label, sizeof(u->label), "%s", e->d_name);
            if (e->d_type == DT_DIR) u->kind = UK_DIR;
            else if (L > strlen(ROZLOCK_EXT) && !strcmp(e->d_name + L - strlen(ROZLOCK_EXT), ROZLOCK_EXT) &&
                     rozlock_is_lockfile(u->path)) {
                u->kind = UK_LOCKED;
                rozlock_display_name(e->d_name, u->label, sizeof(u->label));
            }
            else if (strlen(e->d_name) > 4 && !strcmp(e->d_name + strlen(e->d_name) - 4, ".app")) {
                u->kind = UK_APP;
                u->label[strlen(u->label) - 4] = '\0';
                read_app_file(u);
            } else if (is_picture(e->d_name)) u->kind = UK_IMAGE;
            else u->kind = UK_FILE;
            n++;
        }
        closedir(d);
    }
    rozhide_free(&hid);
    if (sig == desk_sig && n == nuser) return 0;
    desk_sig = sig;
    qsort(tmp, (size_t)n, sizeof(tmp[0]), cmp_uicon);
    memcpy(uicons, tmp, sizeof(tmp[0]) * (size_t)n);
    nuser = n;
    if (icon_sel >= nicons_total()) icon_sel = -1;
    return 1;
}

static void icon_rect(int i, int *x, int *y, int *w, int *h)
{
    int per = (H - TASK_H - PANEL_H - 12) / 72;       /* ধাপ ১৪খ: জায়গা না হলে পাশের কলামে */
    if (per < 1) per = 1;
    *x = 16 + (i / per) * 84;
    *y = PANEL_H + 12 + (i % per) * 72;
    *w = 72;
    *h = 66;
}

static const char *icon_label(int i) { return i < NICONS ? icons[i].label : uicons[i - NICONS].label; }

static void draw_icons(void)
{
    int i;
    for (i = 0; i < nicons_total(); i++) {
        int x, y, w, h, tw;
        char lbl[16];
        icon_rect(i, &x, &y, &w, &h);
        if (i == icon_sel) gfx_fill_round_rect(S, x, y, w, h, 6, T->select);
        /* ধাপ ১৪খ: ছবি থাকলে ৩২x৩২ ছবি, নইলে 3D বাক্সে আঁকা চিহ্ন */
        if (i < NICONS) ui_draw_icon(S, x + 20, y + 6, 32, icons[i].image, icons[i].kind);
        else {
            const struct uicon *u = &uicons[i - NICONS];
            char k = u->kind == UK_DIR ? 'f' : u->kind == UK_IMAGE ? 'i' : u->kind == UK_FILE ? 'd' :
                     u->kind == UK_LOCKED ? 'l' : u->window ? 'g' : 't';
            ui_draw_icon(S, x + 20, y + 6, 32, u->kind == UK_IMAGE ? u->path : u->kind == UK_LOCKED ? "locked-folder" : u->image, k);
        }
        /* ধাপ ১৯চ: লম্বা নাম কেটে */
        snprintf(lbl, sizeof(lbl), "%s", icon_label(i));
        if (strlen(icon_label(i)) > 11) { lbl[9] = '.'; lbl[10] = '.'; lbl[11] = '\0'; }   /* আগের ৭টার নাম পুরো */
        tw = gfx_text_width(NULL, lbl);
        gfx_text(S, NULL, x + (w - tw) / 2, y + 44, lbl, i == icon_sel ? GFX_WHITE :
                 (i >= NICONS && uicons[i - NICONS].hidden) ? T->text_dim : T->text);   /* ধাপ ১৯ছ: লুকানো = ঝাপসা */
    }
}

static int icon_at(int px, int py)
{
    int i;
    for (i = 0; i < nicons_total(); i++) {
        int x, y, w, h;
        icon_rect(i, &x, &y, &w, &h);
        if (px >= x && px < x + w && py >= y && py < y + h) return i;
    }
    return -1;
}

static void launch_icon(int i)
{
    char cmd[480];
    const struct uicon *u;
    if (i < NICONS) {
        if (icons[i].term) spawn_terminal(icons[i].cmd, 0);
        else spawn(icons[i].cmd);
        return;
    }
    u = &uicons[i - NICONS];
    if (u->kind == UK_APP) {
        if (!u->run[0]) return;
        if (u->window) spawn(u->run);
        else spawn_terminal(u->run, 1);
        return;
    }
    if (u->kind == UK_LOCKED) {                  /* ধাপ ১৯ছ: secret চেয়ে খোলা, তারপর Files-এ */
        snprintf(cmd, sizeof(cmd), "lockdlg unlock '%s' -o", u->path);
        spawn(cmd);
        return;
    }
    snprintf(cmd, sizeof(cmd), "%s '%s'", u->kind == UK_DIR ? "files" : u->kind == UK_IMAGE ? "imgview" : "code", u->path);
    spawn(cmd);
}

static void damage_icons(void) { damage(0, PANEL_H, W, H - PANEL_H - TASK_H); }

/* ---------- ধাপ ১৯চ: ডেস্কটপের ডান-ক্লিক মেনু ---------- */
enum { CX_NEWTEXT = 1, CX_NEWFOLDER, CX_ADDICON, CX_WALLPAPER, CX_SETTINGS, CX_REFRESH,
       CX_OPEN, CX_REMOVE, CX_SHOWFILES,
       CX_LOCK, CX_UNLOCK, CX_HIDE, CX_SHOWHIDDEN,                   /* ধাপ ১৯ছ */
       CX_ABOUT };                                                    /* ধাপ ২১ */
struct ctxitem { const char *label; int id; };
static const struct ctxitem ctx_desktop[] = {
    { "New text file", CX_NEWTEXT }, { "New folder", CX_NEWFOLDER }, { "-", 0 },
    { "Add icon...", CX_ADDICON }, { "Change wallpaper...", CX_WALLPAPER }, { "Settings...", CX_SETTINGS },
    { "-", 0 }, { "Show hidden icons", CX_SHOWHIDDEN }, { "Refresh", CX_REFRESH }, { "About rozOS", CX_ABOUT }
};
static const struct ctxitem ctx_builtin[] = { { "Open", CX_OPEN } };
static const struct ctxitem ctx_app[] = { { "Open", CX_OPEN }, { "Remove from desktop", CX_REMOVE } };
static const struct ctxitem ctx_file[] = { { "Open", CX_OPEN }, { "Show in Files", CX_SHOWFILES } };
/* ধাপ ১৯ছ: ফোল্ডার আর লক করা ফোল্ডার; সবশেষে Hide/Unhide যোগ হয় (ctx_show) */
static const struct ctxitem ctx_dir[] = { { "Open", CX_OPEN }, { "Show in Files", CX_SHOWFILES }, { "-", 0 },
                                          { "Lock...", CX_LOCK } };
static const struct ctxitem ctx_locked[] = { { "Unlock...", CX_UNLOCK }, { "Show in Files", CX_SHOWFILES }, { "-", 0 } };
static struct ctxitem ctx_dyn[12];
#define CTX_W 190
static const struct ctxitem *ctx_items;
static int ctx_n, ctx_open, ctx_x, ctx_y, ctx_h, ctx_hover = -1, ctx_icon = -1;

static int ctx_row_y(int r)
{
    int i, y = ctx_y + 4;
    for (i = 0; i < r; i++) y += ctx_items[i].label[0] == '-' ? 8 : ITEM_H;
    return y;
}

static void ctx_close(void)
{
    if (ctx_open) damage(ctx_x, ctx_y, CTX_W, ctx_h);
    ctx_open = 0;
}

static void ctx_show(int x, int y, int icon)
{
    ctx_close();
    ctx_icon = icon;
    if (icon < 0) {
        ctx_n = (int)(sizeof(ctx_desktop) / sizeof(ctx_desktop[0]));
        memcpy(ctx_dyn, ctx_desktop, sizeof(ctx_desktop));
        if (desk_show_hidden) ctx_dyn[7].label = "Hide hidden icons";
        ctx_items = ctx_dyn;
    }
    else if (icon < NICONS) { ctx_items = ctx_builtin; ctx_n = 1; }
    else {                                       /* ধাপ ১৯ছ: নিজের আইকন — ধরন অনুযায়ী, শেষে Hide/Unhide */
        const struct uicon *u = &uicons[icon - NICONS];
        const struct ctxitem *src = u->kind == UK_APP ? ctx_app : u->kind == UK_DIR ? ctx_dir :
                                    u->kind == UK_LOCKED ? ctx_locked : ctx_file;
        int n = u->kind == UK_APP ? 2 : u->kind == UK_DIR ? 4 : u->kind == UK_LOCKED ? 3 : 2;
        memcpy(ctx_dyn, src, sizeof(ctx_dyn[0]) * (size_t)n);
        if (src[n - 1].label[0] != '-') { ctx_dyn[n].label = "-"; ctx_dyn[n++].id = 0; }
        ctx_dyn[n].label = u->hidden ? "Unhide" : "Hide";
        ctx_dyn[n++].id = CX_HIDE;
        ctx_items = ctx_dyn;
        ctx_n = n;
    }
    ctx_open = 1;
    ctx_hover = -1;
    ctx_h = ctx_row_y(ctx_n) - ctx_y + 4;
    ctx_x = x + CTX_W > W ? W - CTX_W : x;
    ctx_y = y + ctx_h > H - TASK_H ? H - TASK_H - ctx_h : y;
    damage(ctx_x, ctx_y, CTX_W, ctx_h);
}

static void draw_ctx(void)
{
    int i;
    if (!ctx_open) return;
    ui_panel(S, ctx_x, ctx_y, CTX_W, ctx_h, T->face, 1);
    for (i = 0; i < ctx_n; i++) {
        int y = ctx_row_y(i);
        if (ctx_items[i].label[0] == '-') ui_separator(S, ctx_x + 6, y + 3, CTX_W - 12);
        else ui_menu_item(S, ctx_x + 4, y, CTX_W - 8, ITEM_H, ctx_items[i].label, i == ctx_hover);
    }
}

/* নতুন নাম যা আগে নেই: "New text file.txt", "New text file 2.txt" ... */
static void unique_name(char *out, size_t size, const char *base, const char *ext)
{
    int n;
    struct stat st;
    for (n = 1; n < 1000; n++) {
        if (n == 1) snprintf(out, size, "%s/%s%s", DESKTOP_DIR, base, ext);
        else snprintf(out, size, "%s/%s %d%s", DESKTOP_DIR, base, n, ext);
        if (stat(out, &st) < 0) return;
    }
}

static void load_config(void);

static void ctx_do(int id)
{
    char path[240], cmd[480];
    int ic = ctx_icon;
    mkdir("/home", 0755);
    mkdir(DESKTOP_DIR, 0755);
    switch (id) {
    case CX_NEWTEXT: {
        int fd;
        unique_name(path, sizeof(path), "New text file", ".txt");
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd >= 0) close(fd);
        snprintf(cmd, sizeof(cmd), "code '%s'", path);
        spawn(cmd);
        break;
    }
    case CX_NEWFOLDER:
        unique_name(path, sizeof(path), "New folder", "");
        mkdir(path, 0755);
        break;
    case CX_ADDICON: spawn("settings icons"); break;
    case CX_WALLPAPER: spawn("settings wallpaper"); break;
    case CX_SETTINGS: spawn("settings"); break;
    case CX_REFRESH: desk_sig = 0; load_config(); damage(0, 0, W, H); break;
    case CX_OPEN: if (ic >= 0) launch_icon(ic); break;
    case CX_REMOVE:
        if (ic >= NICONS && uicons[ic - NICONS].kind == UK_APP) unlink(uicons[ic - NICONS].path);
        break;
    case CX_SHOWFILES: spawn("files '" DESKTOP_DIR "'"); break;
    case CX_LOCK:                                /* ধাপ ১৯ছ */
    case CX_UNLOCK:
        if (ic >= NICONS) {
            snprintf(cmd, sizeof(cmd), "lockdlg %s '%s'%s", id == CX_LOCK ? "lock" : "unlock",
                     uicons[ic - NICONS].path, id == CX_LOCK ? "" : " -o");
            spawn(cmd);
        }
        break;
    case CX_HIDE:
        if (ic >= NICONS) rozhide_set(uicons[ic - NICONS].path, !uicons[ic - NICONS].hidden);
        break;
    case CX_SHOWHIDDEN: desk_show_hidden = !desk_show_hidden; break;
    case CX_ABOUT: spawn("about"); break;
    }
    if (load_user_icons()) damage_icons();
}

/* 1 = মেনুর ব্যাপার ছিল (ঘটনা শেষ) */
static int ctx_mouse(int type, int x, int y)
{
    int in, r = -1, i;
    if (!ctx_open) return 0;
    in = x >= ctx_x && x < ctx_x + CTX_W && y >= ctx_y && y < ctx_y + ctx_h;
    if (in)
        for (i = 0; i < ctx_n; i++)
            if (ctx_items[i].label[0] != '-' && y >= ctx_row_y(i) && y < ctx_row_y(i) + ITEM_H) r = i;
    if (r != ctx_hover) { ctx_hover = r; damage(ctx_x, ctx_y, CTX_W, ctx_h); }
    if (type == GFX_EV_MOUSE_DOWN && !in) { ctx_close(); return 0; }    /* বাইরে ক্লিক: বন্ধ, ক্লিক চলবে */
    if (type == GFX_EV_MOUSE_UP && in && r >= 0) {
        int id = ctx_items[r].id;
        ctx_close();
        ctx_do(id);
    }
    return in;
}

/* ---------- আঁকা ---------- */

static void draw_window(int idx)
{
    struct window *w = &wins[idx];
    int ox, oy, ow, oh, active = idx == focus, b;
    gfx_surface src;
    outer_rect(w, &ox, &oy, &ow, &oh);
    /* ফ্রেম: বাইরের 3D ধার + মুখ */
    ui_panel(S, ox, oy, ow, oh, T->face, 1);
    /* শিরোনাম-ফিতা */
    gfx_gradient_v(S, w->x, w->y - TITLE_H, w->w, TITLE_H - 2,
                   active ? T->title_a1 : T->title_i1, active ? T->title_a2 : T->title_i2);
    ui_icon(S, w->x + 4, w->y - TITLE_H + (TITLE_H - 2 - 16) / 2, 'p', T->text);
    {
        char t[64];
        int maxc = (w->w - 24 - 3 * 24 - 8) / 8;
        if (maxc < 0) maxc = 0;
        snprintf(t, sizeof(t), "%.*s", maxc > 63 ? 63 : maxc, w->title);
        gfx_text(S, NULL, w->x + 26, w->y - TITLE_H + (TITLE_H - 2 - 16) / 2, t, active ? GFX_WHITE : T->text_dim);
    }
    for (b = 0; b < 3; b++) {
        int bx, by, bw, bh, hover;
        title_button_rect(w, b, &bx, &by, &bw, &bh);
        hover = mx >= bx && mx < bx + bw && my >= by && my < by + bh;
        if (b == 0 && hover) {
            gfx_fill_rect(S, bx, by, bw, bh, T->close_hover);
            ui_bevel(S, bx, by, bw, bh, !(mbuttons & 1));
        } else {
            ui_button(S, bx, by, bw, bh, NULL, hover ? ((mbuttons & 1) ? UI_DOWN : UI_HOVER) : UI_NORMAL);
        }
        ui_icon(S, bx + (bw - 16) / 2, by + (bh - 16) / 2, b == 0 ? 'x' : b == 1 ? (w->maximized ? 'r' : 'o') : '_', T->text);
    }
    /* ভেতরের অংশ: বসানো ধার, তারপর অ্যাপের পিক্সেল */
    ui_bevel(S, w->x - 2, w->y - 2, w->w + 4, w->h + 4, 0);
    src.w = w->w; src.h = w->h; src.stride = W; src.px = w->px;
    src.clip_x = src.clip_y = 0; src.clip_w = w->w; src.clip_h = w->h; src.owns_pixels = 0;
    gfx_blit(S, w->x, w->y, &src, 0, 0, w->w, w->h);
    /* মাপ বদলানোর কোণ */
    if (!w->fixed && !w->maximized) {
        int k;
        for (k = 0; k < 3; k++) gfx_line(S, ox + ow - 3 - k * 4, oy + oh - 2, ox + ow - 2, oy + oh - 3 - k * 4, T->light);
    }
}

static void draw_panel(void)
{
    char clock[48];
    time_t t = time(NULL) + (time_t)utc_offset_min * 60;
    struct tm *tm = gmtime(&t);
    gfx_fill_rect(S, 0, 0, W, PANEL_H - 1, T->panel);
    gfx_hline(S, 0, PANEL_H - 1, W, T->line);
    gfx_text(S, NULL, 10, 5, "rozOS", T->accent);
    if (focus >= 0 && wins[focus].used) {
        int tw = gfx_text_width(NULL, wins[focus].title);
        gfx_text(S, NULL, (W - tw) / 2, 5, wins[focus].title, T->text);
    }
    strftime(clock, sizeof(clock), "%a %d %b  %H:%M", tm);
    gfx_text(S, NULL, W - 10 - gfx_text_width(NULL, clock), 5, clock, T->text);
}

/* ---------- ধাপ ২১: taskbar — Start | পিন করা ৩টা | খোলা উইন্ডো (শুধু আইকন) | tray ----------
 * পিন: Terminal, Files, Code — চালু না থাকলে ক্লিকে চালু, চালু থাকলে সামনে/ছোট (একাধিক হলে
 * পালা করে); ডান-ক্লিকে নতুন একটা। নিচে রঙিন দাগ = চালু আছে। নাম দেখা যায় মাউস রাখলে
 * (tooltip)। tray: নেটওয়ার্ক (IP থাকলে উজ্জ্বল, ক্লিকে Settings > Network), শব্দ (ড্রাইভার নেই) */
struct pin { const char *label, *image; char kind; const char *cmd; const char *match; };
static const struct pin pins[] = {
    { "Terminal", "terminal", 't', NULL,    "Terminal" },
    { "Files",    "files",    'f', "files", "Files" },
    { "Code",     "code",     'e', "code",  "Code" },
};
#define NPINS   ((int)(sizeof(pins) / sizeof(pins[0])))
#define PIN_X   108
#define PIN_W   38
#define WIN_X   (PIN_X + NPINS * PIN_W + 12)
#define WBTN_W  40
#define TRAY_W  72
static char tip[96];                    /* এখন দেখানো tooltip ("" = নেই) */
static int tip_x, tip_w;
static int net_state = -1;              /* 0 = কার্ড নেই, 1 = ঠিকানা নেই, 2 = চালু */
static char net_ip[20];

/* শিরোনাম "NAME" বা "... - NAME" */
static int title_is(const char *t, const char *app)
{
    size_t a = strlen(t), b = strlen(app);
    return !strcmp(t, app) || (a > b + 3 && !strcmp(t + a - b, app) && !strncmp(t + a - b - 3, " - ", 3));
}

static int win_pin(int i)
{
    int p;
    for (p = 0; p < NPINS; p++) if (title_is(wins[i].title, pins[p].match)) return p;
    return -1;
}

/* উইন্ডোর আইকন শিরোনাম দেখে (চেনা না গেলে সাধারণ প্রোগ্রাম-চিহ্ন) */
static void win_icon(const char *t, const char **img, char *kind)
{
    static const struct { const char *m, *img; char k; } map[] = {
        { "Paint", "paint", 'g' }, { "Calculator", "calculator", 'g' }, { "Images", "images", 'g' },
        { "Help", "help", 'e' }, { "Settings", "settings", 's' }, { "Terminal", "terminal", 't' },
        { "Files", "files", 'f' }, { "Code", "code", 'e' }, { "Lock folder", "locked-folder", 'l' },
        { "Unlock folder", "locked-folder", 'l' }, { "About rozOS", "about", 's' }
    };
    size_t i;
    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (title_is(t, map[i].m) || strstr(t, map[i].m)) { *img = map[i].img; *kind = map[i].k; return; }
    *img = "program";
    *kind = 'p';
}

static void pin_rect(int p, int *x, int *y, int *w, int *h)
{
    *x = PIN_X + p * PIN_W;
    *y = H - TASK_H + 4;
    *w = PIN_W - 4;
    *h = TASK_H - 7;
}

/* k-তম (পিনের বাইরের) উইন্ডোর বোতাম; জায়গা না থাকলে 0 */
static int taskbar_button_rect(int k, int n, int *x, int *y, int *w, int *h)
{
    (void)n;
    *x = WIN_X + k * WBTN_W;
    *y = H - TASK_H + 4;
    *w = WBTN_W - 4;
    *h = TASK_H - 7;
    return *x + *w <= W - TRAY_W - 6;
}

static int task_count(void)
{
    int i, n = 0;
    for (i = 0; i < MAXWIN; i++) if (wins[i].used && win_pin(i) < 0) n++;
    return n;
}

static void update_net(void)
{
    struct rozos_netinfo ni;
    int st = 0;
    char ip[20] = "";
    if (rozos_netinfo(0, &ni) == 0) {
        st = ni.configured && ni.ip ? 2 : 1;
        if (st == 2)
            snprintf(ip, sizeof(ip), "%u.%u.%u.%u", ni.ip & 255, (ni.ip >> 8) & 255, (ni.ip >> 16) & 255, ni.ip >> 24);
    }
    if (st != net_state || strcmp(ip, net_ip)) {
        net_state = st;
        snprintf(net_ip, sizeof(net_ip), "%s", ip);
        damage(W - TRAY_W, H - TASK_H, TRAY_W, TASK_H);
    }
}

static void draw_tray(void)
{
    int x = W - TRAY_W + 6, y = H - TASK_H + 7;
    uint32_t c = net_state == 2 ? T->text : T->text_dim;
    gfx_vline(S, W - TRAY_W, H - TASK_H + 5, TASK_H - 10, T->dark);
    /* নেটওয়ার্ক: তিনটা বাড়তে থাকা দাগ; না থাকলে লাল কাটা */
    gfx_fill_rect(S, x + 2, y + 10, 3, 5, c);
    gfx_fill_rect(S, x + 7, y + 6, 3, 9, c);
    gfx_fill_rect(S, x + 12, y + 2, 3, 13, c);
    if (net_state < 2) gfx_line(S, x, y + 15, x + 16, y, GFX_RGB(220, 80, 70));
    /* শব্দ: স্পিকার; ড্রাইভার নেই বলে x */
    x += 32;
    gfx_fill_rect(S, x + 1, y + 5, 4, 6, T->text_dim);
    gfx_fill_triangle(S, x + 4, y + 8, x + 10, y + 2, x + 10, y + 14, T->text_dim);
    gfx_line(S, x + 12, y + 5, x + 17, y + 11, GFX_RGB(220, 80, 70));
    gfx_line(S, x + 17, y + 5, x + 12, y + 11, GFX_RGB(220, 80, 70));
}

static void draw_taskbar(void)
{
    int i, k = 0, n = task_count(), p;
    int start_hover = mx < 100 && my >= H - TASK_H;
    gfx_fill_rect(S, 0, H - TASK_H, W, TASK_H, T->taskbar);
    gfx_hline(S, 0, H - TASK_H, W, T->line);
    ui_button(S, 4, H - TASK_H + 4, 96, TASK_H - 7, NULL,
              menu_open ? UI_DOWN : start_hover ? UI_HOVER : UI_NORMAL);
    gfx_fill_circle(S, 18, H - TASK_H / 2 + 1, 6, T->accent);
    gfx_fill_circle(S, 18, H - TASK_H / 2 + 1, 3, T->taskbar);
    gfx_text(S, NULL, 30, H - TASK_H + 8, "Start", T->text);
    /* পিন */
    for (p = 0; p < NPINS; p++) {
        int x, y, w, h, running = 0, active = 0, hov;
        pin_rect(p, &x, &y, &w, &h);
        for (i = 0; i < MAXWIN; i++)
            if (wins[i].used && win_pin(i) == p) { running = 1; if (i == focus && !wins[i].minimized) active = 1; }
        hov = mx >= x && mx < x + w && my >= y && my < y + h;
        ui_button(S, x, y, w, h, NULL, active ? UI_DOWN : hov ? UI_HOVER : UI_NORMAL);
        ui_draw_icon(S, x + (w - 18) / 2, y + (h - 18) / 2 - 1, 18, pins[p].image, pins[p].kind);
        if (running) gfx_fill_rect(S, x + 8, y + h - 3, w - 16, 2, T->accent);
    }
    gfx_vline(S, WIN_X - 7, H - TASK_H + 6, TASK_H - 12, T->dark);
    gfx_vline(S, WIN_X - 6, H - TASK_H + 6, TASK_H - 12, T->light2);
    /* বাকি খোলা উইন্ডো — শুধু আইকন */
    for (i = 0; i < MAXWIN; i++) {
        int x, y, w, h, hov;
        const char *img;
        char kind;
        if (!wins[i].used || win_pin(i) >= 0) continue;
        if (!taskbar_button_rect(k++, n, &x, &y, &w, &h)) break;
        hov = mx >= x && mx < x + w && my >= y && my < y + h;
        ui_button(S, x, y, w, h, NULL, (i == focus && !wins[i].minimized) ? UI_DOWN : hov ? UI_HOVER : UI_NORMAL);
        win_icon(wins[i].title, &img, &kind);
        ui_draw_icon(S, x + (w - 18) / 2, y + (h - 18) / 2 - 1, 18, img, kind);
        if (wins[i].minimized) gfx_fill_rect(S, x + 10, y + h - 3, w - 20, 2, T->text_dim);
    }
    draw_tray();
}

/* tooltip: taskbar-এর ঠিক ওপরে, মাউস যার ওপরে তার নাম */
static void draw_tooltip(void)
{
    int y = H - TASK_H - 26;
    if (!tip[0]) return;
    ui_panel(S, tip_x, y, tip_w, 22, T->face, 1);
    gfx_text(S, NULL, tip_x + 6, y + 3, tip, T->text);
}

static void set_tip(const char *t, int cx)
{
    int w = t[0] ? gfx_text_width(NULL, t) + 12 : 0, x = cx - w / 2;
    if (x + w > W - 2) x = W - 2 - w;
    if (x < 2) x = 2;
    if (!strcmp(t, tip) && (!t[0] || x == tip_x)) return;
    if (tip[0]) damage(tip_x, H - TASK_H - 26, tip_w, 22);
    snprintf(tip, sizeof(tip), "%s", t);
    tip_x = x;
    tip_w = w;
    if (tip[0]) damage(tip_x, H - TASK_H - 26, tip_w, 22);
}

/* মাউস taskbar-এ: কোন জিনিসের ওপরে — tooltip ঠিক করা */
static void taskbar_hover(void)
{
    int i, k = 0, n = task_count(), p;
    char t[96];
    if (my < H - TASK_H || menu_open) { set_tip("", 0); return; }
    for (p = 0; p < NPINS; p++) {
        int x, y, w, h;
        pin_rect(p, &x, &y, &w, &h);
        if (mx >= x && mx < x + w) { set_tip(pins[p].label, x + w / 2); return; }
    }
    for (i = 0; i < MAXWIN; i++) {
        int x, y, w, h;
        if (!wins[i].used || win_pin(i) >= 0) continue;
        if (!taskbar_button_rect(k++, n, &x, &y, &w, &h)) break;
        if (mx >= x && mx < x + w) { set_tip(wins[i].title, x + w / 2); return; }
    }
    if (mx >= W - TRAY_W && mx < W - TRAY_W + 32) {
        if (net_state == 2) snprintf(t, sizeof(t), "Network: connected, %s", net_ip);
        else snprintf(t, sizeof(t), "%s", net_state == 1 ? "Network: no address yet" : "Network: no supported network card");
        set_tip(t, mx);
        return;
    }
    if (mx >= W - TRAY_W + 32) { set_tip("Sound: no sound driver yet", mx); return; }
    set_tip("", 0);
}

/* মাউস-তীর (12x19) — সবশেষে আঁকা */
static const char *const arrow[] = {
    "#           ", "##          ", "#.#         ", "#..#        ", "#...#       ",
    "#....#      ", "#.....#     ", "#......#    ", "#.......#   ", "#........#  ",
    "#.........# ", "#..........#", "#......#####", "#...#..#    ", "#..# #..#   ",
    "#.#  #..#   ", "##    #..#  ", "      #..#  ", "       ##   "
};

static void draw_cursor(void)
{
    int i, j;
    for (j = 0; j < 19; j++)
        for (i = 0; i < 12; i++) {
            char c = arrow[j][i];
            if (c == '#') gfx_pixel(S, mx + i, my + j, GFX_BLACK);
            else if (c == '.') gfx_pixel(S, mx + i, my + j, GFX_WHITE);
        }
}

static void render_rect(int x, int y, int w, int h)
{
    int i;
    gfx_set_clip(S, x, y, w, h);
    if (wallpaper) gfx_blit(S, 0, 0, wallpaper, 0, 0, -1, -1);
    else gfx_fill_rect(S, x, y, w, h, T->desktop);
    draw_icons();
    for (i = 0; i < nz; i++) {
        struct window *wn = &wins[zorder[i]];
        int ox, oy, ow, oh;
        if (wn->minimized) continue;
        outer_rect(wn, &ox, &oy, &ow, &oh);
        if (ox >= x + w || oy >= y + h || ox + ow <= x || oy + oh <= y) continue;
        draw_window(zorder[i]);
    }
    if (y < PANEL_H) draw_panel();
    if (y + h > H - TASK_H) draw_taskbar();
    draw_menu();
    draw_ctx();                                  /* ধাপ ১৯চ */
    draw_tooltip();                              /* ধাপ ২১ */
    draw_cursor();
    gfx_reset_clip(S);
    gfx_present_rect(x, y, w, h);
}

static void render(void)
{
    int i;
    if (dmg_full) {
        render_rect(0, 0, W, H);
    } else {
        for (i = 0; i < ndmg; i++) render_rect(dmg[i].x, dmg[i].y, dmg[i].w, dmg[i].h);
    }
    ndmg = 0;
    dmg_full = 0;
}

/* ---------- মাউস ---------- */

enum { DRAG_NONE, DRAG_MOVE, DRAG_RESIZE, DRAG_CONTENT, DRAG_TBUTTON };
static int drag, drag_win = -1, drag_dx, drag_dy, drag_btn;
static long last_click_ms;
static int last_click_x, last_click_y;

static int window_at(int x, int y)
{
    int i;
    for (i = nz - 1; i >= 0; i--) {
        struct window *w = &wins[zorder[i]];
        int ox, oy, ow, oh;
        if (w->minimized) continue;
        outer_rect(w, &ox, &oy, &ow, &oh);
        if (x >= ox && x < ox + ow && y >= oy && y < oy + oh) return zorder[i];
    }
    return -1;
}

static void send_mouse(int idx, int type, int button_or_wheel)
{
    struct window *w = &wins[idx];
    send_msg(w->cl, WM_MOUSE, (uint32_t)w->id, mx - w->x, my - w->y, mbuttons, button_or_wheel,
             (uint32_t)type, NULL);
}

static void request_close(int idx)
{
    send_msg(wins[idx].cl, WM_CLOSE, (uint32_t)wins[idx].id, 0, 0, 0, 0, 0, NULL);
    if (!wins[idx].close_at) wins[idx].close_at = now_ms() + 3000;
}

static void toggle_max(int idx)
{
    struct window *w = &wins[idx];
    damage_window(w);
    if (w->fixed) return;
    if (w->maximized) {
        w->x = w->sx; w->y = w->sy; w->w = w->sw; w->h = w->sh;
        w->maximized = 0;
    } else {
        w->sx = w->x; w->sy = w->y; w->sw = w->w; w->sh = w->h;
        w->x = BORDER;
        w->y = work_top() + TITLE_H + BORDER;
        w->w = W - 2 * BORDER;
        w->h = work_bottom() - w->y - BORDER;
        w->maximized = 1;
    }
    damage_window(w);
    send_msg(w->cl, WM_RESIZE, (uint32_t)w->id, w->w, w->h, 0, 0, 0, NULL);
}

static void minimize(int idx)
{
    damage_window(&wins[idx]);
    wins[idx].minimized = 1;
    if (focus == idx) {
        int i, best = -1;
        for (i = 0; i < nz; i++) if (zorder[i] != idx && !wins[zorder[i]].minimized) best = zorder[i];
        if (best >= 0) set_focus(best);
        else {
            send_msg(wins[idx].cl, WM_FOCUS, (uint32_t)wins[idx].id, 0, 0, 0, 0, 0, NULL);
            focus = -1;
        }
    }
    damage(0, 0, W, PANEL_H);
    damage(0, H - TASK_H, W, TASK_H);
}

static void pin_launch(int p)
{
    if (!pins[p].cmd) spawn_terminal(NULL, 0);
    else spawn(pins[p].cmd);
}

/* ধাপ ২১: পিনে ক্লিক — না চললে চালু; একটাই সামনে থাকলে ছোট; নইলে পরেরটা সামনে */
static void pin_click(int p, int right)
{
    int i, list[MAXWIN], n = 0, cur = -1;
    if (right) { pin_launch(p); return; }
    for (i = 0; i < MAXWIN; i++)
        if (wins[i].used && win_pin(i) == p) {
            if (i == focus && !wins[i].minimized) cur = n;
            list[n++] = i;
        }
    if (!n) { pin_launch(p); return; }
    if (cur >= 0 && n == 1) { minimize(list[0]); return; }
    i = list[cur >= 0 ? (cur + 1) % n : 0];
    wins[i].minimized = 0;
    set_focus(i);
}

static void taskbar_click(int x, int y, int button)
{
    int i, k = 0, n = task_count(), p;
    if (x < 100) {
        open_menu(!menu_open);
        return;
    }
    for (p = 0; p < NPINS; p++) {
        int bx, by, bw, bh;
        pin_rect(p, &bx, &by, &bw, &bh);
        if (x >= bx && x < bx + bw && y >= by && y < by + bh) {
            pin_click(p, button == GFX_BUTTON_RIGHT);
            damage(0, H - TASK_H, W, TASK_H);
            return;
        }
    }
    if (x >= W - TRAY_W) {                       /* tray: নেটওয়ার্ক → Settings */
        if (x < W - TRAY_W + 32) spawn("settings network");
        return;
    }
    for (i = 0; i < MAXWIN; i++) {
        int bx, by, bw, bh;
        if (!wins[i].used || win_pin(i) >= 0) continue;
        if (!taskbar_button_rect(k++, n, &bx, &by, &bw, &bh)) break;
        if (x >= bx && x < bx + bw && y >= by && y < by + bh) {
            if (i == focus && !wins[i].minimized) minimize(i);
            else {
                wins[i].minimized = 0;
                set_focus(i);
            }
            return;
        }
    }
}

static void mouse_event(const gfx_event *ev)
{
    int oldx = mx, oldy = my, idx;
    if (ev->type == GFX_EV_MOUSE_MOVE || ev->type == GFX_EV_MOUSE_DOWN || ev->type == GFX_EV_MOUSE_UP) {
        mx = ev->x; my = ev->y;
    }
    mbuttons = ev->buttons;
    if (mx != oldx || my != oldy) {
        damage(oldx, oldy, 12, 19);
        damage(mx, my, 12, 19);
        /* শিরোনামের বোতাম আর taskbar-এ হোভার রং */
        if (oldy >= H - TASK_H || my >= H - TASK_H) damage(0, H - TASK_H, W, TASK_H);   /* ধাপ ২১: পিন/উইন্ডোর হোভার */
        taskbar_hover();
        idx = window_at(mx, my);
        if (idx >= 0) damage(wins[idx].x + wins[idx].w - 80, wins[idx].y - TITLE_H, 80, TITLE_H);
        idx = window_at(oldx, oldy);
        if (idx >= 0) damage(wins[idx].x + wins[idx].w - 80, wins[idx].y - TITLE_H, 80, TITLE_H);
    }

    /* টেনে চলছে */
    if (drag != DRAG_NONE && drag_win >= 0 && wins[drag_win].used) {
        struct window *w = &wins[drag_win];
        if (ev->type == GFX_EV_MOUSE_MOVE) {
            if (drag == DRAG_MOVE) {
                damage_window(w);
                w->x = mx - drag_dx;
                w->y = my - drag_dy;
                clamp_window(w);
                damage_window(w);
            } else if (drag == DRAG_RESIZE) {
                int nw = mx - w->x - drag_dx, nh = my - w->y - drag_dy;
                if (nw != w->w || nh != w->h) {
                    damage_window(w);
                    w->w = nw;
                    w->h = nh;
                    clamp_window(w);
                    damage_window(w);
                    send_msg(w->cl, WM_RESIZE, (uint32_t)w->id, w->w, w->h, 0, 0, 0, NULL);
                }
            } else if (drag == DRAG_CONTENT) {
                send_mouse(drag_win, GFX_EV_MOUSE_MOVE, 0);
            }
            return;
        }
        if (ev->type == GFX_EV_MOUSE_UP) {
            if (drag == DRAG_CONTENT) send_mouse(drag_win, GFX_EV_MOUSE_UP, ev->button);
            if (drag == DRAG_TBUTTON) {
                int bx, by, bw, bh;
                title_button_rect(w, drag_btn, &bx, &by, &bw, &bh);
                if (mx >= bx && mx < bx + bw && my >= by && my < by + bh) {
                    if (drag_btn == 0) request_close(drag_win);
                    else if (drag_btn == 1) toggle_max(drag_win);
                    else minimize(drag_win);
                }
                damage_window(w);
            }
            if (!mbuttons) { drag = DRAG_NONE; drag_win = -1; }
            return;
        }
    }

    if (menu_mouse(ev->type, mx, my)) return;
    if (ctx_mouse(ev->type, mx, my)) return;     /* ধাপ ১৯চ */

    if (ev->type == GFX_EV_MOUSE_DOWN) {
        int dbl = now_ms() - last_click_ms < 400 && abs(mx - last_click_x) < 6 && abs(my - last_click_y) < 6;
        last_click_ms = now_ms();
        last_click_x = mx; last_click_y = my;
        if (my >= H - TASK_H) {
            taskbar_click(mx, my, ev->button);
            return;
        }
        if (my < PANEL_H) return;
        idx = window_at(mx, my);
        if (idx < 0) {
            int ic = icon_at(mx, my);
            if (ev->button == GFX_BUTTON_RIGHT) {        /* ধাপ ১৯চ: ডান-ক্লিক মেনু */
                if (ic != icon_sel) {
                    int x, y, w, h;
                    if (icon_sel >= 0) { icon_rect(icon_sel, &x, &y, &w, &h); damage(x, y, w, h); }
                    icon_sel = ic;
                    if (ic >= 0) { icon_rect(ic, &x, &y, &w, &h); damage(x, y, w, h); }
                }
                ctx_show(mx, my, ic);
                return;
            }
            if (ic != icon_sel) {
                int x, y, w, h;
                if (icon_sel >= 0) { icon_rect(icon_sel, &x, &y, &w, &h); damage(x, y, w, h); }
                icon_sel = ic;
                if (ic >= 0) { icon_rect(ic, &x, &y, &w, &h); damage(x, y, w, h); }
            }
            if (ic >= 0 && dbl) launch_icon(ic);
            return;
        }
        if (idx != focus) set_focus(idx);
        {
            struct window *w = &wins[idx];
            int b, ox, oy, ow, oh;
            outer_rect(w, &ox, &oy, &ow, &oh);
            for (b = 0; b < 3; b++) {
                int bx, by, bw, bh;
                title_button_rect(w, b, &bx, &by, &bw, &bh);
                if (mx >= bx && mx < bx + bw && my >= by && my < by + bh) {
                    drag = DRAG_TBUTTON; drag_win = idx; drag_btn = b;
                    damage_window(w);
                    return;
                }
            }
            if (!w->fixed && !w->maximized && mx >= ox + ow - 14 && my >= oy + oh - 14) {
                drag = DRAG_RESIZE; drag_win = idx;
                drag_dx = mx - w->x - w->w;
                drag_dy = my - w->y - w->h;
                return;
            }
            if (my < w->y && my >= w->y - TITLE_H) {
                if (dbl) { toggle_max(idx); return; }
                if (!w->maximized) { drag = DRAG_MOVE; drag_win = idx; drag_dx = mx - w->x; drag_dy = my - w->y; }
                return;
            }
            if (mx >= w->x && mx < w->x + w->w && my >= w->y && my < w->y + w->h) {
                drag = DRAG_CONTENT; drag_win = idx;
                send_mouse(idx, GFX_EV_MOUSE_DOWN, ev->button);
            }
        }
        return;
    }
    if (ev->type == GFX_EV_MOUSE_MOVE || ev->type == GFX_EV_MOUSE_WHEEL) {
        idx = window_at(mx, my);
        if (idx >= 0 && idx == focus) {
            struct window *w = &wins[idx];
            if (mx >= w->x && mx < w->x + w->w && my >= w->y && my < w->y + w->h)
                send_mouse(idx, ev->type, ev->type == GFX_EV_MOUSE_WHEEL ? ev->wheel : 0);
        }
    }
}

/* ---------- শুরু, লুপ, শেষ ---------- */

/* ধাপ ১৯খ: ডেস্কটপ আসার আগে বুট ছবি (splash) — /etc/rozos.conf-এর splash= (খালি = বন্ধ)।
 * ছবি অনুপাত ঠিক রেখে পর্দায় বসে, চারপাশ কালো।
 * ধাপ ১৯খ-২: কালো পর্দায় ছবি ধীরে ধীরে ফুটে ওঠে — splash_fade_ms= (ডিফল্ট 2500; 0 = সাথে সাথে)।
 * splash_ms= ছবি শুরু থেকে কমপক্ষে কতক্ষণ থাকবে (ডিফল্ট 0 = ফেড শেষ হলেই ডেস্কটপ)। */
static long splash_until;

/* src-এর উজ্জ্বলতা a/256 করে S-এর (x, y)-তে (R আর B একসাথে, G আলাদা গুণ) */
static void splash_frame(const gfx_surface *src, int x, int y, unsigned a)
{
    int r, c;
    for (r = 0; r < src->h; r++) {
        const uint32_t *in = src->px + (long)r * src->stride;
        uint32_t *out = S->px + (long)(y + r) * S->stride + x;
        for (c = 0; c < src->w; c++) {
            uint32_t p = in[c];
            out[c] = ((((p & 0xff00ffu) * a) >> 8) & 0xff00ffu) | ((((p & 0x00ff00u) * a) >> 8) & 0x00ff00u);
        }
    }
    gfx_present_rect(x, y, src->w, src->h);
}

static void show_splash(void)
{
    FILE *f = fopen("/etc/rozos.conf", "r");
    char line[256], path[200] = "/usr/share/images/rozos-boot.jpg";
    int ms = 0, fade = 2500;
    gfx_surface *img, *pic;
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            char *v = strchr(line, '=');
            if (line[0] == '#' || !v) continue;
            *v++ = '\0';
            v[strcspn(v, "\r\n")] = '\0';
            if (!strcmp(line, "splash")) snprintf(path, sizeof(path), "%s", v);
            else if (!strcmp(line, "splash_ms")) ms = atoi(v);
            else if (!strcmp(line, "splash_fade_ms")) fade = atoi(v);
        }
        fclose(f);
    }
    if (!path[0] || !(img = gfx_load_image(path))) return;
    {
        int dw = W, dh = (int)((long)img->h * W / img->w), x, y;
        long t0, el;
        if (dh > H) { dh = H; dw = (int)((long)img->w * H / img->h); }
        x = (W - dw) / 2;
        y = (H - dh) / 2;
        gfx_fill_rect(S, 0, 0, W, H, 0x000000);
        gfx_present();                                /* আগে পুরো কালো পর্দা */
        pic = gfx_surface_new(dw, dh);                /* একবারই পর্দার মাপে */
        if (pic) gfx_blit_scaled(pic, 0, 0, dw, dh, img);
        gfx_surface_free(img);
        if (!pic) return;
        t0 = gfx_ticks_ms();
        if (fade > 0) {
            while ((el = gfx_ticks_ms() - t0) < fade) {
                double k = (double)el / fade;
                k = k * k * (3.0 - 2.0 * k);          /* ধীরে শুরু, ধীরে শেষ */
                splash_frame(pic, x, y, (unsigned)(k * 256.0));
                gfx_sleep_ms(5);
            }
        }
        splash_frame(pic, x, y, 256);                 /* শেষে পুরো উজ্জ্বল */
        gfx_surface_free(pic);
        splash_until = t0 + ms;
    }
}

static void splash_wait(void)
{
    long left = splash_until - gfx_ticks_ms();
    if (splash_until && left > 0) gfx_sleep_ms((int)left);
}

/* ধাপ ১৯চ: wallpaper_style= stretch (পর্দা জুড়ে টেনে, আগের মতো) বা fill (অনুপাত ঠিক রেখে ভরানো,
 * কিনারা কাটা); conf বদলালে (Settings) প্রতি সেকেন্ডের পরীক্ষায় আবার পড়া হয় */
static time_t conf_mtime;
static long conf_size = -1;

static void set_wallpaper(const char *path, int fill)
{
    gfx_surface *img;
    gfx_surface_free(wallpaper);
    wallpaper = NULL;
    if (!path[0] || !(img = gfx_load_image(path))) return;
    wallpaper = gfx_surface_new(W, H);                 /* একবারই পর্দার মাপে বড়-ছোট করা */
    if (wallpaper) {
        if (fill && img->w > 0 && img->h > 0) {
            int dw = W, dh = (int)((long)img->h * W / img->w);
            if (dh < H) { dh = H; dw = (int)((long)img->w * H / img->h); }
            gfx_blit_scaled(wallpaper, (W - dw) / 2, (H - dh) / 2, dw, dh, img);
        } else {
            gfx_blit_scaled(wallpaper, 0, 0, W, H, img);
        }
    }
    gfx_surface_free(img);
}

static int conf_changed(void)
{
    struct stat st;
    if (stat("/etc/rozos.conf", &st) < 0) return 0;
    if (st.st_mtime == conf_mtime && (long)st.st_size == conf_size) return 0;
    conf_mtime = st.st_mtime;
    conf_size = (long)st.st_size;
    return 1;
}

static void load_config(void)
{
    FILE *f = fopen("/etc/rozos.conf", "r");
    char line[256], wp[200] = "";
    int fill = 0;
    conf_changed();
    if (!f) { set_wallpaper("", 0); return; }
    while (fgets(line, sizeof(line), f)) {
        char *v = strchr(line, '=');
        if (line[0] == '#' || !v) continue;
        *v++ = '\0';
        v[strcspn(v, "\r\n")] = '\0';
        if (!strcmp(line, "utc_offset")) {
            int sign = 1, hh = 0, mm = 0;
            if (*v == '-') { sign = -1; v++; } else if (*v == '+') v++;
            sscanf(v, "%d:%d", &hh, &mm);
            utc_offset_min = sign * (hh * 60 + mm);
        } else if (!strcmp(line, "wallpaper")) {
            snprintf(wp, sizeof(wp), "%s", v);
        } else if (!strcmp(line, "wallpaper_style")) {
            fill = !strcmp(v, "fill");
        }
    }
    fclose(f);
    set_wallpaper(wp, fill);
}

static void accept_client(void)
{
    int fd = accept(listen_fd, NULL, NULL), i;
    if (fd < 0) return;
    for (i = 0; i < MAXCLI; i++) {
        if (!clients[i].used) {
            memset(&clients[i], 0, sizeof(clients[i]));
            clients[i].used = 1;
            clients[i].fd = fd;
            return;
        }
    }
    close(fd);
}

static void shutdown_clients(void)
{
    int i;
    for (i = 0; i < MAXCLI; i++) {
        if (clients[i].used && clients[i].pid > 0) kill(-clients[i].pid, SIGTERM);
    }
    for (i = 0; i < MAXCLI; i++) if (clients[i].used) drop_client(&clients[i]);
}

int main(int argc, char **argv)
{
    struct sockaddr_un a;
    struct termios tio;
    long last_clock = 0;
    int start_term = 0, i0;

    for (i0 = 1; i0 < argc; i0++) {
        if (!strcmp(argv[i0], "-t")) start_term = 1;
        else if (!strcmp(argv[i0], "-k")) gfx_input_keyboard_only(1);   /* init: serial আলাদা */
    }
    setenv("ROZWM_SERVER", "1", 1);
    if (gfx_init_fullscreen() < 0) {
        perror("rozwm: /dev/fb0");
        return 1;
    }
    gfx_raw_keys(1);          /* ধাপ ১৮ক: কাঁচা কী (চাপা/ছাড়া) → সামনের উইন্ডোতে WM_RAWKEY */
    S = gfx_screen();
    W = gfx_width();
    H = gfx_height();
    T = ui_theme_get();
    /* কনসোলের Ctrl+C আমাদের জন্য সাধারণ কী (সামনের টার্মিনালে যাবে) */
    if (tcgetattr(0, &tio) == 0) {
        tio.c_lflag &= ~ISIG;
        tcsetattr(0, TCSANOW, &tio);
    }
    signal(SIGINT, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    show_splash();
    load_config();
    load_user_icons();                           /* ধাপ ১৯চ: /home/Desktop */

    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path, ROZWM_SOCKET);
    if (listen_fd < 0 || bind(listen_fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(listen_fd, 16) < 0) {
        gfx_quit();
        fprintf(stderr, "rozwm: %s: %s (is rozwm already running?)\n", ROZWM_SOCKET, strerror(errno));
        return 1;
    }
    splash_wait();
    mx = W / 2;
    my = H / 2;
    damage_all();
    render();
    if (start_term) spawn_terminal(NULL, 0);

    while (running) {
        struct pollfd p[MAXCLI + 4];
        int np = 0, i, k, evfds[4], nev;
        int cli_index[MAXCLI + 4];
        long t = now_ms();
        gfx_event ev;

        nev = gfx_event_fds(evfds, 4);
        for (i = 0; i < nev; i++) { p[np].fd = evfds[i]; p[np].events = POLLIN; p[np].revents = 0; cli_index[np++] = -1; }
        p[np].fd = listen_fd; p[np].events = POLLIN; p[np].revents = 0; cli_index[np++] = -2;
        for (i = 0; i < MAXCLI; i++) {
            if (!clients[i].used) continue;
            p[np].fd = clients[i].fd; p[np].events = POLLIN; p[np].revents = 0; cli_index[np++] = i;
        }
        poll(p, (unsigned long)np, ndmg || dmg_full ? 0 : 250);

        for (k = 0; k < np; k++) {
            if (!(p[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            if (cli_index[k] == -2) accept_client();
            else if (cli_index[k] >= 0 && clients[cli_index[k]].used) read_client(&clients[cli_index[k]]);
        }
        while (gfx_poll_event(&ev, 0)) {
            if (ev.type == GFX_EV_KEY) {
                if (menu_open && ev.key == GFX_KEY_ESC) open_menu(0);
                else if (focus >= 0 && wins[focus].used)
                    send_msg(wins[focus].cl, WM_KEY, (uint32_t)wins[focus].id, ev.key, ev.mods, 0, 0, 0, NULL);
            } else if (ev.type == GFX_EV_KEY_DOWN || ev.type == GFX_EV_KEY_UP) {
                /* ধাপ ১৮ক: কার্নেলের মতো করেই বাঁধা (বিট 16-18 Shift 1/Ctrl 2/Alt 4) */
                int32_t raw = ev.key | (ev.type == GFX_EV_KEY_UP ? 0x40000000 : 0) | (ev.repeat ? 0x20000000 : 0) |
                              (((ev.mods & GFX_MOD_SHIFT) ? 1 : 0) | ((ev.mods & GFX_MOD_CTRL) ? 2 : 0) |
                               ((ev.mods & GFX_MOD_ALT) ? 4 : 0)) << 16;
                if (!menu_open && focus >= 0 && wins[focus].used)
                    send_msg(wins[focus].cl, WM_RAWKEY, (uint32_t)wins[focus].id, raw, 0, 0, 0, 0, NULL);
            } else {
                mouse_event(&ev);
            }
        }
        /* [X]-এর পর সাড়া না দেওয়া অ্যাপ */
        for (i = 0; i < MAXWIN; i++) {
            if (wins[i].used && wins[i].close_at && t > wins[i].close_at) {
                struct client *c = wins[i].cl;
                if (c && c->pid > 0) kill(-c->pid, SIGKILL), kill(c->pid, SIGKILL);
                wins[i].close_at = 0;
                if (c) drop_client(c);
            }
        }
        while (waitpid(-1, NULL, WNOHANG) > 0) {
        }
        if (t - last_clock > 1000) {
            last_clock = t;
            damage(W - 220, 0, 220, PANEL_H);
            /* ধাপ ১৯চ: Settings-এ বদল (ওয়ালপেপার) আর /home/Desktop-এ নতুন/মোছা আইকন */
            if (conf_changed()) { load_config(); damage(0, 0, W, H); }
            if (load_user_icons()) damage_icons();
            update_net();                        /* ধাপ ২১: tray-এর নেটওয়ার্ক চিহ্ন */
        }
        render();
    }

    shutdown_clients();
    close(listen_fd);
    gfx_quit();
    if (exit_action == 'r') execl("/bin/reboot", "reboot", (char *)NULL);
    if (exit_action == 'p') execl("/bin/poweroff", "poweroff", (char *)NULL);
    return 0;
}
