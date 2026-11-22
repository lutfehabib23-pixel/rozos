/* rozOS — edit: nano/নোটপ্যাড-ধাঁচের টেক্সট এডিটর (ধাপ ৭; ধাপ ১২-এ মাউস, সিলেক্ট, undo)
 *
 *   edit [file]
 *   edit --new [TEMPLATE]   নতুন নামহীন ফাইল: FILE001.c (থাকলে FILE002.c …), TEMPLATE-এর
 *                           (ধাপ ১৮খ: শুধু নাম — যেমন game — দিলে /usr/share/templates/game.c)
 *                           লেখা দিয়ে শুরু (ডিফল্ট /usr/share/templates/new.c); প্রথম Ctrl+S-এ
 *                           নাম চায় (ধাপ ১২ক)
 *
 * কী:
 *   তীর, Home/End, PgUp/PgDn  কার্সর সরানো        Ctrl+S  সংরক্ষণ
 *   Shift + এগুলো            সিলেক্ট করা          Ctrl+A  সব সিলেক্ট
 *   Ctrl+C / Ctrl+X / Ctrl+V  copy / cut / paste   Ctrl+Z / Ctrl+Y  undo / redo
 *   Ctrl+Q                   বের হওয়া            Ctrl+F  খোঁজা (আবার Ctrl+F = পরেরটা)
 *   Ctrl+G                   লাইনে যাওয়া         Ctrl+K  লাইন কাটা, Ctrl+U  বসানো
 *   Ctrl+N                   লাইন নম্বর দেখানো/লুকানো   Ctrl+L  আবার আঁকা (মাপ নতুন করে)
 *   Ctrl+W                   অন্য নামে সংরক্ষণ (Save as)  Ctrl+R  C ফাইল: সংরক্ষণ, tcc দিয়ে
 *                            কম্পাইল করে চালানো, Enter দিলে এডিটরে ফেরা (ধাপ ১২ক)
 *   মাউস: ক্লিকে কার্সর, টেনে সিলেক্ট, চাকায় স্ক্রল, ডানের স্ক্রলবারে ক্লিকে লাফ
 *   ধাপ ১৪খ: F10 (বা ওপরের লাইনে ক্লিক) = মেনু (File Edit Search Build View Help), প্রতিটা
 *   কাজের পাশে তার শর্টকাট; Ctrl+O অন্য ফাইল খোলা; F1 সব কী-র তালিকা। টার্মিনাল-উইন্ডো
 *   বড়/ছোট (পুরো পর্দা) করলে এডিটর নিজেই নতুন মাপে বসে যায় (Ctrl+L লাগে না)।
 *   Backspace/Delete, Enter, Tab  যেমন হওয়ার কথা (সিলেক্ট থাকলে সেটা মোছে/বদলায়)
 *
 * স্ক্রিন ANSI escape দিয়ে আঁকা হয় (কার্নেলের tty আর serial টার্মিনাল দুটোই বোঝে)।
 * ধাপ ১২: alternate screen (ESC[?1049h) — বন্ধ করলে আগের পর্দা ফেরে; মাউস-রিপোর্টিং
 * (ESC[?1000h ?1002h ?1006h); টার্মিনালের মাপ নিজে থেকে ধরা (TIOCROZDETECT)।
 * প্রতিবার শুধু বদলে যাওয়া লাইনগুলো পাঠানো হয় — serial-এ ধীর লিংকেও দ্রুত।
 * ফাইল লাইনের তালিকা হিসেবে মেমোরিতে থাকে; সংরক্ষণে পুরো ফাইল নতুন করে লেখা হয়। */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <poll.h>
#include <rozcref.h>

#define TAB_STOP   4
#define MSG_MAX    160
#define CTRL(k)    ((k) & 0x1F)
#define UNDO_MAX   200
#define UNDO_BYTES (8 * 1024 * 1024)

enum {
    K_UP = 1000, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_PGUP, K_PGDN, K_DEL, K_ESC, K_MOUSE,
    K_F1, K_F10, K_NEWFILE, K_NONE,          /* ধাপ ১৪খ */
    K_CREFW, K_CREFI                         /* ধাপ ১৯ক: C সহায়িকা (মেনু থেকে) */
};
#define K_SHIFT 0x10000                /* বিশেষ কী-র সাথে Shift */
#define K_CTRLM 0x20000                /* ... Ctrl */
#define KEYBASE(k) ((k) & 0xFFFF)

struct line {
    char *s;
    int len, cap;
};

static struct line *lines;
static int nlines, lines_cap;
static int cx, cy;                     /* কার্সর: লাইনের ভেতরে বাইট, লাইন নম্বর */
static int rowoff, coloff;             /* স্ক্রল */
static int screen_rows, screen_cols;   /* পুরো স্ক্রিন */
static int text_rows;                  /* লেখার জায়গা (উপরে শিরোনাম, নিচে ২ লাইন) */
static int text_cols;                  /* লেখার কলাম (gutter আর স্ক্রলবার বাদে) */
static int gutter;                     /* লাইন নম্বরের চওড়া (0 = বন্ধ) */
static int show_numbers;
static char filename[1024];
static int untitled;                   /* ধাপ ১২ক: নামটা শুধু প্রস্তাব — প্রথম ^S-এ নাম চাওয়া */
static int modified;
static char message[MSG_MAX];
static char **frame;                   /* আগের বার আঁকা লাইনগুলো (পরিবর্তন খোঁজার জন্য) */
static char *clip;                     /* ক্লিপবোর্ড (Ctrl+C/X/K) */
static int clip_len;
static int clip_is_line;               /* Ctrl+K দিয়ে কাটা পুরো লাইন (Ctrl+U-এর জন্য) */
static char last_search[128];
/* সিলেক্ট: anchor থেকে কার্সর পর্যন্ত */
static int sel_active, sel_x, sel_y;
/* মাউস (শেষ ঘটনা) */
static int mouse_b, mouse_x, mouse_y, mouse_release, mouse_dragging;

/* ---------- আউটপুট বাফার ---------- */

static char *obuf;
static size_t olen, ocap;

static void oput(const char *s, size_t n)
{
    if (olen + n > ocap) {
        ocap = (olen + n) * 2 + 1024;
        obuf = (char *)realloc(obuf, ocap);
        if (!obuf) exit(1);
    }
    memcpy(obuf + olen, s, n);
    olen += n;
}

static void ostr(const char *s) { oput(s, strlen(s)); }

static void oflush(void)
{
    size_t done = 0;
    while (done < olen) {
        ssize_t n = write(1, obuf + done, olen - done);
        if (n <= 0) break;
        done += (size_t)n;
    }
    olen = 0;
}

/* ---------- লাইন ---------- */

static void line_reserve(struct line *l, int need)
{
    if (need + 1 > l->cap) {
        l->cap = need * 2 + 16;
        l->s = (char *)realloc(l->s, (size_t)l->cap);
        if (!l->s) exit(1);
    }
}

static void insert_line(int at, const char *s, int len)
{
    if (nlines == lines_cap) {
        lines_cap = lines_cap ? lines_cap * 2 : 64;
        lines = (struct line *)realloc(lines, sizeof(*lines) * (size_t)lines_cap);
        if (!lines) exit(1);
    }
    memmove(&lines[at + 1], &lines[at], sizeof(*lines) * (size_t)(nlines - at));
    lines[at].s = NULL;
    lines[at].len = 0;
    lines[at].cap = 0;
    line_reserve(&lines[at], len);
    memcpy(lines[at].s, s, (size_t)len);
    lines[at].len = len;
    lines[at].s[len] = '\0';
    nlines++;
}

static void delete_line(int at)
{
    free(lines[at].s);
    memmove(&lines[at], &lines[at + 1], sizeof(*lines) * (size_t)(nlines - at - 1));
    nlines--;
}

static void free_all_lines(void)
{
    int i;
    for (i = 0; i < nlines; i++) free(lines[i].s);
    nlines = 0;
}

/* পুরো লেখা একটা স্ট্রিং-এ (লাইনের মাঝে '\n'); *len-এ দৈর্ঘ্য */
static char *join_text(size_t *len)
{
    size_t total = 0, p = 0;
    int i;
    char *t;
    for (i = 0; i < nlines; i++) total += (size_t)lines[i].len + 1;
    t = (char *)malloc(total + 1);
    if (!t) return NULL;
    for (i = 0; i < nlines; i++) {
        memcpy(t + p, lines[i].s, (size_t)lines[i].len);
        p += (size_t)lines[i].len;
        if (i + 1 < nlines) t[p++] = '\n';
    }
    t[p] = '\0';
    *len = p;
    return t;
}

static void set_text(const char *t, size_t len)
{
    size_t start = 0, i;
    free_all_lines();
    for (i = 0; i <= len; i++) {
        if (i == len || t[i] == '\n') {
            insert_line(nlines, t + start, (int)(i - start));
            start = i + 1;
        }
    }
    if (nlines == 0) insert_line(0, "", 0);
}

/* ---------- ফাইল ---------- */

static void set_message(const char *fmt, const char *arg)
{
    snprintf(message, sizeof(message), fmt, arg);
}

static int load_file(const char *path)
{
    FILE *f = fopen(path, "r");
    char *buf = NULL;
    size_t cap = 0;
    ssize_t n;

    if (!f) {
        if (errno == ENOENT) {
            insert_line(0, "", 0);
            set_message("New file: %s", path);
            return 0;
        }
        return -1;
    }
    while ((n = getline(&buf, &cap, f)) >= 0) {
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
        insert_line(nlines, buf, (int)n);
    }
    free(buf);
    fclose(f);
    if (nlines == 0) insert_line(0, "", 0);
    snprintf(message, sizeof(message), "Read %d line%s", nlines, nlines == 1 ? "" : "s");
    return 0;
}

static int prompt_edit(const char *question, char *out, size_t size);
static int read_key(void);
static void draw(void);
static int save_as(void);

static int save_file(void)
{
    int fd, i;
    long total = 0;
    /* ধাপ ১২ক: নাম নেই বা শুধু প্রস্তাবিত নাম → নাম চাওয়া */
    if (!filename[0] || untitled) return save_as();
    fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        snprintf(message, sizeof(message), "Cannot save %s: %s", filename, strerror(errno));
        return -1;
    }
    olen = 0;
    for (i = 0; i < nlines; i++) {
        oput(lines[i].s, (size_t)lines[i].len);
        oput("\n", 1);
    }
    total = (long)olen;
    {
        size_t done = 0;
        while (done < olen) {
            ssize_t n = write(fd, obuf + done, olen - done);
            if (n <= 0) {
                close(fd);
                olen = 0;
                snprintf(message, sizeof(message), "Write error: %s", strerror(errno));
                return -1;
            }
            done += (size_t)n;
        }
    }
    olen = 0;
    close(fd);
    modified = 0;
    snprintf(message, sizeof(message), "Saved %s (%d lines, %ld bytes)", filename, nlines, total);
    return 0;
}

/* ধাপ ১২ক: Save as — নাম চাওয়া (আগের/প্রস্তাবিত নাম লেখা থাকে, বদলানো যায়);
 * অন্য কোনো ফাইল আগে থেকে থাকলে মুছে লেখার আগে জিজ্ঞেস */
static int save_as(void)
{
    char name[sizeof(filename)];
    struct stat st;
    int n;
    snprintf(name, sizeof(name), "%s", filename);
    n = prompt_edit("Save as (Enter = save, Esc = cancel): ", name, sizeof(name));
    if (n <= 0) {
        set_message("%s", n < 0 ? "Not saved" : "Not saved - empty name");
        return -1;
    }
    if (strcmp(name, filename) != 0 || untitled) {
        if (stat(name, &st) == 0) {
            int a;
            if (S_ISDIR(st.st_mode)) {
                set_message("Not saved - %s is a directory", name);
                return -1;
            }
            snprintf(message, sizeof(message), "%s already exists - replace it? y = yes, n = no", name);
            draw();
            do { a = read_key(); } while (a == K_MOUSE);
            if (a != 'y' && a != 'Y') {
                set_message("%s", "Not saved");
                return -1;
            }
        }
    }
    snprintf(filename, sizeof(filename), "%s", name);
    untitled = 0;
    return save_file();
}

/* ---------- undo / redo (পুরো লেখার ছবি) ---------- */

struct snap {
    char *text;
    size_t len;
    int cx, cy, modified;
};

static struct snap undo_st[UNDO_MAX], redo_st[UNDO_MAX];
static int n_undo, n_redo;
static size_t undo_bytes;
static int typing_run;                 /* পরপর টাইপ করা অক্ষর = একটাই undo ধাপ */

static void snap_free(struct snap *s)
{
    if (s->text) undo_bytes -= s->len;
    free(s->text);
    s->text = NULL;
}

static int snap_take(struct snap *s)
{
    s->text = join_text(&s->len);
    if (!s->text) return -1;
    s->cx = cx;
    s->cy = cy;
    s->modified = modified;
    undo_bytes += s->len;
    return 0;
}

static void clear_redo(void)
{
    while (n_redo > 0) snap_free(&redo_st[--n_redo]);
}

/* কোনো বদলের ঠিক আগে ডাকা হয় */
static void undo_point(int is_typing)
{
    if (is_typing && typing_run) return;
    typing_run = is_typing;
    clear_redo();
    while (n_undo > 0 && (n_undo >= UNDO_MAX || undo_bytes > UNDO_BYTES)) {
        snap_free(&undo_st[0]);
        memmove(&undo_st[0], &undo_st[1], sizeof(undo_st[0]) * (size_t)(n_undo - 1));
        n_undo--;
    }
    if (snap_take(&undo_st[n_undo]) == 0) n_undo++;
}

static void snap_restore(struct snap *s)
{
    set_text(s->text, s->len);
    cy = s->cy < nlines ? s->cy : nlines - 1;
    cx = s->cx;
    modified = s->modified;
}

static void do_undo(int redo)
{
    struct snap *from = redo ? redo_st : undo_st, *to = redo ? undo_st : redo_st;
    int *nf = redo ? &n_redo : &n_undo, *nt = redo ? &n_undo : &n_redo;
    struct snap cur;
    typing_run = 0;
    sel_active = 0;
    if (*nf == 0) {
        set_message("%s", redo ? "Nothing to redo" : "Nothing to undo");
        return;
    }
    if (snap_take(&cur) < 0) return;
    if (*nt >= UNDO_MAX) {
        snap_free(&to[0]);
        memmove(&to[0], &to[1], sizeof(to[0]) * (size_t)(*nt - 1));
        (*nt)--;
    }
    to[(*nt)++] = cur;
    (*nf)--;
    snap_restore(&from[*nf]);
    snap_free(&from[*nf]);
    set_message("%s", redo ? "Redo" : "Undo (^Y = redo)");
}

/* ---------- কী পড়া ---------- */

static int read_byte(void)
{
    unsigned char c;
    return read(0, &c, 1) == 1 ? c : -1;
}

static int read_key(void)
{
    int c = read_byte(), num = 0, num2 = 0, nparams = 0, mods, key;
    if (c != 0x1B) return c;
    c = read_byte();
    if (c != '[' && c != 'O') return K_ESC;
    c = read_byte();
    if (c == '<') {
        /* ধাপ ১২: xterm SGR মাউস — ESC[<b;x;yM (চাপা/নড়া) বা m (ছাড়া) */
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
        if (isdigit(c)) {
            if (nparams == 0) nparams = 1;
            if (nparams == 1) num = num * 10 + (c - '0'); else num2 = num2 * 10 + (c - '0');
            continue;
        }
        if (c == ';') { nparams++; continue; }
        break;
    }
    /* ধাপ ১২: modifier — ESC[1;2A (Shift), ESC[3;5~ (Ctrl) ইত্যাদি */
    mods = 0;
    if (nparams >= 2 && num2 > 1) {
        int m = num2 - 1;
        if (m & 1) mods |= K_SHIFT;
        if (m & 4) mods |= K_CTRLM;
    }
    switch (c) {
    case 'A': key = K_UP; break;
    case 'B': key = K_DOWN; break;
    case 'C': key = K_RIGHT; break;
    case 'D': key = K_LEFT; break;
    case 'H': key = K_HOME; break;
    case 'F': key = K_END; break;
    case 'P': key = K_F1; break;                  /* ধাপ ১৪খ: ESC O P = F1 */
    case 'Q': case 'R': case 'S': return K_NONE;  /* F2-F4: এখনো কাজ নেই */
    case '~':
        switch (num) {
        case 1: case 7: key = K_HOME; break;
        case 4: case 8: key = K_END; break;
        case 3: key = K_DEL; break;
        case 5: key = K_PGUP; break;
        case 6: key = K_PGDN; break;
        case 21: key = K_F10; break;              /* ধাপ ১৪খ: মেনু */
        case 2: case 15: case 17: case 18: case 19: case 20: case 23: case 24:
            return K_NONE;                        /* Insert, অন্য F-কী: উপেক্ষা */
        default: return K_ESC;
        }
        break;
    default:
        return K_ESC;
    }
    return key | mods;
}

/* ---------- কার্সর ও স্ক্রল ---------- */

static int render_x(const struct line *l, int x)
{
    int rx = 0, i;
    for (i = 0; i < x && i < l->len; i++) {
        if (l->s[i] == '\t') rx += TAB_STOP - (rx % TAB_STOP);
        else rx++;
    }
    return rx;
}

/* render-কলাম → বাইট (মাউস ক্লিকের জন্য) */
static int byte_at_render(const struct line *l, int want)
{
    int rx = 0, i;
    for (i = 0; i < l->len; i++) {
        int w = l->s[i] == '\t' ? TAB_STOP - (rx % TAB_STOP) : 1;
        if (rx + w > want) return (want - rx) * 2 >= w ? i + 1 : i;
        rx += w;
    }
    return l->len;
}

static void clamp_cursor(void)
{
    if (cy >= nlines) cy = nlines - 1;
    if (cy < 0) cy = 0;
    if (cx > lines[cy].len) cx = lines[cy].len;
    if (cx < 0) cx = 0;
}

static void layout(void)
{
    int digits = 1, n = nlines;
    while (n >= 10) { n /= 10; digits++; }
    gutter = show_numbers ? digits + 1 : 0;
    text_cols = screen_cols - 1 - gutter - 1;            /* -1: শেষ কলাম ফাঁকা, -1: স্ক্রলবার */
    if (text_cols < 5) text_cols = 5;
}

static void scroll(void)
{
    int rx = render_x(&lines[cy], cx);
    if (cy < rowoff) rowoff = cy;
    if (cy >= rowoff + text_rows) rowoff = cy - text_rows + 1;
    if (rx < coloff) coloff = rx;
    if (rx >= coloff + text_cols - 1) coloff = rx - text_cols + 2;
    if (rowoff < 0) rowoff = 0;
}

/* ---------- সিলেক্ট ---------- */

/* সিলেক্টের শুরু (y1,x1) আর শেষ (y2,x2), শুরু ≤ শেষ; সিলেক্ট না থাকলে 0 */
static int sel_range(int *y1, int *x1, int *y2, int *x2)
{
    if (!sel_active || (sel_y == cy && sel_x == cx)) return 0;
    if (sel_y < cy || (sel_y == cy && sel_x < cx)) {
        *y1 = sel_y; *x1 = sel_x; *y2 = cy; *x2 = cx;
    } else {
        *y1 = cy; *x1 = cx; *y2 = sel_y; *x2 = sel_x;
    }
    if (*y1 >= nlines) *y1 = nlines - 1;
    if (*y2 >= nlines) *y2 = nlines - 1;
    if (*x1 > lines[*y1].len) *x1 = lines[*y1].len;
    if (*x2 > lines[*y2].len) *x2 = lines[*y2].len;
    return 1;
}

static int has_selection(void)
{
    int a, b, c, d;
    return sel_range(&a, &b, &c, &d);
}

static int in_selection(int y, int x)
{
    int y1, x1, y2, x2;
    if (!sel_range(&y1, &x1, &y2, &x2)) return 0;
    if (y < y1 || y > y2) return 0;
    if (y == y1 && x < x1) return 0;
    if (y == y2 && x >= x2) return 0;
    return 1;
}

static char *selection_text(int *len)
{
    int y1, x1, y2, x2, y;
    size_t total = 0, p = 0;
    char *t;
    if (!sel_range(&y1, &x1, &y2, &x2)) return NULL;
    for (y = y1; y <= y2; y++) total += (size_t)lines[y].len + 1;
    t = (char *)malloc(total + 1);
    if (!t) return NULL;
    for (y = y1; y <= y2; y++) {
        int a = y == y1 ? x1 : 0, b = y == y2 ? x2 : lines[y].len;
        memcpy(t + p, lines[y].s + a, (size_t)(b - a));
        p += (size_t)(b - a);
        if (y < y2) t[p++] = '\n';
    }
    t[p] = '\0';
    *len = (int)p;
    return t;
}

static void delete_selection(void)
{
    int y1, x1, y2, x2;
    struct line *a;
    if (!sel_range(&y1, &x1, &y2, &x2)) { sel_active = 0; return; }
    a = &lines[y1];
    {
        struct line *b = &lines[y2];
        int tail = b->len - x2;
        char *rest = (char *)malloc((size_t)tail + 1);
        if (!rest) return;
        memcpy(rest, b->s + x2, (size_t)tail + 1);
        while (y2 > y1) { delete_line(y1 + 1); y2--; }
        a = &lines[y1];
        line_reserve(a, x1 + tail);
        memcpy(a->s + x1, rest, (size_t)tail + 1);
        a->len = x1 + tail;
        free(rest);
    }
    cy = y1;
    cx = x1;
    sel_active = 0;
    modified = 1;
}

/* ---------- আঁকা ---------- */

/* একটা লেখার সারি: gutter + coloff থেকে text_cols অক্ষর (tab → স্পেস) + সিলেক্ট উল্টো রঙে */
static void render_row(int y, char *out, size_t size)
{
    const struct line *l = &lines[y];
    int rx = 0, n = 0, i, inv = 0;
    size_t p = 0;
#define PUT(str) do { const char *q_ = (str); while (*q_ && p + 1 < size) out[p++] = *q_++; } while (0)
    if (gutter) {
        char g[24];
        snprintf(g, sizeof(g), "\x1b[90m%*d \x1b[0m", gutter - 1, y + 1);
        PUT(g);
    }
    if (coloff > 0) PUT("\x1b[33m<\x1b[0m");
    for (i = 0; i < l->len && n < text_cols - (coloff > 0); i++) {
        unsigned char c = (unsigned char)l->s[i];
        int k = 1, j, sel = in_selection(y, i);
        char ch = (char)c;
        if (c == '\t') { k = TAB_STOP - (rx % TAB_STOP); ch = ' '; }
        else if (c < 32 || c >= 127) ch = '?';
        for (j = 0; j < k; j++, rx++) {
            if (rx >= coloff + (coloff > 0) && n < text_cols - (coloff > 0) - 1) {
                if (sel != inv) { PUT(sel ? "\x1b[7m" : "\x1b[27m"); inv = sel; }
                if (p + 1 < size) out[p++] = ch;
                n++;
            }
        }
    }
    /* লাইন শেষে সিলেক্ট থাকলে (পরের লাইনেও যায়) একটা উল্টো স্পেস */
    if (in_selection(y, l->len) && y < nlines - 1 && n < text_cols - 1) {
        if (!inv) { PUT("\x1b[7m"); inv = 1; }
        if (p + 1 < size) out[p++] = ' ';
        n++;
    }
    if (inv) PUT("\x1b[27m");
    if (render_x(l, l->len) > coloff + text_cols - 1) {   /* ডানে আরো আছে */
        while (n < text_cols - 1 && p + 1 < size) { out[p++] = ' '; n++; }
        PUT("\x1b[33m>\x1b[0m");
        n++;
    }
    while (n < text_cols && p + 1 < size) { out[p++] = ' '; n++; }
    out[p] = '\0';
#undef PUT
}

/* row-তম স্ক্রিন-লাইনের পুরো লেখা (ANSI সহ) বানিয়ে, আগের থেকে আলাদা হলে পাঠানো */
static void put_row(int row, const char *content)
{
    char pos[24];
    if (frame[row] && !strcmp(frame[row], content)) return;
    free(frame[row]);
    frame[row] = strdup(content);
    snprintf(pos, sizeof(pos), "\x1b[%d;1H", row + 1);
    ostr(pos);
    ostr(content);
    ostr("\x1b[K");
}

/* স্ক্রলবারের "thumb": কোন সারি থেকে কোন সারি */
static void thumb(int *top, int *len)
{
    int total = nlines > text_rows ? nlines : text_rows;
    *len = text_rows * text_rows / total;
    if (*len < 1) *len = 1;
    *top = (int)((long)rowoff * text_rows / total);
    if (*top + *len > text_rows) *top = text_rows - *len;
}

static void draw(void)
{
    size_t bsz = (size_t)screen_cols * 12 + 128;
    char *buf = (char *)malloc(bsz);
    char *txt = (char *)malloc((size_t)screen_cols + 1);
    int r, w = screen_cols - 1;                  /* শেষ কলামে লিখলে কিছু টার্মিনাল লাইন বদলায় */
    int ttop, tlen, y1, x1, y2, x2;
    char pos[32];

    if (!buf || !txt) exit(1);
    clamp_cursor();
    layout();
    scroll();
    thumb(&ttop, &tlen);
    ostr("\x1b[?25l");

    /* শিরোনাম — ধাপ ১৪খ: বাঁয়ে মেনুর নাম (F10 বা ক্লিক), তারপর ফাইলের নাম */
    snprintf(txt, (size_t)screen_cols, " File  Edit  Search  Build  View  Help  |  %s%s%s",
             filename[0] ? filename : "[no name]",
             untitled ? "  [new - not saved yet]" : "", modified ? "  [modified]" : "");
    snprintf(buf, bsz, "\x1b[7m%-*.*s\x1b[0m", w, w, txt);
    put_row(0, buf);

    for (r = 0; r < text_rows; r++) {
        int i = rowoff + r;
        const char *bar = (r >= ttop && r < ttop + tlen) ? "\x1b[7m \x1b[0m" : "\x1b[90m|\x1b[0m";
        if (i < nlines) {
            render_row(i, buf, bsz - 32);
        } else {
            snprintf(buf, bsz, "%*s\x1b[34m~\x1b[0m%*s", gutter, "", text_cols - 1, "");
        }
        strncat(buf, bar, bsz - strlen(buf) - 1);
        put_row(r + 1, buf);
    }

    /* অবস্থা-লাইন ও সাহায্য */
    if (!message[0] && sel_range(&y1, &x1, &y2, &x2)) {
        int len = 0;
        char *t = selection_text(&len);
        free(t);
        snprintf(message, sizeof(message), "%d characters selected (^C copy, ^X cut)", len);
    }
    snprintf(txt, (size_t)screen_cols, "%s", message);
    snprintf(buf, bsz, "%-*.*s", w - 18 > 0 ? w - 18 : 0, w - 18 > 0 ? w - 18 : 0, txt);
    {
        char where[40];
        snprintf(where, sizeof(where), "%6d:%-4d %3d%%", cy + 1, cx + 1,
                 nlines > 1 ? cy * 100 / (nlines - 1) : 100);
        strncat(buf, where, bsz - strlen(buf) - 1);
    }
    put_row(screen_rows - 2, buf);
    snprintf(txt, (size_t)screen_cols,
             "F10 Menu ^S Save ^O Open ^R Run ^Q Quit ^F Find ^G Go ^C/^X/^V Copy/Cut/Paste ^Z Undo F1 Keys");
    snprintf(buf, bsz, "\x1b[7m%-*.*s\x1b[0m", w, w, txt);
    put_row(screen_rows - 1, buf);

    snprintf(pos, sizeof(pos), "\x1b[%d;%dH\x1b[?25h", cy - rowoff + 2,
             gutter + (coloff > 0) + render_x(&lines[cy], cx) - coloff + 1);
    ostr(pos);
    oflush();
    free(buf);
    free(txt);
}

static void full_redraw(void)
{
    int r;
    for (r = 0; r < screen_rows; r++) {
        free(frame[r]);
        frame[r] = NULL;
    }
    ostr("\x1b[0m\x1b[2J");
    draw();
}

/* নিচের অবস্থা-লাইনে প্রশ্ন করে উত্তর পড়া; Esc/Ctrl+C = বাতিল (-1) */
static int prompt_input(const char *question, char *out, size_t size)
{
    out[0] = '\0';
    return prompt_edit(question, out, size);
}

/* ধাপ ১২ক: out-এ আগে থেকে যা আছে সেখান থেকে শুরু (Save as-এর নাম বদলানো) */
static int prompt_edit(const char *question, char *out, size_t size)
{
    size_t n = strlen(out);
    for (;;) {
        int k;
        snprintf(message, sizeof(message), "%s%s", question, out);
        draw();
        k = read_key();
        if (k == K_MOUSE) continue;
        if (k == '\r' || k == '\n') return (int)n;
        if (k == K_ESC || k == CTRL('c') || k == CTRL('q')) {
            message[0] = '\0';
            return -1;
        }
        if ((k == '\b' || k == 127) && n > 0) out[--n] = '\0';
        else if (k >= 32 && k < 127 && n + 1 < size) { out[n++] = (char)k; out[n] = '\0'; }
    }
}

/* ---------- সম্পাদনা ---------- */

static void insert_char(int c)
{
    struct line *l = &lines[cy];
    line_reserve(l, l->len + 1);
    memmove(l->s + cx + 1, l->s + cx, (size_t)(l->len - cx + 1));
    l->s[cx++] = (char)c;
    l->len++;
    modified = 1;
}

static void insert_newline(void)
{
    struct line *l = &lines[cy];
    int indent = 0;
    /* আগের লাইনের শুরুর ফাঁকা অংশ নতুন লাইনেও (কোড লেখায় সুবিধা) */
    while (indent < cx && (l->s[indent] == ' ' || l->s[indent] == '\t')) indent++;
    insert_line(cy + 1, l->s + cx, l->len - cx);
    l = &lines[cy];                                 /* realloc হয়ে থাকতে পারে */
    l->len = cx;
    l->s[cx] = '\0';
    if (indent) {
        struct line *n = &lines[cy + 1];
        line_reserve(n, n->len + indent);
        memmove(n->s + indent, n->s, (size_t)n->len + 1);
        memcpy(n->s, l->s, (size_t)indent);
        n->len += indent;
    }
    cy++;
    cx = indent;
    modified = 1;
}

/* লেখা (একাধিক লাইনও) কার্সরে বসানো — paste-এর জন্য; auto-indent ছাড়া */
static void insert_text(const char *t, int len)
{
    int i;
    for (i = 0; i < len; i++) {
        if (t[i] == '\n') {
            struct line *l = &lines[cy];
            insert_line(cy + 1, l->s + cx, l->len - cx);
            l = &lines[cy];
            l->len = cx;
            l->s[cx] = '\0';
            cy++;
            cx = 0;
        } else if (t[i] != '\r') {
            insert_char((unsigned char)t[i]);
        }
    }
    modified = 1;
}

static void backspace(void)
{
    if (cx > 0) {
        struct line *l = &lines[cy];
        memmove(l->s + cx - 1, l->s + cx, (size_t)(l->len - cx + 1));
        l->len--;
        cx--;
        modified = 1;
    } else if (cy > 0) {
        struct line *prev = &lines[cy - 1], *cur = &lines[cy];
        int plen = prev->len;
        line_reserve(prev, prev->len + cur->len);
        memcpy(prev->s + prev->len, cur->s, (size_t)cur->len + 1);
        prev->len += cur->len;
        delete_line(cy);
        cy--;
        cx = plen;
        modified = 1;
    }
}

static void delete_char(void)
{
    if (cx < lines[cy].len) {
        cx++;
        backspace();
    } else if (cy + 1 < nlines) {
        cy++;
        cx = 0;
        backspace();
    }
}

static void set_clip(char *t, int len, int is_line)
{
    free(clip);
    clip = t;
    clip_len = len;
    clip_is_line = is_line;
}

static void cut_line(void)
{
    struct line *l = &lines[cy];
    char *t = (char *)malloc((size_t)l->len + 1);
    if (!t) return;
    undo_point(0);
    memcpy(t, l->s, (size_t)l->len + 1);
    set_clip(t, l->len, 1);
    if (nlines > 1) {
        delete_line(cy);
    } else {
        l->len = 0;
        l->s[0] = '\0';
    }
    cx = 0;
    sel_active = 0;
    modified = 1;
    set_message("%s", "Cut 1 line (^U to paste it above, ^V at the cursor)");
}

static void paste_line(void)
{
    if (!clip) {
        set_message("%s", "Nothing to paste - cut a line with ^K first");
        return;
    }
    undo_point(0);
    if (clip_is_line) {
        insert_line(cy, clip, clip_len);
        cy++;
        cx = 0;
    } else {
        insert_text(clip, clip_len);
    }
    modified = 1;
}

static void copy_sel(int cut)
{
    int len = 0;
    char *t = selection_text(&len);
    if (!t) {
        set_message("%s", cut ? "Nothing selected - select with Shift+arrows or the mouse (^K cuts a line)"
                              : "Nothing selected - select with Shift+arrows or the mouse");
        return;
    }
    set_clip(t, len, 0);
    if (cut) {
        undo_point(0);
        delete_selection();
        snprintf(message, sizeof(message), "Cut %d characters (^V to paste)", len);
    } else {
        snprintf(message, sizeof(message), "Copied %d characters (^V to paste)", len);
    }
}

static void paste_clip(void)
{
    if (!clip) {
        set_message("%s", "Nothing to paste - copy with ^C or cut with ^X first");
        return;
    }
    undo_point(0);
    if (sel_active) delete_selection();
    insert_text(clip, clip_len);
    if (clip_is_line) insert_text("\n", 1);
}

static void find(void)
{
    char q[128];
    int i, start_y = cy, start_x = cx + 1;
    if (prompt_input("Find (Enter = search, Esc = cancel): ", q, sizeof(q)) < 0) return;
    if (q[0]) strcpy(last_search, q);
    if (!last_search[0]) return;
    for (i = 0; i <= nlines; i++) {
        int y = (start_y + i) % nlines;
        int from = (i == 0) ? start_x : 0;
        char *hit;
        if (from > lines[y].len) continue;
        hit = strstr(lines[y].s + from, last_search);
        if (hit) {
            cy = y;
            cx = (int)(hit - lines[y].s);
            /* খুঁজে পাওয়া শব্দটা সিলেক্ট করা — দেখতে সুবিধা, আর টাইপ করলে বদলে যায় */
            sel_active = 1;
            sel_y = cy;
            sel_x = cx;
            cx += (int)strlen(last_search);
            set_message("Found \"%s\" (^F again for the next one)", last_search);
            return;
        }
    }
    set_message("\"%s\" not found", last_search);
}

static void goto_line(void)
{
    char q[16];
    int n;
    if (prompt_input("Go to line: ", q, sizeof(q)) <= 0) return;
    n = atoi(q);
    if (n < 1) n = 1;
    if (n > nlines) n = nlines;
    cy = n - 1;
    cx = 0;
    sel_active = 0;
    message[0] = '\0';
}

/* ---------- মাউস ---------- */

static void mouse_to_cursor(int sx, int sy)
{
    int row = sy - 1, rcol;
    if (row < 0) row = 0;
    if (row >= text_rows) row = text_rows - 1;
    cy = rowoff + row;
    if (cy >= nlines) cy = nlines - 1;
    rcol = sx - gutter - (coloff > 0);
    if (rcol < 0) rcol = 0;
    cx = byte_at_render(&lines[cy], rcol + coloff);
}

static void handle_mouse(void)
{
    int b = mouse_b & 3, motion = (mouse_b & 32) != 0, wheel = (mouse_b & 64) != 0;
    if (wheel) {
        int step = 3;
        if (b == 0) {                                  /* ওপরে */
            rowoff -= step;
            if (rowoff < 0) rowoff = 0;
            if (cy >= rowoff + text_rows) cy = rowoff + text_rows - 1;
        } else {                                       /* নিচে */
            rowoff += step;
            if (rowoff > nlines - 1) rowoff = nlines - 1;
            if (rowoff < 0) rowoff = 0;
            if (cy < rowoff) cy = rowoff;
        }
        clamp_cursor();
        return;
    }
    if (b != 0) return;                                /* শুধু বাম বোতাম */
    if (mouse_release) {
        mouse_dragging = 0;
        return;
    }
    /* স্ক্রলবার (ডানের কলাম) */
    if (!motion && mouse_x >= gutter + text_cols && mouse_y >= 1 && mouse_y <= text_rows) {
        int target = (int)((long)(mouse_y - 1) * nlines / text_rows);
        rowoff = target;
        if (rowoff > nlines - text_rows) rowoff = nlines - text_rows;
        if (rowoff < 0) rowoff = 0;
        cy = rowoff;
        cx = 0;
        sel_active = 0;
        return;
    }
    if (mouse_y < 1 || mouse_y > text_rows) return;    /* শিরোনাম বা নিচের লাইন */
    if (!motion) {
        mouse_to_cursor(mouse_x, mouse_y);
        sel_active = 1;                                /* টানলে এখান থেকে সিলেক্ট */
        sel_x = cx;
        sel_y = cy;
        mouse_dragging = 1;
    } else if (mouse_dragging) {
        if (mouse_y <= 1 && rowoff > 0) rowoff--;      /* ধারে টানলে স্ক্রল */
        if (mouse_y >= text_rows && rowoff + text_rows < nlines) rowoff++;
        mouse_to_cursor(mouse_x, mouse_y);
    }
    typing_run = 0;
}

/* ---------- মূল লুপ ---------- */

static void screen_size(void)
{
    struct winsize ws;
    const char *l = getenv("LINES"), *c = getenv("COLUMNS");
    screen_rows = 24;
    screen_cols = 80;
    /* ধাপ ১২: আগে টার্মিনালকে জিজ্ঞেস (stty দিয়ে হাতে বসানো থাকলে সেটাই) */
    if ((ioctl(1, TIOCROZDETECT, &ws) == 0 || ioctl(1, TIOCGWINSZ, &ws) == 0) && ws.ws_row > 0 && ws.ws_col > 0) {
        screen_rows = ws.ws_row;
        screen_cols = ws.ws_col;
    }
    if (l && atoi(l) > 0) screen_rows = atoi(l);
    if (c && atoi(c) > 0) screen_cols = atoi(c);
    if (screen_rows < 5) screen_rows = 5;
    if (screen_cols < 20) screen_cols = 20;
    text_rows = screen_rows - 3;
}

/* ধাপ ১৪: Ctrl+C এডিটরের নিজের (copy) — টার্মিনালের SIGINT বন্ধ/চালু */
static void set_isig(int on)
{
    struct termios t;
    if (tcgetattr(0, &t) == 0) {
        if (on) t.c_lflag |= ISIG;
        else t.c_lflag &= ~ISIG;
        tcsetattr(0, TCSANOW, &t);
    }
}

static void leave(int code)
{
    set_isig(1);
    /* মাউস-রিপোর্টিং বন্ধ, আগের পর্দা ফেরত (ধাপ ১২) */
    ostr("\x1b[0m\x1b[?1006l\x1b[?1002l\x1b[?1000l\x1b[?25h\x1b[?1049l");
    oflush();
    exit(code);
}

/* ধাপ ১২ক: Ctrl+R — C ফাইল সংরক্ষণ, tcc দিয়ে কম্পাইল করে চালানো। এডিটরের পর্দা ছেড়ে
 * সাধারণ পর্দায় চলে (প্রোগ্রামের output/input স্বাভাবিক), Enter দিলে এডিটরে ফেরে। */
static void run_c(void)
{
    char cmd[sizeof(filename) + 96];
    const char *dot;
    int rc, c;
    if (save_file() < 0) return;
    dot = strrchr(filename, '.');
    if (!dot || strcmp(dot, ".c") != 0) {
        set_message("%s", "^R compiles and runs C files - save with a .c name (^W)");
        return;
    }
    ostr("\x1b[0m\x1b[?1006l\x1b[?1002l\x1b[?1000l\x1b[?25h\x1b[?1049l");
    oflush();
    printf("\n== tcc %s ==\n", filename);
    fflush(stdout);
    /* ধাপ ১৩: -lrozgfx — গ্রাফিক্স প্রোগ্রামও চলে (না লাগলে কিছুই যোগ হয় না) */
    /* ধাপ ১৮ক: -lrozgame -lm — গেমও চলে */
    snprintf(cmd, sizeof(cmd), "set -e; tcc %s -lrozgame -lrozgfx -lm -o /tmp/edit-run; /tmp/edit-run", filename);
    set_isig(1);                        /* চলন্ত প্রোগ্রাম Ctrl+C-তে থামে, এডিটর নয় */
    rc = system(cmd);
    set_isig(0);
    printf("\n== finished (exit status %d) - press Enter to return to the editor ==", WEXITSTATUS(rc));
    fflush(stdout);
    do { c = read_byte(); } while (c >= 0 && c != '\r' && c != '\n');
    ostr("\x1b[?1049h\x1b[?1000h\x1b[?1002h\x1b[?1006h");
    full_redraw();
    set_message("%s", WEXITSTATUS(rc) == 0 ? "Program finished (exit status 0)" : "Program or compile failed - see the output (^R again to retry)");
}

/* ধাপ ১২ক: edit --new — এই ফোল্ডারে প্রথম খালি FILE001.c, FILE002.c … নাম আর
 * টেমপ্লেটের লেখা; নামটা শুধু প্রস্তাব (untitled), প্রথম ^S-এ নাম চাওয়া হয় */
static void start_new(const char *tmpl)
{
    const char *ext = ".c", *dot;
    struct stat st;
    int i;
    FILE *f;
    if (!tmpl) tmpl = "/usr/share/templates/new.c";
    /* ধাপ ১৮খ: শুধু নাম দিলে (edit --new game) /usr/share/templates/<নাম>.c */
    if (!strchr(tmpl, '/') && stat(tmpl, &st) != 0) {
        static char tpath[256];
        snprintf(tpath, sizeof(tpath), "/usr/share/templates/%s%s", tmpl, strchr(tmpl, '.') ? "" : ".c");
        tmpl = tpath;
    }
    dot = strrchr(tmpl, '.');
    if (dot && !strchr(dot, '/') && strlen(dot) < 12) ext = dot;
    for (i = 1; i < 1000; i++) {
        snprintf(filename, sizeof(filename), "FILE%03d%s", i, ext);
        if (stat(filename, &st) != 0) break;
    }
    untitled = 1;
    f = fopen(tmpl, "r");
    if (f) {
        char *buf = NULL;
        size_t cap = 0;
        ssize_t n;
        while ((n = getline(&buf, &cap, f)) >= 0) {
            while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
            insert_line(nlines, buf, (int)n);
        }
        free(buf);
        fclose(f);
    }
    if (nlines == 0) insert_line(0, "", 0);
    set_message("%s", "New file - ^S asks for a name, ^R compile & run, ^Q quit");
}

/* ---------- ধাপ ১৪খ: মেনু (F10 বা ওপরের লাইনে ক্লিক) ---------- */
struct emenu_item { const char *label, *keys; int key; };
struct emenu { const char *title; const struct emenu_item *items; int n; };
static const struct emenu_item em_file[] = {
    { "New", "", K_NEWFILE }, { "Open...", "^O", CTRL('o') }, { "Save", "^S", CTRL('s') },
    { "Save As...", "^W", CTRL('w') }, { "Quit", "^Q", CTRL('q') }
};
static const struct emenu_item em_edit[] = {
    { "Undo", "^Z", CTRL('z') }, { "Redo", "^Y", CTRL('y') }, { "Cut", "^X", CTRL('x') },
    { "Copy", "^C", CTRL('c') }, { "Paste", "^V", CTRL('v') }, { "Cut line", "^K", CTRL('k') },
    { "Paste line", "^U", CTRL('u') }, { "Select all", "^A", CTRL('a') }
};
static const struct emenu_item em_search[] = {
    { "Find / find next", "^F", CTRL('f') }, { "Go to line...", "^G", CTRL('g') }
};
static const struct emenu_item em_build[] = {
    { "Save, compile and run", "^R", CTRL('r') }
};
static const struct emenu_item em_view[] = {
    { "Line numbers on/off", "^N", CTRL('n') }, { "Redraw / fit window", "^L", CTRL('l') }
};
static const struct emenu_item em_help[] = {
    { "Keyboard shortcuts", "F1", K_F1 },
    { "C reference: word at cursor", "Ctrl+F1", K_CREFW },     /* ধাপ ১৯ক */
    { "C reference: look up...", "Shift+F1", K_CREFI }
};
static const struct emenu emenus[] = {
    { "File", em_file, 5 }, { "Edit", em_edit, 8 }, { "Search", em_search, 2 },
    { "Build", em_build, 1 }, { "View", em_view, 2 }, { "Help", em_help, 3 }
};
#define NEMENU 6

/* শিরোনাম-লাইনে মেনুর নামের কলাম (0 থেকে): " File  Edit  Search ..." */
static int emenu_col(int m)
{
    int i, c = 1;
    for (i = 0; i < m; i++) c += (int)strlen(emenus[i].title) + 2;
    return c;
}

static int emenu_at(int col)
{
    int i;
    for (i = 0; i < NEMENU; i++) {
        int c = emenu_col(i);
        if (col >= c - 1 && col < c + (int)strlen(emenus[i].title) + 1) return i;
    }
    return -1;
}

static int emenu_width(int m)
{
    int i, w = 0;
    for (i = 0; i < emenus[m].n; i++) {
        int k = (int)strlen(emenus[m].items[i].label) + (int)strlen(emenus[m].items[i].keys) + 6;
        if (k > w) w = k;
    }
    return w;
}

static void emenu_draw(int m, int sel)
{
    int i, col = emenu_col(m) - 1, w = emenu_width(m);
    char b[160];
    if (col + w > screen_cols) col = screen_cols - w;
    if (col < 0) col = 0;
    /* খোলা মেনুর নাম উজ্জ্বল */
    snprintf(b, sizeof(b), "\x1b[1;%dH\x1b[0;30;47m %s \x1b[0m", emenu_col(m), emenus[m].title);
    ostr(b);
    for (i = 0; i < emenus[m].n && i + 2 < screen_rows; i++) {
        const struct emenu_item *it = &emenus[m].items[i];
        snprintf(b, sizeof(b), "\x1b[%d;%dH%s  %-*s%s  \x1b[0m", i + 2, col + 1,
                 i == sel ? "\x1b[0;30;47m" : "\x1b[0;97;100m",
                 w - 4 - (int)strlen(it->keys), it->label, it->keys);
        ostr(b);
    }
    ostr("\x1b[?25l");
    oflush();
}

static void forget_frame(void)
{
    int r;
    for (r = 0; r < screen_rows; r++) { free(frame[r]); frame[r] = NULL; }
}

/* মেনু চালানো: বেছে নেওয়া কাজের কী-কোড, নইলে 0 */
static int run_menu(int m)
{
    int sel = 0;
    for (;;) {
        int k, kb, col, w;
        forget_frame();
        draw();
        emenu_draw(m, sel);
        k = read_key();
        kb = KEYBASE(k);
        if (k == K_MOUSE) {
            int wheel = (mouse_b & 64) != 0, motion = (mouse_b & 32) != 0;
            col = emenu_col(m) - 1;
            w = emenu_width(m);
            if (col + w > screen_cols) col = screen_cols - w;
            if (col < 0) col = 0;
            if (wheel || mouse_release) continue;
            if (mouse_y == 0) {
                int t = emenu_at(mouse_x);
                if (motion) { if (t >= 0 && t != m) { m = t; sel = 0; } continue; }
                if (t >= 0 && t != m) { m = t; sel = 0; continue; }
                break;
            }
            if (mouse_x >= col && mouse_x < col + w && mouse_y >= 1 && mouse_y <= emenus[m].n) {
                sel = mouse_y - 1;
                if (!motion) { forget_frame(); return emenus[m].items[sel].key; }
                continue;
            }
            if (motion) continue;
            break;                                    /* বাইরে ক্লিক = বন্ধ */
        }
        if (kb == K_ESC || kb == K_F10 || k == CTRL('q')) break;
        if (kb == K_DOWN) sel = (sel + 1) % emenus[m].n;
        else if (kb == K_UP) sel = (sel + emenus[m].n - 1) % emenus[m].n;
        else if (kb == K_RIGHT) { m = (m + 1) % NEMENU; sel = 0; }
        else if (kb == K_LEFT) { m = (m + NEMENU - 1) % NEMENU; sel = 0; }
        else if (k == '\r' || k == '\n') { forget_frame(); return emenus[m].items[sel].key; }
    }
    forget_frame();
    return 0;
}

/* F1: সব কী — পর্দার মাঝখানে বাক্স, যেকোনো কী-তে বন্ধ */
/* ---------- ধাপ ১৯ক: C সহায়িকা (Ctrl+F1 = কার্সরের শব্দ, Shift+F1 = নাম লিখে) ---------- */

/* কার্সরের নিচের (বা ঠিক আগের) শনাক্তকারী — printf, strcpy, struct … */
static void word_at_cursor(char *out, size_t size)
{
    const struct line *l;
    int a, b;
    out[0] = 0;
    if (cy < 0 || cy >= nlines) return;
    l = &lines[cy];
    a = cx;
    if (a > l->len) a = l->len;
    if (a > 0 && (a == l->len || !(isalnum((unsigned char)l->s[a]) || l->s[a] == '_')) &&
        (isalnum((unsigned char)l->s[a - 1]) || l->s[a - 1] == '_')) a--;
    while (a > 0 && (isalnum((unsigned char)l->s[a - 1]) || l->s[a - 1] == '_')) a--;
    b = a;
    while (b < l->len && (isalnum((unsigned char)l->s[b]) || l->s[b] == '_')) b++;
    if (b - a <= 0 || (size_t)(b - a) >= size) return;
    memcpy(out, l->s + a, (size_t)(b - a));
    out[b - a] = 0;
}

/* পুরো পর্দায় পাতা দেখানো; ↑↓ PgUp PgDn স্ক্রল, C = উদাহরণ ক্লিপবোর্ডে (তারপর ^V), Esc = ফেরা */
static void show_cref(const char *name)
{
    const cref_entry *e;
    char *text, **row = NULL, b[512];
    int nrow = 0, cap = 0, top = 0, i;
    if (cref_load(NULL) < 0) {
        set_message("%s", "No C reference installed (/usr/share/cref)");
        return;
    }
    e = cref_find(name);
    if (!e) {
        const cref_entry *hits[1];
        if (name[0] && cref_search(name, hits, 1) == 1) e = hits[0];
    }
    if (!e) {
        snprintf(b, sizeof(b), "No C reference page for '%s' - try Shift+F1, or cref -k in a terminal", name);
        set_message("%s", b);
        return;
    }
    text = cref_format(e, screen_cols - 2);
    if (!text) return;
    {
        char *p = text;
        while (*p) {                                       /* লাইনে ভাগ */
            char *eol = strchr(p, '\n');
            if (nrow == cap) {
                char **n = (char **)realloc(row, sizeof(char *) * (size_t)(cap ? cap * 2 : 128));
                if (!n) break;
                row = n;
                cap = cap ? cap * 2 : 128;
            }
            row[nrow++] = p;
            if (!eol) break;
            *eol = 0;
            p = eol + 1;
        }
    }
    for (;;) {
        int k, kb, body = screen_rows - 2;
        forget_frame();
        snprintf(b, sizeof(b), "\x1b[1;1H\x1b[0;30;47m%-*.*s\x1b[0m", screen_cols, screen_cols,
                 " C reference   Up/Down/PgUp/PgDn scroll   C copy the example (then ^V)   Esc back");
        ostr(b);
        for (i = 0; i < body; i++) {
            const char *t = top + i < nrow ? row[top + i] : "";
            int code = 0, title = top + i == 0;
            if (top + i < nrow && t[0] == ' ' && t[1] == ' ' && t[2] == ' ' && t[3] == ' ') code = 1;
            snprintf(b, sizeof(b), "\x1b[%d;1H\x1b[0;%sm%-*.*s\x1b[0m", i + 2,
                     title ? "1;93" : code ? "92" : (t[0] && t[0] != ' ' && t[strlen(t) - 1] == ':') ? "1;96" : "97",
                     screen_cols, screen_cols, t);
            ostr(b);
        }
        snprintf(b, sizeof(b), "\x1b[%d;1H\x1b[0;30;47m%-*.*s\x1b[0m", screen_rows, screen_cols, screen_cols,
                 message[0] ? message : " ");
        ostr(b);
        ostr("\x1b[?25l");
        oflush();
        k = read_key();
        kb = KEYBASE(k);
        message[0] = 0;
        if (kb == K_ESC || k == 'q' || k == CTRL('q')) break;
        if (kb == K_DOWN || k == 'j') top++;
        else if (kb == K_UP || k == 'k') top--;
        else if (kb == K_PGDN || k == ' ') top += body - 1;
        else if (kb == K_PGUP) top -= body - 1;
        else if (kb == K_HOME) top = 0;
        else if (kb == K_END) top = nrow - body;
        else if (k == K_MOUSE) {
            if (mouse_b & 64) top += (mouse_b & 1) ? 3 : -3;
        } else if ((k == 'c' || k == 'C') && *e->example) {
            char *t = strdup(e->example);
            if (t) set_clip(t, (int)strlen(t), 0);
            set_message("%s", " Example copied - Esc, then ^V pastes it");
        }
        if (top > nrow - body) top = nrow - body;
        if (top < 0) top = 0;
    }
    free(row);
    free(text);
    forget_frame();
}

static void cref_word(void)
{
    char w[64];
    word_at_cursor(w, sizeof(w));
    if (!w[0]) {
        set_message("%s", "Put the cursor on a word (like printf), then Ctrl+F1");
        return;
    }
    show_cref(w);
}

static void cref_ask(void)
{
    char w[64] = "";
    word_at_cursor(w, sizeof(w));
    if (!prompt_edit("C reference for: ", w, sizeof(w)) || !w[0]) return;
    show_cref(w);
}

static void show_keys(void)
{
    static const char *const t[] = {
        " Keyboard shortcuts (F10 = menu, click the top line too)  ",
        "",
        "  ^S save        ^W save as      ^O open        ^Q quit",
        "  ^Z undo        ^Y redo         ^A select all",
        "  ^C copy        ^X cut          ^V paste",
        "  ^K cut line    ^U paste line   ^F find (again = next)",
        "  ^G go to line  ^N line numbers ^L redraw / fit window",
        "  ^R save, compile with tcc and run (C files)",
        "  Ctrl+F1 C help on the word    Shift+F1 C help by name",
        "  Shift+arrows select, mouse click/drag/wheel",
        "",
        "                    press any key"
    };
    int i, n = (int)(sizeof(t) / sizeof(t[0])), w = 60, r0 = (screen_rows - n) / 2, c0 = (screen_cols - w) / 2;
    char b[200];
    if (r0 < 0) r0 = 0;
    if (c0 < 0) c0 = 0;
    forget_frame();
    draw();
    for (i = 0; i < n && r0 + i < screen_rows; i++) {
        snprintf(b, sizeof(b), "\x1b[%d;%dH\x1b[0;%sm%-*.*s\x1b[0m", r0 + i + 1, c0 + 1,
                 i == 0 ? "30;47" : "97;100", w, w, t[i]);
        ostr(b);
    }
    ostr("\x1b[?25l");
    oflush();
    while (read_key() == K_MOUSE && mouse_release) { }
    forget_frame();
}

/* ধাপ ১৪খ: টার্মিনালের মাপ বদলেছে কিনা (TIOCGWINSZ আগের বারের সাথে মেলানো) */
static int last_ws_row = -1, last_ws_col = -1;

static void remember_winsize(void)
{
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0) { last_ws_row = ws.ws_row; last_ws_col = ws.ws_col; }
}

static int winsize_changed(void)
{
    struct winsize ws;
    const char *l = getenv("LINES"), *c = getenv("COLUMNS");
    if ((l && atoi(l) > 0) || (c && atoi(c) > 0)) return 0;   /* হাতে বসানো মাপ */
    if (ioctl(1, TIOCGWINSZ, &ws) != 0 || ws.ws_row == 0 || ws.ws_col == 0) return 0;
    return ws.ws_row != last_ws_row || ws.ws_col != last_ws_col;
}

static void refit_screen(void)
{
    screen_size();
    remember_winsize();
    free(frame);
    frame = (char **)calloc((size_t)screen_rows, sizeof(char *));
    if (!frame) leave(1);
    full_redraw();
}

/* কী আসার অপেক্ষা; মাঝে মাঝে মাপ দেখা (উইন্ডো বড় করলে নিজে মানিয়ে নেওয়া) */
static int wait_key(void)
{
    for (;;) {
        struct pollfd p;
        p.fd = 0;
        p.events = POLLIN;
        p.revents = 0;
        if (poll(&p, 1, 400) > 0) return read_key();
        if (winsize_changed()) refit_screen();
    }
}

static void open_other(void)
{
    char name[1024] = "";
    if (modified) {
        int a;
        set_message("%s", "Save changes first? y = save, n = discard, Esc = cancel");
        draw();
        do { a = read_key(); } while (a == K_MOUSE);
        if (a == 'y' || a == 'Y') { if (save_file() < 0) return; }
        else if (a != 'n' && a != 'N') { set_message("%s", "Cancelled"); return; }
    }
    if (prompt_input("Open file: ", name, sizeof(name)) <= 0) { set_message("%s", "Cancelled"); return; }
    free_all_lines();
    if (load_file(name) < 0) {
        char m[MSG_MAX];
        snprintf(m, sizeof(m), "Cannot open %s: %s", name, strerror(errno));
        insert_line(0, "", 0);
        set_message("%s", m);
        filename[0] = '\0';
        untitled = 1;
        modified = 0;
        return;
    }
    strncpy(filename, name, sizeof(filename) - 1);
    untitled = 0;
    modified = 0;
    cx = cy = rowoff = coloff = 0;
    sel_active = 0;
    while (n_undo > 0) snap_free(&undo_st[--n_undo]);
    clear_redo();
    set_message("%s", "Opened");
}

static void new_other(void)
{
    if (modified) {
        int a;
        set_message("%s", "Save changes first? y = save, n = discard, Esc = cancel");
        draw();
        do { a = read_key(); } while (a == K_MOUSE);
        if (a == 'y' || a == 'Y') { if (save_file() < 0) return; }
        else if (a != 'n' && a != 'N') { set_message("%s", "Cancelled"); return; }
    }
    free_all_lines();
    modified = 0;
    cx = cy = rowoff = coloff = 0;
    sel_active = 0;
    while (n_undo > 0) snap_free(&undo_st[--n_undo]);
    clear_redo();
    start_new(NULL);
}

/* কার্সর-সরানো কী: Shift থাকলে সিলেক্ট বাড়ে, না থাকলে সিলেক্ট বাতিল */
static void before_move(int k)
{
    if (k & K_SHIFT) {
        if (!sel_active) {
            sel_active = 1;
            sel_x = cx;
            sel_y = cy;
        }
    } else {
        sel_active = 0;
    }
    typing_run = 0;
}

int main(int argc, char **argv)
{
    int quit_warned = 0;

    if (!isatty(0) || !isatty(1)) {
        fprintf(stderr, "edit: needs the console (stdin and stdout)\n");
        return 1;
    }
    screen_size();
    frame = (char **)calloc((size_t)screen_rows, sizeof(char *));
    if (!frame) return 1;
    if (argc > 1 && !strcmp(argv[1], "--new")) {
        start_new(argc > 2 ? argv[2] : NULL);
    } else if (argc > 1) {
        strncpy(filename, argv[1], sizeof(filename) - 1);
        if (load_file(filename) < 0) {
            fprintf(stderr, "edit: %s: %s\n", filename, strerror(errno));
            return 1;
        }
    } else {
        insert_line(0, "", 0);
        set_message("%s", "No file name yet - ^S asks for one");
    }
    if (!message[0]) set_message("%s", "^S save, ^Q quit");
    set_isig(0);                        /* ধাপ ১৪ */
    /* ধাপ ১২: আলাদা পর্দা + মাউস (ক্লিক, টানা, SGR ভাষা) */
    ostr("\x1b[?1049h\x1b[?1000h\x1b[?1002h\x1b[?1006h");
    remember_winsize();                 /* ধাপ ১৪খ */
    full_redraw();

    for (;;) {
        int k = wait_key(), kb, edit_key = 0;
        /* ধাপ ১৪খ: মেনু — F10, বা ওপরের লাইনে মেনুর নামে ক্লিক */
        if (k == K_F10) {
            k = run_menu(0);
            if (!k) { draw(); continue; }
        } else if (k == K_MOUSE && mouse_y == 0 && !mouse_release && !(mouse_b & (32 | 64)) && (mouse_b & 3) == 0) {
            int m = emenu_at(mouse_x);
            if (m >= 0) {
                k = run_menu(m);
                if (!k) { draw(); continue; }
            }
        }
        if (k == K_NONE) continue;
        if (k == K_F1) { show_keys(); full_redraw(); continue; }
        /* ধাপ ১৯ক: C সহায়িকা */
        if (k == (K_F1 | K_CTRLM) || k == K_CREFW) { cref_word(); full_redraw(); continue; }
        if (k == (K_F1 | K_SHIFT) || k == K_CREFI) { cref_ask(); full_redraw(); continue; }
        if (k == K_NEWFILE) { new_other(); full_redraw(); continue; }
        if (k == CTRL('o')) { open_other(); full_redraw(); continue; }
        kb = KEYBASE(k);
        if (k != CTRL('q')) quit_warned = 0;

        switch (k) {
        case CTRL('q'):
            if (modified && !quit_warned) {
                int a;
                /* nano-র মতো: এক কী-তেই উত্তর (Enter লাগে না) */
                set_message("%s", "Save changes before quitting? y = save, n = discard, Esc = cancel");
                draw();
                do { a = read_key(); } while (a == K_MOUSE);
                if (a == 'y' || a == 'Y') {
                    if (save_file() < 0) break;
                    leave(0);
                }
                if (a == 'n' || a == 'N') leave(0);
                set_message("%s", "Cancelled");
                break;
            }
            leave(0);
            break;
        case CTRL('s'):
            save_file();
            break;
        case CTRL('w'):
            save_as();
            break;
        case CTRL('r'):
            run_c();
            break;
        case CTRL('f'):
            find();
            break;
        case CTRL('g'):
            goto_line();
            break;
        case CTRL('k'):
            cut_line();
            break;
        case CTRL('u'):
            paste_line();
            break;
        case CTRL('c'):
            copy_sel(0);
            break;
        case CTRL('x'):
            copy_sel(1);
            break;
        case CTRL('v'):
            paste_clip();
            break;
        case CTRL('z'):
            do_undo(0);
            break;
        case CTRL('y'):
            do_undo(1);
            break;
        case CTRL('a'):
            sel_active = 1;
            sel_y = 0;
            sel_x = 0;
            cy = nlines - 1;
            cx = lines[cy].len;
            typing_run = 0;
            break;
        case CTRL('n'):
            show_numbers = !show_numbers;
            full_redraw();
            continue;
        case CTRL('l'):
            refit_screen();                  /* ধাপ ১৪খ: একই কাজ, মাপও মনে রাখে */
            continue;
        case K_MOUSE:
            handle_mouse();
            break;
        case '\r':
        case '\n':
            undo_point(0);
            if (sel_active) delete_selection();
            insert_newline();
            edit_key = 1;
            break;
        case '\b':
        case 127:
            undo_point(1);
            if (has_selection()) delete_selection();
            else backspace();
            sel_active = 0;
            edit_key = 1;
            break;
        case K_DEL:
            undo_point(0);
            if (has_selection()) delete_selection();
            else delete_char();
            sel_active = 0;
            edit_key = 1;
            break;
        case K_ESC:
            sel_active = 0;
            break;
        default:
            if (kb >= K_UP && kb <= K_PGDN) {
                before_move(k);
                switch (kb) {
                case K_LEFT:
                    if (cx > 0) cx--;
                    else if (cy > 0) { cy--; cx = lines[cy].len; }
                    break;
                case K_RIGHT:
                    if (cx < lines[cy].len) cx++;
                    else if (cy + 1 < nlines) { cy++; cx = 0; }
                    break;
                case K_UP:
                    if (cy > 0) cy--;
                    break;
                case K_DOWN:
                    if (cy + 1 < nlines) cy++;
                    break;
                case K_HOME:
                    if (k & K_CTRLM) cy = 0;
                    cx = 0;
                    break;
                case K_END:
                    if (k & K_CTRLM) cy = nlines - 1;
                    cx = lines[cy].len;
                    break;
                case K_PGUP:
                    cy -= text_rows;
                    rowoff -= text_rows;
                    if (rowoff < 0) rowoff = 0;
                    break;
                case K_PGDN:
                    cy += text_rows;
                    rowoff += text_rows;
                    break;
                }
                clamp_cursor();
                if (rowoff > nlines - 1) rowoff = nlines - 1;
            } else if (k == '\t' || (k >= 32 && k < 127)) {
                undo_point(1);
                if (has_selection()) delete_selection();
                sel_active = 0;
                insert_char(k);
                edit_key = 1;
            }
            break;
        }
        if (!edit_key || !((k >= 32 && k < 127) || k == '\t' || k == '\b' || k == 127)) {
            typing_run = 0;                         /* টাইপের ধারা ভাঙলে পরের বদল নতুন undo ধাপ */
        }
        if (k != CTRL('s') && k != CTRL('f') && k != CTRL('g') && k != CTRL('k') &&
            k != CTRL('u') && k != CTRL('q') && k != CTRL('c') && k != CTRL('x') &&
            k != CTRL('v') && k != CTRL('z') && k != CTRL('y') && k != K_ESC && k != K_MOUSE) {
            message[0] = '\0';
        }
        draw();
    }
}
