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
#include <rozgfx.h>
#include <rozui.h>
#include <rozwm_proto.h>

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
enum { FIXED_TERM = 0, FIXED_TEXT, FIXED_RESTART, FIXED_OFF, NFIXED };
static const char *const fixed_names[NFIXED] = { "Terminal", "Text mode", "Restart", "Shut down" };
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

static void icon_rect(int i, int *x, int *y, int *w, int *h)
{
    int per = (H - TASK_H - PANEL_H - 12) / 72;       /* ধাপ ১৪খ: জায়গা না হলে পাশের কলামে */
    if (per < 1) per = 1;
    *x = 16 + (i / per) * 84;
    *y = PANEL_H + 12 + (i % per) * 72;
    *w = 72;
    *h = 66;
}

static void draw_icons(void)
{
    int i;
    for (i = 0; i < NICONS; i++) {
        int x, y, w, h, tw;
        icon_rect(i, &x, &y, &w, &h);
        if (i == icon_sel) gfx_fill_round_rect(S, x, y, w, h, 6, T->select);
        /* ধাপ ১৪খ: ছবি থাকলে ৩২x৩২ ছবি, নইলে 3D বাক্সে আঁকা চিহ্ন */
        ui_draw_icon(S, x + 20, y + 6, 32, icons[i].image, icons[i].kind);
        tw = gfx_text_width(NULL, icons[i].label);
        gfx_text(S, NULL, x + (w - tw) / 2, y + 44, icons[i].label, i == icon_sel ? GFX_WHITE : T->text);
    }
}

static int icon_at(int px, int py)
{
    int i;
    for (i = 0; i < NICONS; i++) {
        int x, y, w, h;
        icon_rect(i, &x, &y, &w, &h);
        if (px >= x && px < x + w && py >= y && py < y + h) return i;
    }
    return -1;
}

static void launch_icon(int i)
{
    if (icons[i].term) spawn_terminal(icons[i].cmd, 0);
    else spawn(icons[i].cmd);
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

static void taskbar_button_rect(int k, int n, int *x, int *y, int *w, int *h)
{
    int avail = W - 110 - 10, bw = n ? avail / n : 0;
    if (bw > 180) bw = 180;
    *x = 108 + k * bw;
    *y = H - TASK_H + 4;
    *w = bw - 4;
    *h = TASK_H - 7;
}

static int task_count(void)
{
    int i, n = 0;
    for (i = 0; i < MAXWIN; i++) if (wins[i].used) n++;
    return n;
}

static void draw_taskbar(void)
{
    int i, k = 0, n = task_count();
    int start_hover = mx < 100 && my >= H - TASK_H;
    gfx_fill_rect(S, 0, H - TASK_H, W, TASK_H, T->taskbar);
    gfx_hline(S, 0, H - TASK_H, W, T->line);
    ui_button(S, 4, H - TASK_H + 4, 96, TASK_H - 7, NULL,
              menu_open ? UI_DOWN : start_hover ? UI_HOVER : UI_NORMAL);
    gfx_fill_circle(S, 18, H - TASK_H / 2 + 1, 6, T->accent);
    gfx_fill_circle(S, 18, H - TASK_H / 2 + 1, 3, T->taskbar);
    gfx_text(S, NULL, 30, H - TASK_H + 8, "Start", T->text);
    for (i = 0; i < MAXWIN; i++) {
        int x, y, w, h;
        char t[40];
        if (!wins[i].used) continue;
        taskbar_button_rect(k++, n, &x, &y, &w, &h);
        ui_button(S, x, y, w, h, NULL, (i == focus && !wins[i].minimized) ? UI_DOWN : UI_NORMAL);
        ui_icon(S, x + 4, y + 3, 'p', T->text);
        snprintf(t, sizeof(t), "%.*s", w > 40 ? (w - 30) / 8 : 0, wins[i].title);
        gfx_text(S, NULL, x + 24, y + 4, t, wins[i].minimized ? T->text_dim : T->text);
    }
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

static void taskbar_click(int x, int y)
{
    int i, k = 0, n = task_count();
    if (x < 100) {
        open_menu(!menu_open);
        return;
    }
    for (i = 0; i < MAXWIN; i++) {
        int bx, by, bw, bh;
        if (!wins[i].used) continue;
        taskbar_button_rect(k++, n, &bx, &by, &bw, &bh);
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
        if (oldy >= H - TASK_H || my >= H - TASK_H) damage(0, H - TASK_H, 110, TASK_H);
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

    if (ev->type == GFX_EV_MOUSE_DOWN) {
        int dbl = now_ms() - last_click_ms < 400 && abs(mx - last_click_x) < 6 && abs(my - last_click_y) < 6;
        last_click_ms = now_ms();
        last_click_x = mx; last_click_y = my;
        if (my >= H - TASK_H) {
            taskbar_click(mx, my);
            return;
        }
        if (my < PANEL_H) return;
        idx = window_at(mx, my);
        if (idx < 0) {
            int ic = icon_at(mx, my);
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

/* ধাপ ১৯খ: ডেস্কটপ আসার আগে বুট ছবি (splash) — /etc/rozos.conf-এর splash= (খালি = বন্ধ),
 * splash_ms= (কমপক্ষে কতক্ষণ দেখাবে)। ছবি অনুপাত ঠিক রেখে পর্দায় বসে, চারপাশ কালো। */
static long splash_until;

static void show_splash(void)
{
    FILE *f = fopen("/etc/rozos.conf", "r");
    char line[256], path[200] = "/usr/share/images/rozos-boot.jpg";
    int ms = 1500;
    gfx_surface *img;
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            char *v = strchr(line, '=');
            if (line[0] == '#' || !v) continue;
            *v++ = '\0';
            v[strcspn(v, "\r\n")] = '\0';
            if (!strcmp(line, "splash")) snprintf(path, sizeof(path), "%s", v);
            else if (!strcmp(line, "splash_ms")) ms = atoi(v);
        }
        fclose(f);
    }
    if (!path[0] || ms <= 0 || !(img = gfx_load_image(path))) return;
    {
        int dw = W, dh = (int)((long)img->h * W / img->w);
        if (dh > H) { dh = H; dw = (int)((long)img->w * H / img->h); }
        gfx_fill_rect(S, 0, 0, W, H, 0x000000);
        gfx_blit_scaled(S, (W - dw) / 2, (H - dh) / 2, dw, dh, img);
        gfx_present();
        gfx_surface_free(img);
    }
    splash_until = gfx_ticks_ms() + ms;
}

static void splash_wait(void)
{
    long left = splash_until - gfx_ticks_ms();
    if (splash_until && left > 0) gfx_sleep_ms((int)left);
}

static void load_config(void)
{
    FILE *f = fopen("/etc/rozos.conf", "r");
    char line[256];
    if (!f) return;
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
        } else if (!strcmp(line, "wallpaper") && v[0]) {
            gfx_surface *img = gfx_load_image(v);
            if (img) {                               /* একবারই পর্দার মাপে বড়-ছোট করা */
                wallpaper = gfx_surface_new(W, H);
                if (wallpaper) gfx_blit_scaled(wallpaper, 0, 0, W, H, img);
                gfx_surface_free(img);
            }
        }
    }
    fclose(f);
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
