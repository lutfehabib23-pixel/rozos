/* rozOS — crefview: C সহায়িকার উইন্ডো, Turbo C-র help-এর মতো (ধাপ ১৯ক)
 *
 *   crefview            সূচি (ভাষার বিষয়, তারপর হেডার অনুযায়ী সব ফাংশন)
 *   crefview strcpy     সোজা সেই পাতা (Code আর edit-এর Ctrl+F1 এভাবেই খোলে)
 *
 * বাঁয়ে তালিকা আর খোঁজার ঘর, ডানে পাতা। "See also"-এর নামে ক্লিক করলে সেই পাতা, Back ফেরায়।
 * Copy = উদাহরণ ক্লিপবোর্ডে (Code/edit/rozterm-এ পেস্ট), Try it = উদাহরণ Code-এ খোলে (F5 চালায়)।
 * ধাপ ১৯গ: Lua game-এর পাতায় Try it সরাসরি luagame দিয়ে চালায়। */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <rozui.h>
#include <rozcref.h>

#define MAXITEMS 2048
#define MAXLINES 4000
#define LIST_W 270
#define TOOL_H 36
#define STATUS_H 22

enum { L_TEXT, L_TITLE, L_HEAD, L_SYNTAX, L_CODE, L_LINKS, L_GAP };

typedef struct { int kind; char *text; } dline;

static ui_ctx U;
static char search[64], last_search[64] = "\x01";
static const char *items[MAXITEMS];
static const cref_entry *item_ent[MAXITEMS];
static const char *item_hdr[MAXITEMS];          /* হেডারের সারি হলে হেডারের নাম */
static char *item_buf[MAXITEMS];
static int nitems, sel = -1, list_top;
static char last_built_search;                  /* আগের তালিকা খোঁজার ফল ছিল কিনা */
static const cref_entry *cur;                   /* দেখানো পাতা (NULL = হেডারের সূচি/স্বাগত) */
static const char *cur_hdr;
static dline lines[MAXLINES];
static int nlines, scroll, wrap_cols;
static const char *back[64];
static int nback;
static char status[160];
static gfx_font *mono;

/* ---------- তালিকা ---------- */

static void free_items(void)
{
    int i;
    for (i = 0; i < nitems; i++) free(item_buf[i]);
    nitems = 0;
}

static void add_item(const char *label, const cref_entry *e, const char *hdr)
{
    if (nitems >= MAXITEMS) return;
    item_buf[nitems] = strdup(label);
    items[nitems] = item_buf[nitems] ? item_buf[nitems] : "";
    item_ent[nitems] = e;
    item_hdr[nitems] = hdr;
    nitems++;
}

static void build_items(void)
{
    char b[96];
    int i, old_top = list_top, same = !search[0] == !last_built_search;
    free_items();
    if (search[0]) {
        static const cref_entry *hits[MAXITEMS];
        int n = cref_search(search, hits, MAXITEMS);
        for (i = 0; i < n; i++) {
            snprintf(b, sizeof(b), "%s", hits[i]->name);
            add_item(b, hits[i], NULL);
        }
    } else {
        const char *prev = NULL;
        for (i = 0; i < cref_count(); i++) {
            const cref_entry *e = cref_at(i);
            if (!prev || strcmp(prev, e->header)) {
                snprintf(b, sizeof(b), "[ %s ]", strcmp(e->header, "language") ? e->header : "C language");
                add_item(b, NULL, e->header);
                prev = e->header;
            }
            snprintf(b, sizeof(b), "   %s", e->name);
            add_item(b, e, NULL);
        }
    }
    sel = -1;
    list_top = same ? old_top : 0;
    last_built_search = search[0];
    for (i = 0; i < nitems; i++)
        if ((cur && item_ent[i] == cur) || (!cur && cur_hdr && item_hdr[i] == cur_hdr)) { sel = i; break; }
    /* বেছে নেওয়াটা তালিকায় দেখা না গেলে সেখানে সরানো (প্রায় ২৫ সারি দেখা যায়) */
    if (sel >= 0 && (sel < list_top || sel > list_top + 24)) list_top = sel > 8 ? sel - 8 : 0;
}

/* ---------- পাতা তৈরি ---------- */

static void clear_lines(void)
{
    int i;
    for (i = 0; i < nlines; i++) free(lines[i].text);
    nlines = 0;
}

static void add_line(int kind, const char *s, size_t n)
{
    if (nlines >= MAXLINES) return;
    lines[nlines].kind = kind;
    lines[nlines].text = (char *)malloc(n + 1);
    if (lines[nlines].text) {
        memcpy(lines[nlines].text, s, n);
        lines[nlines].text[n] = 0;
    }
    nlines++;
}

/* লেখা ভেঙে লাইনে (স্পেস দিয়ে শুরু লাইন — কোড/তালিকা — ভাঙা হয় না) */
static void add_wrapped(int kind, const char *text, int indent)
{
    const char *p = text;
    char pad[16];
    int cols = wrap_cols - indent;
    memset(pad, ' ', sizeof(pad));
    if (indent > 15) indent = 15;
    if (cols < 20) cols = 20;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char buf[600];
        if (len == 0) add_line(kind == L_CODE ? L_CODE : L_GAP, "", 0);   /* কোডের ফাঁকা লাইনেও পটভূমি */
        else if (p[0] == ' ' || (int)len <= cols) {
            size_t n = len < sizeof(buf) - 20 ? len : sizeof(buf) - 20;
            memcpy(buf, pad, (size_t)indent);
            memcpy(buf + indent, p, n);
            add_line(kind, buf, n + (size_t)indent);
        } else {
            const char *w = p, *end = p + len;
            while (w < end) {
                const char *brk = w + cols < end ? w + cols : end, *q;
                if (brk < end) {
                    for (q = brk; q > w && *q != ' '; q--) {
                    }
                    if (q > w) brk = q;
                }
                memcpy(buf, pad, (size_t)indent);
                memcpy(buf + indent, w, (size_t)(brk - w));
                add_line(kind, buf, (size_t)(brk - w) + (size_t)indent);
                w = brk;
                while (w < end && *w == ' ') w++;
            }
        }
        p = eol ? eol + 1 : p + len;
    }
}

static void build_page(void)
{
    char b[256];
    int i;
    clear_lines();
    scroll = 0;
    if (cur) {
        const cref_entry *e = cur;
        add_line(L_TITLE, e->name, strlen(e->name));
        snprintf(b, sizeof(b), strcmp(e->header, "language") ? "#include <%s>" : "C language", e->header);
        add_line(L_TEXT, b, strlen(b));
        if (*e->summary) add_wrapped(L_TEXT, e->summary, 0);
        if (*e->also) {
            snprintf(b, sizeof(b), "Also: %s", e->also);
            add_wrapped(L_TEXT, b, 0);
        }
        if (*e->syntax) {
            add_line(L_GAP, "", 0);
            add_line(L_HEAD, "Syntax", 6);
            add_wrapped(L_SYNTAX, e->syntax, 2);
        }
        if (*e->desc) {
            add_line(L_GAP, "", 0);
            add_line(L_HEAD, "Description", 11);
            add_wrapped(L_TEXT, e->desc, 0);
        }
        if (*e->returns) {
            add_line(L_GAP, "", 0);
            add_line(L_HEAD, "Returns", 7);
            add_wrapped(L_TEXT, e->returns, 0);
        }
        if (*e->see) {
            add_line(L_GAP, "", 0);
            add_line(L_HEAD, "See also", 8);
            add_line(L_LINKS, e->see, strlen(e->see));
        }
        if (*e->example) {
            add_line(L_GAP, "", 0);
            add_line(L_HEAD, "Example  (Copy, or Try it - then F5 in Code)", 44);
            add_wrapped(L_CODE, e->example, 1);
        }
    } else if (cur_hdr) {
        snprintf(b, sizeof(b), strcmp(cur_hdr, "language") ? "<%s>" : "The C language", cur_hdr);
        add_line(L_TITLE, b, strlen(b));
        add_line(L_TEXT, "Click a name below, or pick one in the list.", 44);
        add_line(L_GAP, "", 0);
        for (i = 0; i < cref_count(); i++) {
            const cref_entry *e = cref_at(i);
            if (strcmp(e->header, cur_hdr)) continue;
            add_line(L_LINKS, e->name, strlen(e->name));
            if (*e->summary) add_wrapped(L_TEXT, e->summary, 4);
        }
    } else {
        add_line(L_TITLE, "C reference", 11);
        add_wrapped(L_TEXT,
                    "Every function of the C library in rozOS with its syntax, what it does, what it returns "
                    "and a small program that uses it - like the help of Turbo C.\n\n"
                    "  - pick a name in the list on the left, or type in the search box\n"
                    "  - In Code and edit: Ctrl+F1 on a word opens its page, Shift+F1 this index\n"
                    "  - Copy puts the example on the clipboard; Try it opens it in Code (F5 runs it)\n"
                    "  - In a terminal: cref printf, cref -k string, cref -x printf > p.c\n\n"
                    "Start with the C language topics at the top of the list.", 0);
    }
}

static void open_entry(const cref_entry *e, int remember)
{
    if (!e) return;
    if (remember && (cur || cur_hdr) && nback < 64) back[nback++] = cur ? cur->name : cur_hdr;
    cur = e;
    cur_hdr = NULL;
    build_page();
    build_items();
    status[0] = 0;
}

static void open_header(const char *h, int remember)
{
    if (remember && (cur || cur_hdr) && nback < 64) back[nback++] = cur ? cur->name : cur_hdr;
    cur = NULL;
    cur_hdr = h;
    build_page();
    build_items();
}

static void go_back(void)
{
    const char *n;
    const cref_entry *e;
    int i;
    if (nback == 0) return;
    n = back[--nback];
    e = cref_find(n);
    if (e && !strcmp(e->name, n)) { open_entry(e, 0); return; }
    for (i = 0; i < cref_count(); i++)
        if (!strcmp(cref_at(i)->header, n)) { open_header(cref_at(i)->header, 0); return; }
}

static void try_it(void)
{
    char path[128], name[48];
    const char *argv[3];
    FILE *f;
    int i, j = 0, lua;
    if (!cur || !*cur->example) return;
    for (i = 0; cur->name[i] && j < (int)sizeof(name) - 1; i++)
        name[j++] = isalnum((unsigned char)cur->name[i]) ? cur->name[i] : '_';
    name[j] = 0;
    /* ধাপ ১৯গ: Lua-র game মডিউলের পাতা (header "lua game") — .lua ফাইল, luagame দিয়ে চালানো */
    lua = !strcmp(cur->header, "lua game");
    snprintf(path, sizeof(path), "/tmp/cref-%s.%s", name, lua ? "lua" : "c");
    f = fopen(path, "w");
    if (!f) {
        snprintf(status, sizeof(status), "cannot write %s", path);
        return;
    }
    fprintf(f, "%s\n", cur->example);
    fclose(f);
    if (lua) {
        argv[0] = "luagame";
        argv[1] = path;
        argv[2] = NULL;
        ui_launch(argv, "/tmp");
        snprintf(status, sizeof(status), "Running %s with luagame (edit it in Code, run: luagame %s)", path, path);
        return;
    }
    argv[0] = "code";
    argv[1] = path;
    argv[2] = NULL;
    ui_launch(argv, "/tmp");
    snprintf(status, sizeof(status), "Opened %s in Code - press F5 to compile and run", path);
}

/* ---------- আঁকা ---------- */

static void draw_page(int x, int y, int w, int h)
{
    const struct ui_theme *T = ui_theme_get();
    int lh = gfx_font_height(mono) + 2, cw = gfx_font_width(mono), rows = (h - 12) / lh, i, yy;
    int cols = (w - 34) / cw;
    if (cols != wrap_cols && cols > 20) {
        wrap_cols = cols;
        build_page();
    }
    ui_sunken(U.s, x, y, w, h, T->field);
    if (U.wheel && ui_hover(&U, x, y, w, h)) scroll += U.wheel * 3;
    if (scroll > nlines - rows) scroll = nlines - rows;
    if (scroll < 0) scroll = 0;
    gfx_set_clip(U.s, x + 2, y + 2, w - 20, h - 4);
    yy = y + 8;
    for (i = scroll; i < nlines && yy < y + h - lh; i++, yy += lh) {
        dline *l = &lines[i];
        int tx = x + 12;
        switch (l->kind) {
        case L_TITLE:
            gfx_fill_rect(U.s, x + 2, yy - 4, w - 22, lh + 4, T->select);
            gfx_text(U.s, mono, tx, yy, l->text, GFX_RGB(255, 230, 140));
            break;
        case L_HEAD:
            gfx_text(U.s, mono, tx, yy, l->text, GFX_RGB(120, 200, 255));
            gfx_hline(U.s, tx, yy + lh - 2, (int)strlen(l->text) * cw, GFX_RGB(70, 120, 170));
            break;
        case L_SYNTAX:
            gfx_text(U.s, mono, tx, yy, l->text, GFX_RGB(255, 220, 120));
            break;
        case L_CODE:
            gfx_fill_rect(U.s, x + 6, yy - 1, w - 30, lh, GFX_RGB(16, 22, 28));
            gfx_text(U.s, mono, tx, yy, l->text, GFX_RGB(170, 235, 170));
            break;
        case L_LINKS: {
            /* প্রতিটা শব্দ আলাদা লিংক */
            char word[64];
            const char *p = l->text;
            int lx = tx;
            while (*p) {
                int n = 0;
                while (*p == ' ') p++;
                while (*p && *p != ' ' && n < 63) word[n++] = *p++;
                word[n] = 0;
                if (!n) break;
                {
                    int ww = n * cw, hov = ui_hover(&U, lx, yy, ww, lh);
                    gfx_text(U.s, mono, lx, yy, word, hov ? GFX_RGB(255, 255, 255) : GFX_RGB(110, 190, 255));
                    gfx_hline(U.s, lx, yy + lh - 3, ww, GFX_RGB(80, 140, 220));
                    if (ui_clicked(&U, lx, yy, ww, lh)) {
                        const cref_entry *e = cref_find(word);
                        if (e) {
                            gfx_reset_clip(U.s);
                            open_entry(e, 1);
                            return;
                        }
                        snprintf(status, sizeof(status), "no page for '%s'", word);
                    }
                    lx += ww + 2 * cw;
                }
            }
            break;
        }
        case L_GAP:
            break;
        default:
            gfx_text(U.s, mono, tx, yy, l->text, T->field_text);
            break;
        }
    }
    gfx_reset_clip(U.s);
    if (nlines > rows) ui_do_vscroll(&U, x + w - 18, y + 2, 16, h - 4, &scroll, nlines, rows);
    /* কী: পাতা স্ক্রল (খোঁজার ঘরে লেখার সময় নয়) */
    if (U.key == GFX_KEY_PAGE_DOWN) { scroll += rows - 2; U.key = 0; }
    else if (U.key == GFX_KEY_PAGE_UP) { scroll -= rows - 2; U.key = 0; }
}

static void frame(void)
{
    const struct ui_theme *T = ui_theme_get();
    int W = U.w, H = U.h, x, r, listh;
    gfx_clear(U.s, T->window_bg);
    if (strcmp(search, last_search)) {
        snprintf(last_search, sizeof(last_search), "%s", search);
        build_items();
    }

    /* টুলবার */
    ui_panel(U.s, 0, 0, W, TOOL_H, T->face, 1);
    ui_label(U.s, 10, 10, "Find:", T->text);
    r = ui_do_textbox(&U, 52, 6, LIST_W - 60, search, sizeof(search));
    if (r == 2) {                                        /* Enter: প্রথম মিল খোলা */
        const cref_entry *e = cref_find(search), *hits[1];
        if (!e && cref_search(search, hits, 1) == 1) e = hits[0];
        if (e) open_entry(e, 1);
        else snprintf(status, sizeof(status), "nothing found for '%s'", search);
    }
    x = LIST_W + 8;
    if (ui_do_button(&U, x, 5, 70, 26, "Back") || (U.key == GFX_KEY_LEFT && (U.mods & GFX_MOD_ALT))) go_back();
    x += 76;
    if (ui_do_button(&U, x, 5, 70, 26, "Index")) {
        search[0] = 0;
        cur = NULL;
        cur_hdr = NULL;
        nback = 0;
        build_page();
        build_items();
    }
    x += 76;
    if (ui_do_button(&U, x, 5, 120, 26, "Copy example") && cur && *cur->example) {
        ui_clip_set(cur->example, (int)strlen(cur->example));
        snprintf(status, sizeof(status), "Example copied - paste it with Ctrl+V in Code or edit");
    }
    x += 126;
    if (ui_do_button(&U, x, 5, 80, 26, "Try it")) try_it();
    x += 86;
    if (ui_do_button(&U, x, 5, 110, 26, "Copy syntax") && cur && *cur->syntax) {
        ui_clip_set(cur->syntax, (int)strlen(cur->syntax));
        snprintf(status, sizeof(status), "Syntax copied");
    }

    /* বাঁয়ে তালিকা */
    listh = H - TOOL_H - STATUS_H - 8;
    r = ui_do_list(&U, 4, TOOL_H + 4, LIST_W, listh, items, nitems, &sel, &list_top);
    if (r && sel >= 0 && sel < nitems) {
        if (item_ent[sel] && item_ent[sel] != cur) open_entry(item_ent[sel], 1);
        else if (item_hdr[sel] && item_hdr[sel] != cur_hdr) open_header(item_hdr[sel], 1);
    }
    draw_page(LIST_W + 8, TOOL_H + 4, W - LIST_W - 12, listh);

    {
        char st[200];
        if (status[0]) snprintf(st, sizeof(st), "%s", status);
        else snprintf(st, sizeof(st), "%d pages - Esc closes, Alt+Left goes back, PageUp/PageDown scroll", cref_count());
        ui_statusbar(U.s, 0, H - STATUS_H, W, STATUS_H, st);
    }
    if (U.key == GFX_KEY_ESC) ui_quit(&U);
}

int main(int argc, char **argv)
{
    if (cref_load(NULL) < 0) {
        fprintf(stderr, "crefview: no reference files in %s\n", CREF_DIR);
        return 1;
    }
    if (gfx_init_window("C reference", 980, 660) < 0) {
        perror("crefview: no screen");
        return 1;
    }
    mono = gfx_font_mono();                      /* ধাপ ২৩ক */
    ui_init(&U);
    wrap_cols = 80;
    if (argc > 1) {
        const cref_entry *e = cref_find(argv[1]), *hits[1];
        if (!e && cref_search(argv[1], hits, 1) == 1) e = hits[0];
        if (e) open_entry(e, 0);
        else {
            snprintf(search, sizeof(search), "%s", argv[1]);
            snprintf(status, sizeof(status), "no page called '%s' - these mention it", argv[1]);
        }
    }
    if (!cur) build_page();
    build_items();
    while (ui_run(&U)) {
        frame();
        if (cur) {
            static const cref_entry *shown;
            if (shown != cur) {
                char t[96];
                snprintf(t, sizeof(t), "%s - C reference", cur->name);
                gfx_set_title(t);
                shown = cur;
            }
        }
    }
    free_items();
    clear_lines();
    gfx_quit();
    return 0;
}
