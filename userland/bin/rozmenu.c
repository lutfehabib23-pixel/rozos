/* rozOS — rozmenu (shell-এ `menu`): প্রোগ্রাম খোলার মেনু (ধাপ ১২)
 *
 * /usr/share/apps-এর .app ফাইলগুলো পড়ে বিভাগ অনুযায়ী তালিকা দেখায়। প্রতিটি ফাইল:
 *     name: Calculator          মেনুতে নাম
 *     category: Tools           বিভাগ
 *     run: /usr/local/bin/calc  চালানোর কমান্ড (/bin/sh -c দিয়ে)
 *     about: a simple calculator
 *     wait: yes                 শেষ হলে "কী চাপুন" (কমান্ড-লাইনের প্রোগ্রামের output পড়তে)
 *     confirm: yes              চালানোর আগে জিজ্ঞেস (reboot ইত্যাদি)
 *
 * কী: তীর/PgUp/PgDn/Home/End বাছাই, Enter চালানো, / খোঁজা, Esc খোঁজা বাতিল, q বের হওয়া।
 * মাউস: ক্লিকে বাছাই, আবার ক্লিক (বা দুবার) চালানো, চাকায় স্ক্রল।
 * আলাদা পর্দায় খোলে (ESC[?1049h) — বের হলে আগের কমান্ড লাইন ফিরে আসে। */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#define APPS_DIR "/usr/share/apps"
#define MAX_APPS 256

struct app {
    char name[64], category[40], run[256], about[120];
    int wait, confirm;
};

static struct app apps[MAX_APPS];
static int napps;
static int shown[MAX_APPS + 64];           /* দেখানো সারি: >=0 app, <0 বিভাগের শিরোনাম (-1-index) */
static char headers[64][40];
static int nshown, nheaders;
static int sel, top, rows = 24, cols = 80;
static char filter[40];
static char msg[160];
static int mouse_b, mouse_x, mouse_y, mouse_release;

enum { K_UP = 1000, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_PGUP, K_PGDN, K_ESC, K_MOUSE };

static void trim(char *s)
{
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    if (s != s + strspn(s, " \t")) memmove(s, s + strspn(s, " \t"), strlen(s + strspn(s, " \t")) + 1);
}

static void load_apps(void)
{
    DIR *d = opendir(APPS_DIR);
    struct dirent *e;
    napps = 0;
    if (!d) return;
    while ((e = readdir(d)) && napps < MAX_APPS) {
        char path[300], line[300];
        size_t l = strlen(e->d_name);
        FILE *f;
        struct app *a = &apps[napps];
        if (l < 5 || strcmp(e->d_name + l - 4, ".app")) continue;
        snprintf(path, sizeof(path), APPS_DIR "/%s", e->d_name);
        if (!(f = fopen(path, "r"))) continue;
        memset(a, 0, sizeof(*a));
        snprintf(a->category, sizeof(a->category), "Other");
        while (fgets(line, sizeof(line), f)) {
            char *colon = strchr(line, ':');
            if (line[0] == '#' || !colon) continue;
            *colon = '\0';
            trim(line);
            trim(colon + 1);
            if (!strcmp(line, "name")) snprintf(a->name, sizeof(a->name), "%s", colon + 1);
            else if (!strcmp(line, "category")) snprintf(a->category, sizeof(a->category), "%s", colon + 1);
            else if (!strcmp(line, "run")) snprintf(a->run, sizeof(a->run), "%s", colon + 1);
            else if (!strcmp(line, "about")) snprintf(a->about, sizeof(a->about), "%s", colon + 1);
            else if (!strcmp(line, "wait")) a->wait = !strcmp(colon + 1, "yes");
            else if (!strcmp(line, "confirm")) a->confirm = !strcmp(colon + 1, "yes");
        }
        fclose(f);
        if (!a->name[0]) snprintf(a->name, sizeof(a->name), "%.*s", (int)(l - 4), e->d_name);
        if (a->run[0]) napps++;
    }
    closedir(d);
}

static int cmp_app(const void *x, const void *y)
{
    const struct app *a = (const struct app *)x, *b = (const struct app *)y;
    int c = strcasecmp(a->category, b->category);
    return c ? c : strcasecmp(a->name, b->name);
}

static int matches(const struct app *a)
{
    char hay[300], needle[40];
    size_t i;
    if (!filter[0]) return 1;
    snprintf(hay, sizeof(hay), "%s %s %s", a->name, a->category, a->about);
    snprintf(needle, sizeof(needle), "%s", filter);
    for (i = 0; hay[i]; i++) hay[i] = (char)tolower((unsigned char)hay[i]);
    for (i = 0; needle[i]; i++) needle[i] = (char)tolower((unsigned char)needle[i]);
    return strstr(hay, needle) != NULL;
}

static void build_list(void)
{
    int i;
    const char *last = "";
    nshown = nheaders = 0;
    for (i = 0; i < napps; i++) {
        if (!matches(&apps[i])) continue;
        if (strcmp(apps[i].category, last) && nheaders < 64) {
            snprintf(headers[nheaders], sizeof(headers[0]), "%s", apps[i].category);
            shown[nshown++] = -1 - nheaders;
            nheaders++;
            last = apps[i].category;
        }
        shown[nshown++] = i;
    }
    if (sel >= nshown || sel < 0 || (nshown && shown[sel] < 0)) {
        for (sel = 0; sel < nshown && shown[sel] < 0; sel++) {
        }
        if (sel >= nshown) sel = -1;
    }
}

static int list_rows(void) { return rows - 4; }

static void move_sel(int dir)
{
    int s = sel;
    if (sel < 0) return;
    for (;;) {
        s += dir;
        if (s < 0 || s >= nshown) return;
        if (shown[s] >= 0) { sel = s; return; }
    }
}

static void keep_visible(void)
{
    if (sel < 0) return;
    if (sel < top) top = sel - (sel > 0 && shown[sel - 1] < 0);
    if (sel >= top + list_rows()) top = sel - list_rows() + 1;
    if (top < 0) top = 0;
}

static void draw(void)
{
    char buf[512];
    int r, w = cols - 1;
    printf("\x1b[?25l\x1b[H\x1b[7m");
    snprintf(buf, sizeof(buf), "  rozOS menu   %d program%s%s%s", napps, napps == 1 ? "" : "s",
             filter[0] ? "   search: " : "", filter);
    printf("%-*.*s\x1b[0m\x1b[K\n", w, w, buf);
    for (r = 0; r < list_rows(); r++) {
        int i = top + r;
        printf("\x1b[%d;1H", r + 2);
        if (i < nshown) {
            if (shown[i] < 0) {
                printf("\x1b[97m %s\x1b[0m", headers[-1 - shown[i]]);
            } else {
                const struct app *a = &apps[shown[i]];
                int nw = 28, aw = w - nw - 6;
                if (aw < 0) aw = 0;
                printf("%s   %-*.*s %-*.*s\x1b[0m", i == sel ? "\x1b[30;46m" : "", nw, nw, a->name, aw, aw,
                       a->about);
            }
        } else if (i == 0) {
            printf("  (no programs%s — see: help menu)", filter[0] ? " match the search" : "");
        }
        printf("\x1b[K");
    }
    printf("\x1b[%d;1H\x1b[K", rows - 1);
    if (msg[0]) printf(" %.*s", w - 1, msg);
    else if (sel >= 0) printf(" \x1b[90m%.*s\x1b[0m", w - 1, apps[shown[sel]].run);
    printf("\x1b[%d;1H\x1b[7m%-*.*s\x1b[0m", rows, w, w,
           " Enter/click: run   arrows: choose   /: search   Esc: clear search   q: quit");
    fflush(stdout);
}

static int read_byte(void)
{
    unsigned char c;
    return read(0, &c, 1) == 1 ? c : -1;
}

static int read_key(void)
{
    int c = read_byte(), num = 0;
    if (c != 0x1B) return c;
    c = read_byte();
    if (c != '[' && c != 'O') return K_ESC;
    c = read_byte();
    if (c == '<') {
        int v[3] = { 0, 0, 0 }, i = 0;
        for (;;) {
            c = read_byte();
            if (c < 0) return K_ESC;
            if (isdigit(c)) { if (i < 3) v[i] = v[i] * 10 + (c - '0'); continue; }
            if (c == ';') { i++; continue; }
            break;
        }
        mouse_b = v[0];
        mouse_x = v[1] - 1;
        mouse_y = v[2] - 1;
        mouse_release = (c == 'm');
        return K_MOUSE;
    }
    for (;; c = read_byte()) {
        if (c < 0) return K_ESC;
        if (isdigit(c)) { num = num * 10 + (c - '0'); continue; }
        if (c == ';') { num = 0; continue; }
        break;
    }
    switch (c) {
    case 'A': return K_UP;
    case 'B': return K_DOWN;
    case 'C': return K_RIGHT;
    case 'D': return K_LEFT;
    case 'H': return K_HOME;
    case 'F': return K_END;
    case '~':
        if (num == 1 || num == 7) return K_HOME;
        if (num == 4 || num == 8) return K_END;
        if (num == 5) return K_PGUP;
        if (num == 6) return K_PGDN;
    }
    return K_ESC;
}

static void screen_on(void)
{
    printf("\x1b[?1049h\x1b[?1000h\x1b[?1006h\x1b[0m\x1b[2J");
    fflush(stdout);
}

static void screen_off(void)
{
    printf("\x1b[0m\x1b[?1006l\x1b[?1000l\x1b[?25h\x1b[?1049l");
    fflush(stdout);
}

static void screen_size(void)
{
    struct winsize ws;
    if ((ioctl(1, TIOCROZDETECT, &ws) == 0 || ioctl(1, TIOCGWINSZ, &ws) == 0) && ws.ws_row >= 8 && ws.ws_col >= 30) {
        rows = ws.ws_row;
        cols = ws.ws_col;
    }
}

static void run_app(const struct app *a)
{
    pid_t pid;
    int st = 0;
    if (a->confirm) {
        int k;
        snprintf(msg, sizeof(msg), "Run \"%s\"? y = yes, any other key = no", a->name);
        draw();
        do { k = read_key(); } while (k == K_MOUSE);
        msg[0] = '\0';
        if (k != 'y' && k != 'Y') return;
    }
    screen_off();                                    /* প্রোগ্রাম সাধারণ পর্দায় চলে */
    printf("\x1b[2J\x1b[H\x1b[1m%s\x1b[0m  (%s)\n\n", a->name, a->run);
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        char *argv[4];
        argv[0] = (char *)"sh";
        argv[1] = (char *)"-c";
        argv[2] = (char *)a->run;
        argv[3] = NULL;
        execv("/bin/sh", argv);
        _exit(127);
    }
    if (pid > 0) waitpid(pid, &st, 0);
    if (a->wait || pid < 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        if (pid < 0) printf("\ncould not start it\n");
        else if (!WIFEXITED(st)) printf("\n[%s ended by signal %d]\n", a->name, WTERMSIG(st));
        else if (WEXITSTATUS(st)) printf("\n[%s ended with code %d]\n", a->name, WEXITSTATUS(st));
        printf("\n-- press a key to return to the menu --");
        fflush(stdout);
        read_key();
    }
    screen_size();
    screen_on();
    load_apps();
    qsort(apps, (size_t)napps, sizeof(apps[0]), cmp_app);
    build_list();
}

static void search(void)
{
    size_t n = strlen(filter);
    for (;;) {
        int k;
        snprintf(msg, sizeof(msg), "search: %s_   (Enter = done, Esc = clear)", filter);
        build_list();
        top = 0;
        keep_visible();
        draw();
        k = read_key();
        if (k == '\r' || k == '\n') break;
        if (k == K_ESC || k == 3) { filter[0] = '\0'; break; }
        if ((k == '\b' || k == 127) && n > 0) filter[--n] = '\0';
        else if (k >= 32 && k < 127 && n + 1 < sizeof(filter)) { filter[n++] = (char)k; filter[n] = '\0'; }
    }
    msg[0] = '\0';
    build_list();
    top = 0;
    keep_visible();
}

int main(void)
{
    if (!isatty(0) || !isatty(1)) {
        fprintf(stderr, "menu: needs the console\n");
        return 1;
    }
    load_apps();
    qsort(apps, (size_t)napps, sizeof(apps[0]), cmp_app);
    build_list();
    screen_size();
    screen_on();
    for (;;) {
        int k;
        keep_visible();
        draw();
        k = read_key();
        msg[0] = '\0';
        switch (k) {
        case 'q': case 'Q': case 17: case 3:
            screen_off();
            return 0;
        case K_ESC:
            if (filter[0]) { filter[0] = '\0'; build_list(); top = 0; }
            break;
        case K_UP: case 'k': move_sel(-1); break;
        case K_DOWN: case 'j': move_sel(1); break;
        case K_PGUP: { int i; for (i = 0; i < list_rows() - 1; i++) move_sel(-1); } break;
        case K_PGDN: { int i; for (i = 0; i < list_rows() - 1; i++) move_sel(1); } break;
        case K_HOME: sel = -1; build_list(); top = 0; break;
        case K_END: { int i; for (i = 0; i < nshown; i++) move_sel(1); } break;
        case '/': search(); break;
        case '\r': case '\n': case K_RIGHT:
            if (sel >= 0) run_app(&apps[shown[sel]]);
            break;
        case 12:                                     /* Ctrl+L: মাপ নতুন করে */
            screen_size();
            printf("\x1b[2J");
            break;
        case K_MOUSE:
            if (mouse_b & 64) {
                top += (mouse_b & 1) ? 3 : -3;
                if (top > nshown - list_rows()) top = nshown - list_rows();
                if (top < 0) top = 0;
            } else if ((mouse_b & 3) == 0 && !mouse_release && !(mouse_b & 32) && mouse_y >= 1 &&
                       mouse_y <= list_rows()) {
                int i = top + mouse_y - 1;
                if (i < nshown && shown[i] >= 0) {
                    if (i == sel) run_app(&apps[shown[i]]);   /* বাছাই করাটায় আবার ক্লিক = চালানো */
                    else sel = i;
                }
            }
            break;
        default:
            if (k >= 32 && k < 127 && isalnum(k)) {     /* অক্ষর চাপলেই খোঁজা শুরু */
                filter[0] = (char)k;
                filter[1] = '\0';
                search();
            }
            break;
        }
    }
}
