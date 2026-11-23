/* rozOS — Code: গ্রাফিক্যাল C এডিটর (ধাপ ১৪খ)
 *
 *   code [ফাইল]
 *
 * মেনু (File Edit Search Build View Help) — প্রতিটা কাজের পাশে শর্টকাট লেখা:
 *   Ctrl+N নতুন, Ctrl+O খোলো, Ctrl+S সেভ, F12 অন্য নামে সেভ, Ctrl+Q বন্ধ
 *   Ctrl+Z/Ctrl+Y undo/redo, Ctrl+X/C/V কাটা/কপি/পেস্ট, Ctrl+A সব
 *   Ctrl+F খোঁজা, F3 আবার, Ctrl+E বদলানো, Ctrl+G লাইনে যাও
 *   Ctrl+B (F7) কম্পাইল, Ctrl+R (F5) কম্পাইল করে চালাও — আলাদা টার্মিনাল-উইন্ডোতে
 *   F8 Output প্যানেল, F1 সাহায্য
 * C-র রঙিন লেখা (keyword, type, string, comment, #include), লাইন নম্বর, মাউসে কার্সর বসানো
 * ও টেনে বাছাই, ডাবল-ক্লিকে শব্দ, স্ক্রলবার, auto-indent। কম্পাইলের ভুলের লাইনে ক্লিক
 * করলে সেই লাইনে যায়। উইন্ডো বড়/ছোট (বা পুরো পর্দা) করলে সব নতুন মাপে বসে যায়।
 * ক্লিপবোর্ড টার্মিনালের সাথে এক (/tmp/.clipboard)। rozui-র উইজেট দিয়ে বানানো —
 * নিজের প্রোগ্রামে কীভাবে মেনু/ডায়ালগ বানাতে হয় তার উদাহরণও (help rozui)। */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <rozgfx.h>
#include <rozui.h>

#define TOOL_H   32
#define STATUS_H 22
#define OUT_ROWS 7
#define TABW     4
#define UNDO_MAX 64

struct line { char *s; int len, cap; unsigned char st; };
/* UTF-8: পরের বাইটগুলো (10xxxxxx) পর্দায় জায়গা নেয় না — পুরো অক্ষর একটা ঘর (ফন্টে বাংলা
 * নেই, তাই একটা বিন্দু-চিহ্ন দেখায়; ফাইলের বাইট যেমন ছিল তেমনই থাকে) */
#define CONT(c) ((((unsigned char)(c)) & 0xC0) == 0x80)

static struct line *L;
static int nl, capl;
static int cx, cy;                    /* কার্সর: বাইট, লাইন */
static int want_col = -1;             /* ওপর-নিচে যাওয়ার সময় কলাম মনে রাখা */
static int sel_on, sx, sy;            /* বাছাইয়ের শুরু (anchor) */
static int top, left;                 /* স্ক্রল: লাইন, কলাম */
static char path[512];
static int untitled, modified, makefile_mode;
static gfx_font *font;
static int cw = 8, chh = 16, font_px = 16;
static int show_numbers = 1, show_output;
static char status_msg[160];
static char last_find[128], last_repl[128];
static int hl_dirty = 1;
static int follow_cursor;             /* পরের ফ্রেমে কার্সর দেখা যায় এমন স্ক্রল */
static int dragging;
static ui_ctx U;

/* Output প্যানেল */
static char *outl[400];
static int nout, out_sel = -1, out_top;

/* ---------- লাইন ---------- */
static void line_reserve(struct line *l, int need)
{
    if (need + 1 > l->cap) {
        int c = l->cap ? l->cap : 16;
        while (c < need + 1) c *= 2;
        l->s = (char *)realloc(l->s, (size_t)c);
        if (!l->s) { perror("code"); exit(1); }
        l->cap = c;
    }
}

static void insert_line(int at, const char *s, int len)
{
    if (nl + 1 > capl) {
        capl = capl ? capl * 2 : 256;
        L = (struct line *)realloc(L, sizeof(*L) * (size_t)capl);
        if (!L) { perror("code"); exit(1); }
    }
    memmove(&L[at + 1], &L[at], sizeof(*L) * (size_t)(nl - at));
    memset(&L[at], 0, sizeof(*L));
    line_reserve(&L[at], len);
    if (len) memcpy(L[at].s, s, (size_t)len);
    L[at].len = len;
    L[at].s[len] = '\0';
    nl++;
}

static void delete_line(int at)
{
    free(L[at].s);
    memmove(&L[at], &L[at + 1], sizeof(*L) * (size_t)(nl - at - 1));
    nl--;
}

static void clear_all(void)
{
    while (nl > 0) delete_line(nl - 1);
}

static char *join_text(size_t *len)
{
    size_t n = 0, o = 0;
    int i;
    char *t;
    for (i = 0; i < nl; i++) n += (size_t)L[i].len + 1;
    t = (char *)malloc(n + 1);
    if (!t) return NULL;
    for (i = 0; i < nl; i++) {
        memcpy(t + o, L[i].s, (size_t)L[i].len);
        o += (size_t)L[i].len;
        if (i + 1 < nl) t[o++] = '\n';
    }
    t[o] = '\0';
    *len = o;
    return t;
}

static void set_text(const char *t, size_t len)
{
    size_t i, st = 0;
    clear_all();
    for (i = 0; i <= len; i++) {
        if (i == len || t[i] == '\n') {
            size_t e = i;
            if (e > st && t[e - 1] == '\r') e--;
            insert_line(nl, t + st, (int)(e - st));
            st = i + 1;
        }
    }
    if (nl == 0) insert_line(0, "", 0);
    hl_dirty = 1;
}

/* ---------- স্তম্ভ (ট্যাব বুঝে) ---------- */
static int render_x(const struct line *l, int x)
{
    int i, r = 0;
    for (i = 0; i < x && i < l->len; i++) r = l->s[i] == '\t' ? (r / TABW + 1) * TABW : CONT(l->s[i]) ? r : r + 1;
    return r;
}

static int byte_at_render(const struct line *l, int want)
{
    int i, r = 0;
    for (i = 0; i < l->len; i++) {
        int nr = l->s[i] == '\t' ? (r / TABW + 1) * TABW : CONT(l->s[i]) ? r : r + 1;
        if (nr > want) {
            int k = (want - r) * 2 >= nr - r ? i + 1 : i;
            while (k < l->len && CONT(l->s[k])) k++;          /* অক্ষরের মাঝখানে নয় */
            return k;
        }
        r = nr;
    }
    return l->len;
}

static void clamp_cursor(void)
{
    if (cy >= nl) cy = nl - 1;
    if (cy < 0) cy = 0;
    if (cx > L[cy].len) cx = L[cy].len;
    if (cx < 0) cx = 0;
}

/* ---------- বাছাই ---------- */
static int sel_range(int *y1, int *x1, int *y2, int *x2)
{
    if (!sel_on || (sx == cx && sy == cy)) return 0;
    if (sy < cy || (sy == cy && sx < cx)) { *y1 = sy; *x1 = sx; *y2 = cy; *x2 = cx; }
    else { *y1 = cy; *x1 = cx; *y2 = sy; *x2 = sx; }
    if (*y1 >= nl) *y1 = nl - 1;
    if (*y2 >= nl) *y2 = nl - 1;
    if (*x1 > L[*y1].len) *x1 = L[*y1].len;
    if (*x2 > L[*y2].len) *x2 = L[*y2].len;
    return 1;
}

static char *get_text(int y1, int x1, int y2, int x2, int *len)
{
    int y, n = 0, o = 0;
    char *t;
    for (y = y1; y <= y2; y++) n += L[y].len + 1;
    t = (char *)malloc((size_t)n + 1);
    if (!t) return NULL;
    for (y = y1; y <= y2; y++) {
        int a = y == y1 ? x1 : 0, b = y == y2 ? x2 : L[y].len;
        memcpy(t + o, L[y].s + a, (size_t)(b - a));
        o += b - a;
        if (y < y2) t[o++] = '\n';
    }
    t[o] = '\0';
    *len = o;
    return t;
}

static void delete_range(int y1, int x1, int y2, int x2)
{
    struct line *a = &L[y1];
    int tail = L[y2].len - x2;
    line_reserve(a, x1 + tail);
    memmove(a->s + x1, L[y2].s + x2, (size_t)tail);
    a->len = x1 + tail;
    a->s[a->len] = '\0';
    while (y2 > y1) delete_line(y2--);
    cy = y1;
    cx = x1;
    modified = 1;
    hl_dirty = 1;
}

static int delete_selection(void)
{
    int y1, x1, y2, x2;
    if (!sel_range(&y1, &x1, &y2, &x2)) { sel_on = 0; return 0; }
    delete_range(y1, x1, y2, x2);
    sel_on = 0;
    return 1;
}

static void insert_text(const char *t, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        struct line *l = &L[cy];
        if (t[i] == '\r') continue;
        if (t[i] == '\n') {
            insert_line(cy + 1, l->s + cx, l->len - cx);
            l = &L[cy];
            l->len = cx;
            l->s[cx] = '\0';
            cy++;
            cx = 0;
        } else {
            line_reserve(l, l->len + 1);
            memmove(l->s + cx + 1, l->s + cx, (size_t)(l->len - cx + 1));
            l->s[cx++] = t[i];
            l->len++;
        }
    }
    modified = 1;
    hl_dirty = 1;
}

/* ---------- undo (পুরো লেখার ছবি — ছোট ফাইলের জন্য সহজ আর নির্ভরযোগ্য) ---------- */
struct snap { char *t; size_t len; int cx, cy; };
static struct snap undo_st[UNDO_MAX], redo_st[UNDO_MAX];
static int n_undo, n_redo, typing_run;

static void snap_take(struct snap *s)
{
    s->t = join_text(&s->len);
    s->cx = cx;
    s->cy = cy;
}

static void push_snap(struct snap *st, int *n)
{
    if (*n == UNDO_MAX) {
        free(st[0].t);
        memmove(&st[0], &st[1], sizeof(st[0]) * (UNDO_MAX - 1));
        (*n)--;
    }
    snap_take(&st[*n]);
    if (st[*n].t) (*n)++;
}

static void undo_point(int typing)
{
    if (typing && typing_run) return;
    typing_run = typing;
    while (n_redo > 0) free(redo_st[--n_redo].t);
    push_snap(undo_st, &n_undo);
}

static void do_undo(int redo)
{
    struct snap *from = redo ? redo_st : undo_st, *to = redo ? undo_st : redo_st;
    int *nf = redo ? &n_redo : &n_undo, *nt = redo ? &n_undo : &n_redo;
    if (*nf == 0) { snprintf(status_msg, sizeof(status_msg), "Nothing to %s", redo ? "redo" : "undo"); return; }
    push_snap(to, nt);
    (*nf)--;
    set_text(from[*nf].t, from[*nf].len);
    cx = from[*nf].cx;
    cy = from[*nf].cy;
    free(from[*nf].t);
    clamp_cursor();
    sel_on = 0;
    modified = 1;
    typing_run = 0;
    follow_cursor = 1;
}

static void clear_undo(void)
{
    while (n_undo > 0) free(undo_st[--n_undo].t);
    while (n_redo > 0) free(redo_st[--n_redo].t);
    typing_run = 0;
}

/* ---------- ফাইল ---------- */
static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static void update_title(void)
{
    char t[96];
    snprintf(t, sizeof(t), "%s%s - Code", modified ? "*" : "", untitled ? "untitled.c" : base_name(path));
    gfx_set_title(t);
}

static void set_path(const char *p)
{
    char cwd[256];
    if (p[0] == '/' || !getcwd(cwd, sizeof(cwd))) snprintf(path, sizeof(path), "%s", p);
    else snprintf(path, sizeof(path), "%s/%s", strcmp(cwd, "/") ? cwd : "", p);
    {
        const char *b = base_name(path);
        size_t n = strlen(b);
        makefile_mode = !strcmp(b, "Makefile") || !strcmp(b, "makefile") || (n > 3 && !strcmp(b + n - 3, ".mk"));
    }
}

static int load_file(const char *p)
{
    FILE *f = fopen(p, "rb");
    char *buf;
    long n;
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)(n > 0 ? n : 0) + 1);
    if (!buf) { fclose(f); return -1; }
    n = (long)fread(buf, 1, (size_t)(n > 0 ? n : 0), f);
    fclose(f);
    set_text(buf, (size_t)n);
    free(buf);
    set_path(p);
    untitled = 0;
    modified = 0;
    cx = cy = top = left = 0;
    sel_on = 0;
    clear_undo();
    snprintf(status_msg, sizeof(status_msg), "Opened %s (%d lines)", base_name(path), nl);
    return 0;
}

static int save_as(void);

static int save_file(void)
{
    size_t len;
    char *t;
    FILE *f;
    if (untitled) return save_as();
    t = join_text(&len);
    if (!t) return -1;
    f = fopen(path, "wb");
    if (!f) {
        char m[600];
        snprintf(m, sizeof(m), "Cannot save %s:\n%s", path, strerror(errno));
        ui_msgbox("Save", m, UI_MB_OK | UI_MB_ERROR);
        free(t);
        return -1;
    }
    fwrite(t, 1, len, f);
    if (len == 0 || t[len - 1] != '\n') fputc('\n', f);
    fclose(f);
    free(t);
    modified = 0;
    snprintf(status_msg, sizeof(status_msg), "Saved %s", path);
    return 0;
}

static int save_as(void)
{
    char p[512];
    snprintf(p, sizeof(p), "%s", path);
    if (!ui_filedlg("Save as", p, sizeof(p), 1)) return -1;
    set_path(p);
    untitled = 0;
    return save_file();
}

/* বদল থাকলে জিজ্ঞেস: 1 = এগোও, 0 = থামো */
static int confirm_discard(void)
{
    char m[300];
    int a;
    if (!modified) return 1;
    snprintf(m, sizeof(m), "Save changes to %s?", untitled ? "untitled.c" : base_name(path));
    a = ui_msgbox("Code", m, UI_MB_YESNOCANCEL | UI_MB_QUESTION);
    if (a == UI_YES) return save_file() == 0;
    return a == UI_NO;
}

static void new_file(void)
{
    char cwd[256];
    if (!confirm_discard()) return;
    clear_all();
    insert_line(0, "", 0);
    if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "/home");
    snprintf(path, sizeof(path), "%s/untitled.c", strcmp(cwd, "/") ? cwd : "");
    untitled = 1;
    modified = 0;
    makefile_mode = 0;
    cx = cy = top = left = 0;
    sel_on = 0;
    clear_undo();
    hl_dirty = 1;
    snprintf(status_msg, sizeof(status_msg), "New file - Ctrl+S asks for a name");
}

/* ধাপ ১৮খ: File > New game — /usr/share/templates/game.c দিয়ে শুরু (নামহীন) */
static void new_game_file(void)
{
    char cwd[256];
    if (!confirm_discard()) return;
    if (load_file("/usr/share/templates/game.c") < 0) {
        ui_msgbox("Code", "/usr/share/templates/game.c is missing", UI_MB_OK | UI_MB_ERROR);
        return;
    }
    if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "/home");
    if (!strcmp(cwd, "/usr/share/templates") || !strcmp(cwd, "/")) strcpy(cwd, "/home");
    snprintf(path, sizeof(path), "%s/untitled.c", cwd);
    untitled = 1;
    modified = 1;
    snprintf(status_msg, sizeof(status_msg), "New game - F5 compiles and runs, Ctrl+S asks for a name");
}

static void open_file(void)
{
    char p[512];
    if (!confirm_discard()) return;
    snprintf(p, sizeof(p), "%s", path);
    if (!ui_filedlg("Open", p, sizeof(p), 0)) return;
    if (load_file(p) < 0) {
        char m[600];
        snprintf(m, sizeof(m), "Cannot open %s:\n%s", p, strerror(errno));
        ui_msgbox("Open", m, UI_MB_OK | UI_MB_ERROR);
    }
}

/* ---------- ক্লিপবোর্ড ---------- */
static void copy_sel(int cut)
{
    int y1, x1, y2, x2, n = 0;
    char *t;
    if (!sel_range(&y1, &x1, &y2, &x2)) { snprintf(status_msg, sizeof(status_msg), "Nothing selected"); return; }
    t = get_text(y1, x1, y2, x2, &n);
    if (t) { ui_clip_set(t, n); free(t); }
    if (cut) { undo_point(0); delete_selection(); }
    snprintf(status_msg, sizeof(status_msg), "%d characters %s", n, cut ? "cut" : "copied");
}

static void paste(void)
{
    int n;
    char *t = ui_clip_get(&n);
    if (!t) { snprintf(status_msg, sizeof(status_msg), "Clipboard is empty"); return; }
    undo_point(0);
    delete_selection();
    insert_text(t, n);
    free(t);
    follow_cursor = 1;
}

/* ---------- খোঁজা ---------- */
static int find_next(const char *what, int from_y, int from_x)
{
    int y, pass;
    size_t wl = strlen(what);
    if (!wl) return 0;
    for (pass = 0; pass < 2; pass++) {
        for (y = pass ? 0 : from_y; y < nl && (!pass || y <= from_y); y++) {
            char *p = (y == from_y && !pass) ? L[y].s + from_x : L[y].s;
            char *hit = strstr(p, what);
            if (hit) {
                sy = y;
                sx = (int)(hit - L[y].s);
                cy = y;
                cx = sx + (int)wl;
                sel_on = 1;
                follow_cursor = 1;
                if (pass) snprintf(status_msg, sizeof(status_msg), "Search wrapped to the top");
                return 1;
            }
        }
    }
    snprintf(status_msg, sizeof(status_msg), "Not found: %s", what);
    return 0;
}

static void do_find(void)
{
    if (!ui_inputbox("Find", "Find what:", last_find, sizeof(last_find))) return;
    find_next(last_find, cy, cx);
}

static void do_replace(void)
{
    int y, count = 0;
    size_t fl, rl;
    if (!ui_inputbox("Replace", "Find what:", last_find, sizeof(last_find)) || !last_find[0]) return;
    if (!ui_inputbox("Replace", "Replace with:", last_repl, sizeof(last_repl))) return;
    fl = strlen(last_find);
    rl = strlen(last_repl);
    undo_point(0);
    for (y = 0; y < nl; y++) {
        int x = 0;
        char *hit;
        while ((hit = strstr(L[y].s + x, last_find))) {
            int at = (int)(hit - L[y].s), tail = L[y].len - at - (int)fl;
            line_reserve(&L[y], L[y].len - (int)fl + (int)rl);
            memmove(L[y].s + at + rl, L[y].s + at + fl, (size_t)tail + 1);
            memcpy(L[y].s + at, last_repl, rl);
            L[y].len += (int)rl - (int)fl;
            x = at + (int)rl;
            count++;
        }
    }
    if (count) { modified = 1; hl_dirty = 1; }
    clamp_cursor();
    sel_on = 0;
    snprintf(status_msg, sizeof(status_msg), "%d replaced", count);
}

static void do_goto(void)
{
    char b[16] = "";
    int n;
    if (!ui_inputbox("Go to line", "Line number:", b, sizeof(b))) return;
    n = atoi(b);
    if (n < 1) return;
    cy = n - 1;
    cx = 0;
    clamp_cursor();
    sel_on = 0;
    follow_cursor = 1;
}

/* ---------- কম্পাইল / চালানো ---------- */
static void out_clear(void)
{
    while (nout > 0) free(outl[--nout]);
    out_sel = -1;
    out_top = 0;
}

static void out_add(const char *s)
{
    if (nout < 400) outl[nout++] = strdup(s);
}

static void exe_name(char *out, size_t size)
{
    size_t n;
    snprintf(out, size, "%s", path);
    n = strlen(out);
    if (n > 2 && !strcmp(out + n - 2, ".c")) out[n - 2] = '\0';
    else snprintf(out + n, size - n, ".out");
}

/* 0 = ঠিক আছে */
/* ধাপ ১৯গ: প্রোগ্রাম কোন হেডার #include করেছে দেখে বাড়তি লাইব্রেরি (C সহায়িকার উদাহরণ
 * সরাসরি চলে): rozhttp → -lrozhttp -lbearssl, lua/lauxlib → -llua, zlib → -lz, rozcref → -lrozcref */
static int extra_libs(const char *file, const char **libs, int max)
{
    static const struct { const char *inc, *lib1, *lib2; } t[] = {
        { "<rozhttp.h>", "-lrozhttp", "-lbearssl" }, { "<lua.h>", "-llua", NULL },
        { "<lauxlib.h>", "-llua", NULL }, { "<zlib.h>", "-lz", NULL }, { "<rozcref.h>", "-lrozcref", NULL },
    };
    char line[512];
    int n = 0, i, j, k;
    FILE *f = fopen(file, "r");
    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, "include")) continue;
        for (i = 0; i < (int)(sizeof(t) / sizeof(t[0])); i++) {
            const char *l[2];
            if (!strstr(line, t[i].inc)) continue;
            l[0] = t[i].lib1;
            l[1] = t[i].lib2;
            for (j = 0; j < 2 && l[j]; j++) {
                for (k = 0; k < n; k++) if (!strcmp(libs[k], l[j])) break;
                if (k == n && n < max) libs[n++] = l[j];
            }
        }
    }
    fclose(f);
    return n;
}

static int compile(void)
{
    char exe[512], dir[512], buf[512];
    const char *extra[8];
    int pfd[2], st = 0, errors = 0, warnings = 0, nextra, i;
    pid_t pid;
    FILE *f;
    if (modified || untitled) {
        if (save_file() < 0) return -1;
    }
    exe_name(exe, sizeof(exe));
    snprintf(dir, sizeof(dir), "%s", path);
    if (strrchr(dir, '/')) *strrchr(dir, '/') = '\0';
    if (!dir[0]) strcpy(dir, "/");
    out_clear();
    /* ধাপ ১৮ক: -lrozgame -lm — গেমও Build হয় (না লাগলে কিছু যোগ হয় না) */
    nextra = extra_libs(path, extra, 8);
    snprintf(buf, sizeof(buf), "tcc -o %s %s", base_name(exe), base_name(path));
    for (i = 0; i < nextra; i++) {
        strncat(buf, " ", sizeof(buf) - strlen(buf) - 1);
        strncat(buf, extra[i], sizeof(buf) - strlen(buf) - 1);
    }
    strncat(buf, " -lrozui -lrozgame -lrozgfx -lm", sizeof(buf) - strlen(buf) - 1);
    out_add(buf);
    show_output = 1;
    if (pipe(pfd) < 0) { out_add("cannot start the compiler (pipe)"); return -1; }
    pid = fork();
    if (pid == 0) {
        dup2(pfd[1], 1);
        dup2(pfd[1], 2);
        close(pfd[0]);
        close(pfd[1]);
        chdir(dir);
        {
            const char *av[20];
            int n = 0;
            av[n++] = "tcc"; av[n++] = "-o"; av[n++] = base_name(exe); av[n++] = base_name(path);
            for (i = 0; i < nextra; i++) av[n++] = extra[i];
            av[n++] = "-lrozui"; av[n++] = "-lrozgame"; av[n++] = "-lrozgfx"; av[n++] = "-lm";
            av[n] = NULL;
            execvp("tcc", (char *const *)av);
        }
        printf("cannot run tcc: %s\n", strerror(errno));
        _exit(127);
    }
    close(pfd[1]);
    f = fdopen(pfd[0], "r");
    while (f && fgets(buf, sizeof(buf), f)) {
        buf[strcspn(buf, "\r\n")] = '\0';
        if (strstr(buf, "error")) errors++;
        else if (strstr(buf, "warning")) warnings++;
        out_add(buf);
    }
    if (f) fclose(f); else close(pfd[0]);
    waitpid(pid, &st, 0);
    if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
        snprintf(buf, sizeof(buf), "OK - %s built%s", base_name(exe), warnings ? " (with warnings)" : "");
        out_add(buf);
        snprintf(status_msg, sizeof(status_msg), "Compiled: %s", base_name(exe));
        return 0;
    }
    snprintf(buf, sizeof(buf), "FAILED - %d error(s). Click an error line to jump to it.", errors ? errors : 1);
    out_add(buf);
    snprintf(status_msg, sizeof(status_msg), "Compile failed");
    return -1;
}

static void run_program(void)
{
    char exe[512], dir[512], cmd[600];
    const char *argv[6];
    if (compile() != 0) return;
    exe_name(exe, sizeof(exe));
    snprintf(dir, sizeof(dir), "%s", path);
    if (strrchr(dir, '/')) *strrchr(dir, '/') = '\0';
    if (!dir[0]) strcpy(dir, "/");
    snprintf(cmd, sizeof(cmd), "./%s", base_name(exe));
    if (!gfx_windowed()) {
        ui_msgbox("Run", "Running needs the desktop.\nStart it with 'desktop'.", UI_MB_OK | UI_MB_INFO);
        return;
    }
    argv[0] = "rozterm"; argv[1] = "-w"; argv[2] = "-e"; argv[3] = cmd; argv[4] = NULL;
    ui_launch(argv, dir);
    snprintf(status_msg, sizeof(status_msg), "Running %s in a terminal window", base_name(exe));
}

/* "file.c:12: error: ..." → 12 */
static void jump_to_error(const char *s)
{
    const char *c = strchr(s, ':');
    if (!c || !isdigit((unsigned char)c[1])) return;
    cy = atoi(c + 1) - 1;
    cx = 0;
    clamp_cursor();
    sel_on = 0;
    follow_cursor = 1;
}

/* ---------- রঙিন লেখা ---------- */
static const char *const kw[] = {
    "if", "else", "for", "while", "do", "switch", "case", "default", "break", "continue",
    "return", "goto", "sizeof", "typedef", "struct", "union", "enum", "static", "extern",
    "const", "volatile", "register", "inline", "auto", "restrict", NULL
};
static const char *const types[] = {
    "int", "char", "void", "short", "long", "float", "double", "signed", "unsigned", "size_t",
    "ssize_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "int8_t", "int16_t", "int32_t",
    "int64_t", "FILE", "pid_t", "off_t", "bool", "_Bool", "gfx_surface", "gfx_event", "ui_ctx", NULL
};
#define C_TEXT  GFX_RGB(225, 225, 225)
#define C_KW    GFX_RGB(140, 175, 235)
#define C_TYPE  GFX_RGB(110, 200, 185)
#define C_STR   GFX_RGB(196, 214, 140)
#define C_NUM   GFX_RGB(235, 175, 105)
#define C_CMT   GFX_RGB(128, 138, 120)
#define C_PRE   GFX_RGB(205, 145, 205)

static int in_list(const char *const *list, const char *w, int n)
{
    int i;
    for (i = 0; list[i]; i++) if ((int)strlen(list[i]) == n && !strncmp(list[i], w, (size_t)n)) return 1;
    return 0;
}

/* একটা লাইনের প্রতিটা বাইটের রং; ফেরত: লাইন শেষে block comment চলছে কিনা */
static int color_line(const struct line *l, int st, uint32_t *col)
{
    int i = 0, n = l->len;
    const char *s = l->s;
    int pre = 0, j;
    for (j = 0; j < n && (s[j] == ' ' || s[j] == '\t'); j++) ;
    if (!st && j < n && s[j] == '#') pre = 1;
    while (i < n) {
        if (st) {                                  /* comment-এর ভেতরে */
            col[i] = C_CMT;
            if (s[i] == '*' && i + 1 < n && s[i + 1] == '/') { col[i + 1] = C_CMT; i += 2; st = 0; continue; }
            i++;
            continue;
        }
        if (s[i] == '/' && i + 1 < n && s[i + 1] == '/') { while (i < n) col[i++] = C_CMT; break; }
        if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') { col[i] = col[i + 1] = C_CMT; i += 2; st = 1; continue; }
        if (s[i] == '"' || s[i] == '\'') {
            char q = s[i];
            col[i++] = C_STR;
            while (i < n && s[i] != q) {
                if (s[i] == '\\' && i + 1 < n) col[i++] = C_STR;
                col[i++] = C_STR;
            }
            if (i < n) col[i++] = C_STR;
            continue;
        }
        if (pre && s[i] == '<') {                  /* #include <...> */
            while (i < n && s[i] != '>') col[i++] = C_STR;
            if (i < n) col[i++] = C_STR;
            continue;
        }
        if (isdigit((unsigned char)s[i])) {
            while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '.')) col[i++] = C_NUM;
            continue;
        }
        if (isalpha((unsigned char)s[i]) || s[i] == '_') {
            int b = i;
            uint32_t c;
            while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '_')) i++;
            c = pre ? C_PRE : in_list(kw, s + b, i - b) ? C_KW : in_list(types, s + b, i - b) ? C_TYPE : C_TEXT;
            while (b < i) col[b++] = c;
            continue;
        }
        col[i++] = pre ? C_PRE : C_TEXT;
    }
    return st;
}

static void update_states(void)
{
    static uint32_t *tmp;
    static int tcap;
    int i, st = 0;
    if (!hl_dirty) return;
    for (i = 0; i < nl; i++) {
        L[i].st = (unsigned char)st;
        if (L[i].len + 1 > tcap) { tcap = L[i].len + 64; tmp = (uint32_t *)realloc(tmp, sizeof(uint32_t) * (size_t)tcap); }
        if (!tmp) break;
        st = color_line(&L[i], st, tmp);
    }
    hl_dirty = 0;
}

/* ---------- মেনু ---------- */
enum {
    M_NEW = 1, M_OPEN, M_SAVE, M_SAVEAS, M_QUIT, M_NEWGAME,
    M_UNDO, M_REDO, M_CUT, M_COPY, M_PASTE, M_ALL,
    M_FIND, M_NEXT, M_REPL, M_GOTO,
    M_COMPILE, M_RUN, M_COMPILE2, M_RUN2,
    M_NUMS, M_OUT, M_F16, M_F20, M_F24,
    M_KEYS, M_ABOUT,
    M_CREF_WORD, M_CREF_INDEX                    /* ধাপ ১৯ক */
};
static const struct ui_menuitem file_items[] = {
    { "New", "Ctrl+N", M_NEW }, { "New game", NULL, M_NEWGAME }, { "Open...", "Ctrl+O", M_OPEN }, { "-", NULL, 0 },
    { "Save", "Ctrl+S", M_SAVE }, { "Save As...", "F12", M_SAVEAS }, { "-", NULL, 0 },
    { "Quit", "Ctrl+Q", M_QUIT }
};
static const struct ui_menuitem edit_items[] = {
    { "Undo", "Ctrl+Z", M_UNDO }, { "Redo", "Ctrl+Y", M_REDO }, { "-", NULL, 0 },
    { "Cut", "Ctrl+X", M_CUT }, { "Copy", "Ctrl+C", M_COPY }, { "Paste", "Ctrl+V", M_PASTE },
    { "-", NULL, 0 }, { "Select All", "Ctrl+A", M_ALL }
};
static const struct ui_menuitem search_items[] = {
    { "Find...", "Ctrl+F", M_FIND }, { "Find Next", "F3", M_NEXT },
    { "Replace...", "Ctrl+E", M_REPL }, { "-", NULL, 0 }, { "Go to Line...", "Ctrl+G", M_GOTO }
};
static const struct ui_menuitem build_items[] = {
    { "Compile", "Ctrl+B", M_COMPILE }, { "Compile and Run", "Ctrl+R", M_RUN }, { "-", NULL, 0 },
    { "Compile (F7)", "F7", M_COMPILE2 }, { "Run (F5)", "F5", M_RUN2 }
};
static struct ui_menuitem view_items[] = {
    { "[x] Line numbers", NULL, M_NUMS }, { "[ ] Output panel", "F8", M_OUT }, { "-", NULL, 0 },
    { "(o) Text size 16", NULL, M_F16 }, { "( ) Text size 20", NULL, M_F20 }, { "( ) Text size 24", NULL, M_F24 }
};
static const struct ui_menuitem help_items[] = {
    { "Keyboard shortcuts", "F1", M_KEYS },
    { "C reference: word at cursor", "Ctrl+F1", M_CREF_WORD },   /* ধাপ ১৯ক */
    { "C reference: index", "Shift+F1", M_CREF_INDEX },
    { "-", NULL, 0 }, { "About Code", NULL, M_ABOUT }
};
static const struct ui_menu menus[] = {
    { "File", file_items, 8 }, { "Edit", edit_items, 8 }, { "Search", search_items, 5 },
    { "Build", build_items, 5 }, { "View", view_items, 6 }, { "Help", help_items, 5 }
};

static void set_font(int px)
{
    char p[80];
    gfx_font *f;
    snprintf(p, sizeof(p), "/usr/share/fonts/rozos-mono-%d.psf", px);
    f = px == 16 ? gfx_font_default() : gfx_font_load(p);
    if (!f) { snprintf(status_msg, sizeof(status_msg), "Font not found: %s", p); return; }
    font = f;
    font_px = px;
    cw = gfx_font_width(f);
    chh = gfx_font_height(f);
    follow_cursor = 1;
}

static void menu_labels(void)
{
    view_items[0].label = show_numbers ? "[x] Line numbers" : "[ ] Line numbers";
    view_items[1].label = show_output ? "[x] Output panel" : "[ ] Output panel";
    view_items[3].label = font_px == 16 ? "(o) Text size 16" : "( ) Text size 16";
    view_items[4].label = font_px == 20 ? "(o) Text size 20" : "( ) Text size 20";
    view_items[5].label = font_px == 24 ? "(o) Text size 24" : "( ) Text size 24";
}

/* ধাপ ১৯ক: C সহায়িকা (crefview) — word = 1 হলে কার্সরের শব্দের পাতা */
static void cref_open(int word)
{
    char w[64] = "";
    const char *argv[3];
    if (word && cy >= 0 && cy < nl) {
        const struct line *l = &L[cy];
        int a = cx, b;
        if (a > l->len) a = l->len;
        if (a > 0 && (a == l->len || !(isalnum((unsigned char)l->s[a]) || l->s[a] == '_')) &&
            (isalnum((unsigned char)l->s[a - 1]) || l->s[a - 1] == '_')) a--;
        while (a > 0 && (isalnum((unsigned char)l->s[a - 1]) || l->s[a - 1] == '_')) a--;
        b = a;
        while (b < l->len && (isalnum((unsigned char)l->s[b]) || l->s[b] == '_')) b++;
        if (b > a && (size_t)(b - a) < sizeof(w)) {
            memcpy(w, l->s + a, (size_t)(b - a));
            w[b - a] = 0;
        }
    }
    argv[0] = "crefview";
    argv[1] = w[0] ? w : NULL;
    argv[2] = NULL;
    if (ui_launch(argv, NULL) < 0) ui_msgbox("Code", "Cannot start crefview", UI_MB_OK | UI_MB_ERROR);
    else if (word && !w[0]) snprintf(status_msg, sizeof(status_msg), "Put the cursor on a word (like printf) for Ctrl+F1");
}

static void do_action(int id)
{
    switch (id) {
    case M_NEW: new_file(); break;
    case M_NEWGAME: new_game_file(); break;
    case M_OPEN: open_file(); break;
    case M_SAVE: save_file(); break;
    case M_SAVEAS: save_as(); break;
    case M_QUIT: if (confirm_discard()) ui_quit(&U); break;
    case M_UNDO: do_undo(0); break;
    case M_REDO: do_undo(1); break;
    case M_CUT: copy_sel(1); break;
    case M_COPY: copy_sel(0); break;
    case M_PASTE: paste(); break;
    case M_ALL: sel_on = 1; sx = 0; sy = 0; cy = nl - 1; cx = L[cy].len; break;
    case M_FIND: do_find(); break;
    case M_NEXT: if (last_find[0]) find_next(last_find, cy, cx); else do_find(); break;
    case M_REPL: do_replace(); break;
    case M_GOTO: do_goto(); break;
    case M_COMPILE: case M_COMPILE2: compile(); break;
    case M_RUN: case M_RUN2: run_program(); break;
    case M_NUMS: show_numbers = !show_numbers; break;
    case M_OUT: show_output = !show_output; break;
    case M_F16: set_font(16); break;
    case M_F20: set_font(20); break;
    case M_F24: set_font(24); break;
    case M_KEYS:
        ui_msgbox("Keyboard shortcuts",
                  "Ctrl+N new   Ctrl+O open   Ctrl+S save   F12 save as   Ctrl+Q quit\n"
                  "Ctrl+Z undo  Ctrl+Y redo   Ctrl+X/C/V cut/copy/paste   Ctrl+A all\n"
                  "Ctrl+F find  F3 find next  Ctrl+E replace   Ctrl+G go to line\n"
                  "Ctrl+B or F7 compile        Ctrl+R or F5 compile and run\n"
                  "F8 output panel   F10 menu   Shift+arrows select\n"
                  "Ctrl+F1 C help for the word   Shift+F1 C reference index\n"
                  "Ctrl+Left/Right word   Ctrl+Home/End start/end of file\n"
                  "Mouse: click, drag to select, double-click a word, wheel to scroll",
                  UI_MB_OK | UI_MB_INFO);
        break;
    case M_CREF_WORD: cref_open(1); break;
    case M_CREF_INDEX: cref_open(0); break;
    case M_ABOUT:
        ui_msgbox("About Code",
                  "Code - the rozOS C editor (phase 14b)\n"
                  "Built with rozui widgets; compiles with TinyCC.\n"
                  "Source: /usr/src/gfx/code.c",
                  UI_MB_OK | UI_MB_INFO);
        break;
    }
}

/* ---------- কী ---------- */
static int is_word(int c) { return isalnum(c) || c == '_'; }

static void move_word(int dir)
{
    if (dir < 0) {
        if (cx == 0) { if (cy > 0) { cy--; cx = L[cy].len; } return; }
        while (cx > 0 && !is_word((unsigned char)L[cy].s[cx - 1])) cx--;
        while (cx > 0 && is_word((unsigned char)L[cy].s[cx - 1])) cx--;
    } else {
        if (cx == L[cy].len) { if (cy + 1 < nl) { cy++; cx = 0; } return; }
        while (cx < L[cy].len && !is_word((unsigned char)L[cy].s[cx])) cx++;
        while (cx < L[cy].len && is_word((unsigned char)L[cy].s[cx])) cx++;
    }
}

static void handle_key(int k, int mods, int page)
{
    int shift = mods & GFX_MOD_SHIFT, ctrl = mods & GFX_MOD_CTRL;
    int nav = k >= GFX_KEY_UP && k <= GFX_KEY_PAGE_DOWN;
    if (nav) {
        if (shift && !sel_on) { sel_on = 1; sx = cx; sy = cy; }
        if (!shift) sel_on = 0;
        typing_run = 0;
        if (k != GFX_KEY_UP && k != GFX_KEY_DOWN && k != GFX_KEY_PAGE_UP && k != GFX_KEY_PAGE_DOWN) want_col = -1;
        else if (want_col < 0) want_col = render_x(&L[cy], cx);
        switch (k) {
        case GFX_KEY_LEFT:
            if (ctrl) move_word(-1);
            else if (cx > 0) { cx--; while (cx > 0 && CONT(L[cy].s[cx])) cx--; }
            else if (cy > 0) { cy--; cx = L[cy].len; }
            break;
        case GFX_KEY_RIGHT:
            if (ctrl) move_word(1);
            else if (cx < L[cy].len) { cx++; while (cx < L[cy].len && CONT(L[cy].s[cx])) cx++; }
            else if (cy + 1 < nl) { cy++; cx = 0; }
            break;
        case GFX_KEY_UP: if (cy > 0) cy--; cx = byte_at_render(&L[cy], want_col); break;
        case GFX_KEY_DOWN: if (cy + 1 < nl) cy++; cx = byte_at_render(&L[cy], want_col); break;
        case GFX_KEY_PAGE_UP: cy -= page; top -= page; if (cy < 0) cy = 0; cx = byte_at_render(&L[cy], want_col); break;
        case GFX_KEY_PAGE_DOWN: cy += page; top += page; if (cy >= nl) cy = nl - 1; cx = byte_at_render(&L[cy], want_col); break;
        case GFX_KEY_HOME: {
            int ind = 0;
            if (ctrl) { cy = 0; cx = 0; break; }
            while (ind < L[cy].len && (L[cy].s[ind] == ' ' || L[cy].s[ind] == '\t')) ind++;
            cx = cx == ind ? 0 : ind;                 /* প্রথমে লেখার শুরুতে, আবার চাপলে একদম বাঁয়ে */
            break;
        }
        case GFX_KEY_END:
            if (ctrl) cy = nl - 1;
            cx = L[cy].len;
            break;
        }
        clamp_cursor();
        follow_cursor = 1;
        return;
    }
    want_col = -1;
    switch (k) {
    case 2: do_action(M_COMPILE); return;            /* Ctrl+B / Ctrl+R — মেনুতেও আছে */
    case 18: do_action(M_RUN); return;
    case GFX_KEY_ESC: sel_on = 0; return;
    case GFX_KEY_F8: show_output = !show_output; return;
    case '\n': {
        int ind = 0;
        char pad[128];
        undo_point(0);
        delete_selection();
        while (ind < L[cy].len && ind < 120 && ind < cx && (L[cy].s[ind] == ' ' || L[cy].s[ind] == '\t')) {
            pad[ind] = L[cy].s[ind];
            ind++;
        }
        if (cx > 0 && L[cy].s[cx - 1] == '{' && ind < 120) {        /* { পরে এক ধাপ ভেতরে */
            if (makefile_mode) pad[ind++] = '\t';
            else { int t; for (t = 0; t < TABW && ind < 124; t++) pad[ind++] = ' '; }
        }
        insert_text("\n", 1);
        insert_text(pad, ind);
        break;
    }
    case '\b':
        undo_point(1);
        if (!delete_selection()) {
            if (cx > 0) {
                int n = 1;
                /* শুধু স্পেসের ইনডেন্টে এক ধাপ (৪টা) একসাথে মোছা */
                if (cx % TABW == 0) {
                    int i, all = 1;
                    for (i = 0; i < cx; i++) if (L[cy].s[i] != ' ') all = 0;
                    if (all && cx >= TABW) n = TABW;
                }
                while (n < cx && CONT(L[cy].s[cx - n])) n++;     /* পুরো UTF-8 অক্ষর */
                delete_range(cy, cx - n, cy, cx);
            } else if (cy > 0) {
                int px = L[cy - 1].len;
                delete_range(cy - 1, px, cy, 0);
            }
        }
        break;
    case GFX_KEY_DELETE:
        undo_point(0);
        if (!delete_selection()) {
            if (cx < L[cy].len) {
                int e = cx + 1;
                while (e < L[cy].len && CONT(L[cy].s[e])) e++;
                delete_range(cy, cx, cy, e);
            }
            else if (cy + 1 < nl) delete_range(cy, cx, cy + 1, 0);
        }
        break;
    case '\t':
        undo_point(1);
        delete_selection();
        if (makefile_mode) insert_text("\t", 1);
        else insert_text("    ", TABW - render_x(&L[cy], cx) % TABW);
        break;
    default:
        if (k >= 32 && k < 127) {
            char c = (char)k;
            undo_point(1);
            delete_selection();
            if (c == '}') {                              /* শুধু ফাঁকার পরে } = এক ধাপ বাঁয়ে */
                int i, all = 1;
                for (i = 0; i < cx; i++) if (L[cy].s[i] != ' ') all = 0;
                if (all && cx >= TABW) delete_range(cy, cx - TABW, cy, cx);
            }
            insert_text(&c, 1);
        } else {
            return;
        }
        break;
    }
    if (k != '\b' && !(k >= 32 && k < 127) && k != '\t') typing_run = 0;
    follow_cursor = 1;
}

/* ---------- আঁকা ---------- */
/* gfx_glyph অক্ষরের পেছনও আঁকে — তাই জায়গার রং (বাছাই / চলতি লাইন / সাধারণ) দিয়ে */
static uint32_t field_bg(int curline, int sel)
{
    const struct ui_theme *t = ui_theme_get();
    if (sel) return t->select;
    return curline ? GFX_RGB(20, 30, 34) : t->field;
}

static void draw_editor(int ex, int ey, int ew, int eh, int gut)
{
    const struct ui_theme *t = ui_theme_get();
    gfx_surface *s = U.s;
    static uint32_t *col;
    static int ccap;
    int rows = eh / chh + 1, r, y1 = 0, x1 = 0, y2 = -1, x2 = 0, has = sel_range(&y1, &x1, &y2, &x2);
    update_states();
    ui_sunken(s, ex, ey, ew, eh, t->field);
    if (gut) gfx_fill_rect(s, ex + 2, ey + 2, gut, eh - 4, GFX_RGB(22, 26, 28));
    gfx_set_clip(s, ex + 2, ey + 2, ew - 4, eh - 4);
    for (r = 0; r < rows && top + r < nl; r++) {
        int ln = top + r, y = ey + 2 + r * chh, i, rx = 0, x0 = ex + 2 + gut + 6;
        struct line *l = &L[ln];
        if (ln == cy && !has) gfx_fill_rect(s, x0 - 4, y, ew - gut - 6, chh, GFX_RGB(20, 30, 34));
        if (gut) {
            char num[16];
            snprintf(num, sizeof(num), "%*d", gut / cw - 1, ln + 1);
            gfx_text(s, font, ex + 2, y, num, ln == cy ? GFX_WHITE : GFX_RGB(110, 116, 106));
        }
        if (l->len + 1 > ccap) { ccap = l->len + 64; col = (uint32_t *)realloc(col, sizeof(uint32_t) * (size_t)ccap); }
        if (!col) break;
        color_line(l, l->st, col);
        gfx_set_clip(s, x0 - 2, ey + 2, ew - gut - 8, eh - 4);
        for (i = 0; i <= l->len; i++) {
            int w = (i < l->len && l->s[i] == '\t') ? TABW - rx % TABW : (i < l->len && CONT(l->s[i])) ? 0 : 1;
            int sel = 0, px = x0 + (rx - left) * cw;
            if (has && ((ln > y1 || (ln == y1 && i >= x1)) && (ln < y2 || (ln == y2 && i < x2)))) sel = 1;
            if (has && i == l->len && ln >= y1 && ln < y2) sel = 1;        /* লাইনের শেষ ভাঙনও বাছাইয়ে */
            if (rx + w > left && px < ex + ew) {
                if (sel) gfx_fill_rect(s, px, y, w * cw, chh, t->select);
                if (i < l->len && l->s[i] != '\t' && l->s[i] != ' ' && w > 0) {
                    unsigned char g = (unsigned char)l->s[i];
                    if (g >= 0x80) g = 0xF9;                 /* UTF-8 অক্ষর: বিন্দু */
                    gfx_glyph(s, font, px, y, g, col[i], field_bg(ln == cy && !has, sel));
                }
            }
            rx += w;
        }
        /* কার্সর: ২ পিক্সেলের সাদা দাগ, ঝলকায় */
        if (ln == cy && U.blink) {
            int px = x0 + (render_x(l, cx) - left) * cw;
            gfx_fill_rect(s, px - 1, y, 2, chh, GFX_WHITE);
        }
        gfx_set_clip(s, ex + 2, ey + 2, ew - 4, eh - 4);
    }
    gfx_reset_clip(s);
}


static void frame(void)
{
    const struct ui_theme *t = ui_theme_get();
    gfx_surface *s;
    int act, W, H, ey, eh, ew, gut, outh, page, i;
    menu_labels();
    /* ধাপ ১৯ক: Ctrl+F1 / Shift+F1 — মেনুর "F1"-এর আগে ধরা */
    if (U.key == GFX_KEY_F1 && (U.mods & (GFX_MOD_CTRL | GFX_MOD_SHIFT))) {
        cref_open((U.mods & GFX_MOD_CTRL) != 0);
        U.key = 0;
    }
    act = ui_menubar(&U, 0, 0, U.w, menus, 6);
    if (act) do_action(act);
    s = U.s;
    W = U.w;
    H = U.h;
    /* টুলবার */
    ui_panel(s, 0, UI_MENUBAR_H, W, TOOL_H, t->face, 1);
    {
        int x = 4, y = UI_MENUBAR_H + 3, h = TOOL_H - 6;
        if (ui_do_tool(&U, x, y, 64, h, "new", 'd', "New")) do_action(M_NEW);
        x += 66;
        if (ui_do_tool(&U, x, y, 70, h, "open", 'f', "Open")) do_action(M_OPEN);
        x += 72;
        if (ui_do_tool(&U, x, y, 70, h, "save", 'v', "Save")) do_action(M_SAVE);
        x += 76;
        gfx_vline(s, x, y + 2, h - 4, t->dark); gfx_vline(s, x + 1, y + 2, h - 4, t->light2);
        x += 6;
        if (ui_do_tool(&U, x, y, 90, h, "compile", 'w', "Compile")) do_action(M_COMPILE);
        x += 92;
        if (ui_do_tool(&U, x, y, 64, h, "run", 'y', "Run")) do_action(M_RUN);
        x += 70;
        gfx_vline(s, x, y + 2, h - 4, t->dark); gfx_vline(s, x + 1, y + 2, h - 4, t->light2);
        x += 6;
        if (ui_do_tool(&U, x, y, 70, h, "find", 'e', "Find")) do_action(M_FIND);
    }
    /* মাপ — প্রতি ফ্রেমে উইন্ডো থেকে (বড় করলে সব নিজে বসে যায়) */
    ey = UI_MENUBAR_H + TOOL_H;
    outh = show_output ? OUT_ROWS * 20 + 4 + 22 : 0;
    eh = H - ey - STATUS_H - outh;
    if (eh < chh * 3) { eh = chh * 3; }
    ew = W - 16;
    gut = 0;
    if (show_numbers) {
        int d = 1, n = nl;
        while (n >= 10) { n /= 10; d++; }
        if (d < 3) d = 3;
        gut = (d + 1) * cw;
    }
    page = (eh - 4) / chh;
    if (page < 1) page = 1;
    /* কী */
    if (U.key) {
        handle_key(U.key, U.mods, page);
        U.key = 0;
    }
    /* মাউস — লেখার জায়গায় */
    {
        int x0 = 2 + gut + 6, over = ui_hover(&U, 0, ey, ew, eh);
        if (over && U.wheel) top += U.wheel * 3;
        if ((over && U.pressed) || (dragging && (U.buttons & GFX_BUTTON_LEFT))) {
            int ln = top + (U.my - ey - 2) / chh, colr;
            if (U.my < ey + 2 && dragging) ln = top - 1;
            if (ln >= nl) ln = nl - 1;
            if (ln < 0) ln = 0;
            colr = (U.mx - x0) / cw + left;
            if (colr < 0) colr = 0;
            if (U.pressed) {
                cy = ln;
                cx = byte_at_render(&L[ln], colr);
                sel_on = 1; sx = cx; sy = cy;
                dragging = 1;
                typing_run = 0;
                want_col = -1;
                if (U.dclick) {                        /* শব্দ বাছাই */
                    int a = cx, b = cx;
                    while (a > 0 && is_word((unsigned char)L[cy].s[a - 1])) a--;
                    while (b < L[cy].len && is_word((unsigned char)L[cy].s[b])) b++;
                    sx = a; sy = cy; cx = b;
                    dragging = 0;
                }
            } else {
                cy = ln;
                cx = byte_at_render(&L[ln], colr);
                follow_cursor = 1;
            }
        }
        if (!(U.buttons & GFX_BUTTON_LEFT)) dragging = 0;
    }
    /* কার্সর দেখা যায় এমন স্ক্রল */
    if (follow_cursor) {
        int vis_cols = (ew - gut - 12) / cw, rx = render_x(&L[cy], cx);
        if (cy < top) top = cy;
        if (cy >= top + page) top = cy - page + 1;
        if (rx < left) left = rx;
        if (vis_cols > 4 && rx >= left + vis_cols) left = rx - vis_cols + 1;
        follow_cursor = 0;
    }
    if (top > nl - 1) top = nl - 1;
    if (top < 0) top = 0;
    draw_editor(0, ey, ew, eh, gut);
    ui_do_vscroll(&U, ew, ey, 16, eh, &top, nl + page - 1, page);
    /* Output প্যানেল */
    if (show_output) {
        int oy = ey + eh, r;
        ui_panel(s, 0, oy, W, 22, t->face, 1);
        gfx_text(s, NULL, 8, oy + 3, "Output", t->text);
        if (ui_do_tool(&U, W - 26, oy + 2, 20, 18, NULL, 'x', NULL)) show_output = 0;
        r = ui_do_list(&U, 0, oy + 22, W, outh - 22, (const char *const *)outl, nout, &out_sel, &out_top);
        if (r && out_sel >= 0 && out_sel < nout) jump_to_error(outl[out_sel]);
    }
    /* অবস্থা-লাইন */
    {
        char st[256];
        int y1, x1, y2, x2;
        if (sel_range(&y1, &x1, &y2, &x2))
            snprintf(st, sizeof(st), "Ln %d, Col %d  |  %d lines selected  |  %s", cy + 1, render_x(&L[cy], cx) + 1,
                     y2 - y1 + 1, status_msg);
        else
            snprintf(st, sizeof(st), "Ln %d, Col %d  |  %d lines  |  %s%s", cy + 1, render_x(&L[cy], cx) + 1, nl,
                     modified ? "modified  |  " : "", status_msg);
        ui_statusbar(s, 0, H - STATUS_H, W, STATUS_H, st);
    }
    (void)i;
}

int main(int argc, char **argv)
{
    int was_modified = -1, was_untitled = -1;
    char was_path[512] = "";
    if (gfx_init_window("Code", 960, 660) < 0) {
        perror("code: no screen");
        return 1;
    }
    font = gfx_font_default();
    cw = gfx_font_width(font);
    chh = gfx_font_height(font);
    if (argc > 1) {
        if (load_file(argv[1]) < 0) {           /* নেই = এই নামে নতুন ফাইল */
            clear_all();
            insert_line(0, "", 0);
            set_path(argv[1]);
            untitled = 0;
            snprintf(status_msg, sizeof(status_msg), "New file %s", base_name(path));
        }
    } else {
        char cwd[256];
        insert_line(0, "", 0);
        if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "/home");
        snprintf(path, sizeof(path), "%s/untitled.c", strcmp(cwd, "/") ? cwd : "");
        untitled = 1;
        snprintf(status_msg, sizeof(status_msg), "F10 or click the menu - Ctrl+S saves, F5 runs");
    }
    ui_init(&U);
    ui_ask_before_close(&U, 1);
    U.tick_ms = 500;                             /* কার্সর ঝলকানো */
    while (ui_run(&U)) {
        if (U.closing) {
            if (confirm_discard()) { ui_quit(&U); continue; }
        }
        frame();
        if (modified != was_modified || untitled != was_untitled || strcmp(path, was_path)) {
            was_modified = modified;
            was_untitled = untitled;
            snprintf(was_path, sizeof(was_path), "%s", path);
            update_title();
        }
    }
    gfx_quit();
    return 0;
}
