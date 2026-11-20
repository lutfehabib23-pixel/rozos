/* rozOS — rozhelp: লিংকসহ সাহায্য-দর্শক (ধাপ ৮)
 *
 *   rozhelp [topic]        (shell-এ: help [topic])
 *
 * সাহায্যের পাতা: /usr/share/help/<topic>.txt (সোর্সে docs/help/)। লেখার নিয়ম:
 *   প্রথম লাইন          "# শিরোনাম"
 *   [[topic]]           লিংক — পাতার নাম দেখায়
 *   [[topic|লেখা]]      লিংক — "লেখা" দেখায়, যায় topic-এ
 *   `code`              কমান্ড/কোড — হলুদে
 *   == শিরোনাম ==       অংশের শিরোনাম — উজ্জ্বল সাদায়
 *
 * কী: Tab / Shift+Tab = পরের/আগের লিংক বাছা, Enter = লিংকে যাওয়া,
 *     Backspace বা ← = আগের পাতা, ↑ ↓ PgUp PgDn Home End = স্ক্রল,
 *     i = সূচিপত্র, / = পাতার নাম খোঁজা, q = বের হওয়া।
 * কনসোল না হলে (যেমন `help ls > file`) পাতাটা সাধারণ লেখা হিসেবে ছাপে। */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include <sys/ioctl.h>

#define HELP_DIR   "/usr/share/help"
#define MAX_LINES  2000
#define MAX_LINKS  400
#define HIST_MAX   32
#define NAME_MAX_  64

enum { K_UP = 1000, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_PGUP, K_PGDN, K_BTAB, K_ESC, K_MOUSE };
/* ধাপ ১২: শেষ মাউস-ঘটনা (xterm SGR) */
static int mouse_b, mouse_x, mouse_y, mouse_release;

/* একটা পাতা: লাইনগুলো "দেখানোর রূপে" (মার্কআপ সরানো) আর প্রতিটি অক্ষরের ধরন */
struct link {
    int line, col, len;           /* দেখানো লেখায় অবস্থান */
    char target[NAME_MAX_];
};

static char *text[MAX_LINES];     /* দেখানো লেখা */
static char *kind[MAX_LINES];     /* প্রতি অক্ষরে: 'n' সাধারণ, 'l' লিংক, 'c' কোড, 'h' শিরোনাম */
static int nlines;
static struct link links[MAX_LINKS];
static int nlinks;
static char title[128];
static char topic[NAME_MAX_];
static int rows = 24, cols = 80, top, sel = -1;
static char status_msg[160];

struct place { char topic[NAME_MAX_]; int top, sel; };
static struct place hist[HIST_MAX];
static int nhist;

/* ---------- পাতা পড়া ---------- */

static void clear_page(void)
{
    int i;
    for (i = 0; i < nlines; i++) { free(text[i]); free(kind[i]); }
    nlines = nlinks = 0;
    title[0] = '\0';
}

static void add_line(const char *src)
{
    size_t n = strlen(src);
    char *t = (char *)malloc(n + 1), *k = (char *)malloc(n + 1);
    int o = 0, heading = 0;
    const char *p = src;

    if (!t || !k || nlines >= MAX_LINES) { free(t); free(k); return; }
    if (!strncmp(p, "== ", 3)) {
        heading = 1;
        p += 3;
    }
    while (*p) {
        if (p[0] == '[' && p[1] == '[') {
            const char *end = strstr(p + 2, "]]"), *bar;
            if (end) {
                char target[NAME_MAX_];
                size_t tl;
                const char *shown;
                size_t sl;
                bar = memchr(p + 2, '|', (size_t)(end - p - 2));
                tl = (size_t)((bar ? bar : end) - (p + 2));
                if (tl >= sizeof(target)) tl = sizeof(target) - 1;
                memcpy(target, p + 2, tl);
                target[tl] = '\0';
                shown = bar ? bar + 1 : p + 2;
                sl = (size_t)(end - shown);
                if (nlinks < MAX_LINKS) {
                    links[nlinks].line = nlines;
                    links[nlinks].col = o;
                    links[nlinks].len = (int)sl;
                    strcpy(links[nlinks].target, target);
                    nlinks++;
                }
                memcpy(t + o, shown, sl);
                memset(k + o, 'l', sl);
                o += (int)sl;
                p = end + 2;
                continue;
            }
        }
        if (*p == '`') {
            const char *end = strchr(p + 1, '`');
            if (end) {
                size_t l = (size_t)(end - p - 1);
                memcpy(t + o, p + 1, l);
                memset(k + o, 'c', l);
                o += (int)l;
                p = end + 1;
                continue;
            }
        }
        if (heading && !strcmp(p, " ==")) break;
        t[o] = *p == '\t' ? ' ' : *p;
        k[o] = heading ? 'h' : 'n';
        o++;
        p++;
    }
    t[o] = '\0';
    k[o] = '\0';
    text[nlines] = t;
    kind[nlines] = k;
    nlines++;
}

static int load_topic(const char *name)
{
    char path[256], line[512];
    FILE *f;
    size_t i;

    for (i = 0; name[i]; i++) {
        if (!(isalnum((unsigned char)name[i]) || name[i] == '-' || name[i] == '_' || name[i] == '.')) {
            return -1;
        }
    }
    snprintf(path, sizeof(path), "%s/%s.txt", HELP_DIR, name);
    f = fopen(path, "r");
    if (!f) return -1;
    clear_page();
    strncpy(topic, name, sizeof(topic) - 1);
    topic[sizeof(topic) - 1] = '\0';
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (nlines == 0 && !title[0] && line[0] == '#' && line[1] == ' ') {
            strncpy(title, line + 2, sizeof(title) - 1);
            continue;
        }
        add_line(line);
    }
    fclose(f);
    if (!title[0]) strncpy(title, name, sizeof(title) - 1);
    top = 0;
    sel = nlinks ? 0 : -1;
    return 0;
}

/* ---------- কী ---------- */

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
    if (c == '<') {                                /* ধাপ ১২: ESC[<b;x;yM/m */
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
        if (c == ';') { num = 0; continue; }       /* ESC[1;2A — modifier উপেক্ষা */
        break;
    }
    switch (c) {
    case 'A': return K_UP;
    case 'B': return K_DOWN;
    case 'C': return K_RIGHT;
    case 'D': return K_LEFT;
    case 'H': return K_HOME;
    case 'F': return K_END;
    case 'Z': return K_BTAB;
    case '~':
        if (num == 1 || num == 7) return K_HOME;
        if (num == 4 || num == 8) return K_END;
        if (num == 5) return K_PGUP;
        if (num == 6) return K_PGDN;
    }
    return K_ESC;
}

/* ---------- আঁকা ---------- */

static char *obuf;
static size_t olen, ocap;

static void out(const char *s, size_t n)
{
    if (olen + n > ocap) {
        ocap = (olen + n) * 2 + 4096;
        obuf = (char *)realloc(obuf, ocap);
        if (!obuf) exit(1);
    }
    memcpy(obuf + olen, s, n);
    olen += n;
}

static void outs(const char *s) { out(s, strlen(s)); }

static void flush_out(void)
{
    size_t d = 0;
    while (d < olen) {
        ssize_t n = write(1, obuf + d, olen - d);
        if (n <= 0) break;
        d += (size_t)n;
    }
    olen = 0;
}

static int text_rows(void) { return rows - 2; }

static void draw(void)
{
    char buf[512];
    int r, w = cols - 1;

    outs("\x1b[?25l\x1b[H");
    snprintf(buf, sizeof(buf), " rozOS help: %s", title);
    {
        char right[80];
        size_t bl;
        snprintf(right, sizeof(right), "[%s]  %d/%d ", topic,
                 nlines ? (top + text_rows() > nlines ? nlines : top + text_rows()) : 0, nlines);
        bl = strlen(buf);
        while ((int)bl < w - (int)strlen(right) && bl + 1 < sizeof(buf)) buf[bl++] = ' ';
        buf[bl] = '\0';
        strncat(buf, right, sizeof(buf) - strlen(buf) - 1);
    }
    buf[w] = '\0';
    outs("\x1b[7m");
    outs(buf);
    outs("\x1b[K\x1b[0m");

    for (r = 0; r < text_rows(); r++) {
        int i = top + r, c, cur = 0;
        snprintf(buf, sizeof(buf), "\x1b[%d;1H", r + 2);
        outs(buf);
        if (i < nlines) {
            const char *t = text[i], *k = kind[i];
            int len = (int)strlen(t);
            if (len > w) len = w;
            for (c = 0; c < len; c++) {
                int want;
                /* এই অক্ষর বাছাই করা লিংকের ভেতরে? */
                int in_sel = sel >= 0 && links[sel].line == i && c >= links[sel].col &&
                             c < links[sel].col + links[sel].len;
                want = in_sel ? 4 : k[c] == 'l' ? 1 : k[c] == 'c' ? 2 : k[c] == 'h' ? 3 : 0;
                if (want != cur) {
                    outs(want == 4 ? "\x1b[0m\x1b[30;46m" : want == 1 ? "\x1b[0m\x1b[96m" :
                         want == 2 ? "\x1b[0m\x1b[93m" : want == 3 ? "\x1b[0m\x1b[97m" : "\x1b[0m");
                    cur = want;
                }
                out(&t[c], 1);
            }
            if (cur) outs("\x1b[0m");
        }
        outs("\x1b[K");
    }
    snprintf(buf, sizeof(buf), "\x1b[%d;1H\x1b[7m", rows);
    outs(buf);
    if (status_msg[0]) {
        snprintf(buf, sizeof(buf), " %.*s", w - 1, status_msg);
    } else {
        snprintf(buf, sizeof(buf), " %.*s", w - 1,
                 "Tab/Shift+Tab: pick link  Enter or click: open  Backspace: back  i: index  /: find  q: quit");
    }
    outs(buf);
    outs("\x1b[K\x1b[0m");
    flush_out();
}

/* বাছাই করা লিংক স্ক্রিনে দেখা যায় এমনভাবে স্ক্রল */
static void show_sel(void)
{
    if (sel < 0) return;
    if (links[sel].line < top) top = links[sel].line;
    if (links[sel].line >= top + text_rows()) top = links[sel].line - text_rows() + 1;
}

static void clamp_top(void)
{
    int max = nlines - text_rows();
    if (max < 0) max = 0;
    if (top > max) top = max;
    if (top < 0) top = 0;
}

/* স্ক্রল করলে লিংকের বাছাই স্ক্রিনের ভেতরে রাখা */
static void sel_follow_scroll(void)
{
    int i;
    if (sel >= 0 && links[sel].line >= top && links[sel].line < top + text_rows()) return;
    for (i = 0; i < nlinks; i++) {
        if (links[i].line >= top && links[i].line < top + text_rows()) { sel = i; return; }
    }
}

static void go(const char *name)
{
    if (nhist < HIST_MAX) {
        strcpy(hist[nhist].topic, topic);
        hist[nhist].top = top;
        hist[nhist].sel = sel;
        nhist++;
    }
    if (load_topic(name) < 0) {
        snprintf(status_msg, sizeof(status_msg), "No help page '%s' (press i for the index)", name);
        nhist--;
        load_topic(hist[nhist].topic);
        top = hist[nhist].top;
        sel = hist[nhist].sel;
    }
}

static void back(void)
{
    if (nhist == 0) {
        snprintf(status_msg, sizeof(status_msg), "This is the first page - q quits");
        return;
    }
    nhist--;
    load_topic(hist[nhist].topic);
    top = hist[nhist].top;
    sel = hist[nhist].sel;
}

/* নিচের লাইনে পাতার নাম লিখে যাওয়া */
static void find_topic(void)
{
    char q[NAME_MAX_];
    size_t n = 0;
    q[0] = '\0';
    for (;;) {
        int k;
        snprintf(status_msg, sizeof(status_msg), "Go to page (Enter, Esc = cancel): %s", q);
        draw();
        k = read_key();
        if (k == '\r' || k == '\n') break;
        if (k == K_ESC || k == 3) { status_msg[0] = '\0'; return; }
        if ((k == '\b' || k == 127) && n) q[--n] = '\0';
        else if (k > 32 && k < 127 && n + 1 < sizeof(q)) { q[n++] = (char)tolower(k); q[n] = '\0'; }
    }
    status_msg[0] = '\0';
    if (n) go(q);
}

static void print_plain(void)
{
    int i;
    printf("%s\n\n", title);
    for (i = 0; i < nlines; i++) printf("%s\n", text[i]);
}

int main(int argc, char **argv)
{
    struct winsize ws;
    const char *start = argc > 1 ? argv[1] : "index";

    if (load_topic(start) < 0) {
        fprintf(stderr, "rozhelp: no help page '%s' in %s\n", start, HELP_DIR);
        if (load_topic("index") < 0) return 1;
        if (!isatty(1)) return 1;
    }
    if (!isatty(0) || !isatty(1)) {
        print_plain();
        return 0;
    }
    /* ধাপ ১২: টার্মিনালের মাপ নিজে থেকে (stty দিয়ে বসানো থাকলে সেটাই) */
    if ((ioctl(1, TIOCROZDETECT, &ws) == 0 || ioctl(1, TIOCGWINSZ, &ws) == 0) && ws.ws_row >= 5 && ws.ws_col >= 20) {
        rows = ws.ws_row;
        cols = ws.ws_col;
    }
    /* ধাপ ১২: আলাদা পর্দা (বন্ধ করলে আগেরটা ফেরে) আর মাউস (ক্লিক, চাকা) */
    outs("\x1b[?1049h\x1b[?1000h\x1b[?1006h\x1b[0m\x1b[2J");
    for (;;) {
        int k;
        clamp_top();
        draw();
        k = read_key();
        status_msg[0] = '\0';
        switch (k) {
        case 'q': case 'Q': case 3: case 17:
            outs("\x1b[0m\x1b[?1006l\x1b[?1000l\x1b[?25h\x1b[?1049l");
            flush_out();
            return 0;
        case K_MOUSE:
            /* ধাপ ১২: চাকা = স্ক্রল, লিংকে ক্লিক = খোলা, নিচের লাইনে ক্লিক = ফেরা */
            if (mouse_b & 64) {
                top += (mouse_b & 1) ? 3 : -3;
                clamp_top();
                sel_follow_scroll();
            } else if ((mouse_b & 3) == 0 && !mouse_release && !(mouse_b & 32)) {
                int line = top + mouse_y - 1, i, hit = -1;
                for (i = 0; i < nlinks; i++) {
                    if (links[i].line == line && mouse_x >= links[i].col && mouse_x < links[i].col + links[i].len) hit = i;
                }
                if (hit >= 0) {
                    sel = hit;
                    go(links[hit].target);
                } else if (mouse_y == rows - 1) {
                    back();
                }
            }
            break;
        case '\t':
            if (nlinks) { sel = (sel + 1) % nlinks; show_sel(); }
            break;
        case K_BTAB:
            if (nlinks) { sel = (sel + nlinks - 1) % nlinks; show_sel(); }
            break;
        case '\r': case '\n': case K_RIGHT:
            if (sel >= 0) go(links[sel].target);
            break;
        case '\b': case 127: case K_LEFT: case 'b':
            back();
            break;
        case K_UP: case 'k':
            top--; clamp_top(); sel_follow_scroll();
            break;
        case K_DOWN: case 'j':
            top++; clamp_top(); sel_follow_scroll();
            break;
        case K_PGUP:
            top -= text_rows() - 1; clamp_top(); sel_follow_scroll();
            break;
        case K_PGDN: case ' ':
            top += text_rows() - 1; clamp_top(); sel_follow_scroll();
            break;
        case K_HOME: case 'g':
            top = 0; sel = nlinks ? 0 : -1;
            break;
        case K_END: case 'G':
            top = nlines; clamp_top(); sel_follow_scroll();
            break;
        case 'i': case 'I':
            if (strcmp(topic, "index")) go("index");
            break;
        case '/':
            find_topic();
            break;
        default:
            break;
        }
    }
}
