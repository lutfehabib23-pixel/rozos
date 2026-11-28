/* rozpdf — rozOS-এর PDF রিডার (ধাপ ২৬খ-১)
 *
 *   rozpdf [FILE.pdf [PAGE]]            উইন্ডোতে পড়া
 *   rozpdf -t FILE.pdf [FIRST [LAST]]   লেখা বের করা (stdout-এ)
 *   rozpdf -i FILE.pdf                  তথ্য (পাতা, মাপ, শিরোনাম, লেখক, ফন্ট)
 *   rozpdf -r FILE.pdf PAGE OUT.png [ZOOM%]   একটা পাতা PNG-তে
 *
 * উইন্ডো: পাতাগুলো ওপর-নিচে টানা, চাকা/PgUp/PgDn/তীর দিয়ে চলা, Ctrl+চাকা বা +/- দিয়ে
 * zoom, মাউস টেনে সরানো, Ctrl+F খোঁজা (F3 পরেরটা), Ctrl+C এই পাতার লেখা কপি, Ctrl+G পাতায় যাওয়া।
 * পাতা আঁকা হয় দরকার হলে, শেষ কয়েকটা মনে রাখা হয় (zoom বদলালে আবার)। */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <rozui.h>
#include "pdf.h"

#define VERSION "1.0"
#define GAP 12
#define TOOL_H 30
#define STATUS_H 22
#define NCACHE 8

static ui_ctx U;
static pdf_doc *doc;
static char path[512];
static int zoom = 100;                  /* % (১০০ = ৭২ dpi-তে আসল মাপ ×৯৬/৭২ নয় — সরল ১ pt = ১ px × zoom) */
static int fit_mode = 1;                /* ১ = প্রস্থ মেলানো, ২ = পুরো পাতা, ০ = নিজের zoom */
static long scroll_y, scroll_x;
static int view_x, view_y, view_w, view_h;
static int *page_top, *page_w, *page_h;  /* zoom অনুযায়ী অবস্থান */
static long doc_h;
static int doc_w;
static int layout_zoom = -1;
static char status_msg[200];

static struct {
    int page, zoom;
    gfx_surface *s;
    long used;
} cache[NCACHE];
static long use_clock;

/* খোঁজা */
static char find_str[200];
static int find_page = -1, find_item = -1, find_len;
static pdf_textitem *find_items;        /* find_page-এর লেখা (scale ১.০) */
static int find_nitems;

static double scale_of(void) { return zoom / 100.0 * 96.0 / 72.0; }   /* ১০০% = পর্দায় আসল মাপ (৯৬ dpi) */

/* ---------- লেখা সাজানো (পাতার লেখা → লাইন) ---------- */

static char *layout_text(pdf_textitem *t, int n, int *outlen)
{
    size_t cap = (size_t)n * 4 + 64, len = 0;
    char *o = (char *)malloc(cap);
    int i;
    if (!o) return NULL;
    for (i = 0; i < n; i++) {
        char b[5];
        int k;
        if (i > 0) {
            pdf_textitem *p = &t[i - 1], *c = &t[i];
            float ph = p->y1 - p->y0, ch = c->y1 - c->y0, h = ph > ch ? ph : ch;
            float pc = (p->y0 + p->y1) / 2, cc = (c->y0 + c->y1) / 2;
            if (h < 1) h = 1;
            if ((cc - pc > h * 0.6f) || (pc - cc > h * 0.6f) || c->x0 < p->x0 - h * 2) {
                if (len + 2 < cap) o[len++] = '\n';
            } else if (c->x0 - p->x1 > h * 0.18f && p->ch != ' ' && c->ch != ' ') {
                if (len + 2 < cap) o[len++] = ' ';
            }
        }
        if (t[i].ch == 0 || t[i].ch == '\t') { if (len + 2 < cap) o[len++] = ' '; continue; }
        if (t[i].ch < 32) continue;
        k = pdf_utf8(t[i].ch, b);
        if (len + (size_t)k + 2 >= cap) {
            char *no = (char *)realloc(o, cap * 2);
            if (!no) break;
            o = no;
            cap *= 2;
        }
        memcpy(o + len, b, (size_t)k);
        len += (size_t)k;
    }
    o[len] = 0;
    if (outlen) *outlen = (int)len;
    return o;
}

static pdf_textitem *page_text(pdf_doc *d, int page, int *n)
{
    pdf_render_opts o = { 1.0, 1, 1 };
    pdf_textitem *t = NULL;
    gfx_surface *s = pdf_render_page(d, page, &o, &t, n);
    gfx_surface_free(s);
    return t;
}

/* ---------- কমান্ড-লাইন ---------- */

static void print_str_obj(pdf_doc *d, const char *label, pobj *info, const char *key)
{
    pobj *o = pdf_get(d, info, key);
    char out[600];
    int i, k = 0;
    if (!o || o->type != P_STR || o->u.str.len == 0) return;
    if (o->u.str.len >= 2 && o->u.str.s[0] == 0xFE && o->u.str.s[1] == 0xFF) {
        for (i = 2; i + 1 < o->u.str.len && k < 590; i += 2) {
            uint32_t c = (uint32_t)(o->u.str.s[i] << 8 | o->u.str.s[i + 1]);
            if (c >= 0xD800 && c < 0xDC00 && i + 3 < o->u.str.len) {
                uint32_t c2 = (uint32_t)(o->u.str.s[i + 2] << 8 | o->u.str.s[i + 3]);
                c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00);
                i += 2;
            }
            k += pdf_utf8(c, out + k);
        }
    } else {
        for (i = 0; i < o->u.str.len && k < 590; i++) k += pdf_utf8(o->u.str.s[i], out + k);
    }
    out[k] = 0;
    printf("%-10s %s\n", label, out);
}

static int cli(int argc, char **argv)
{
    char err[200];
    pdf_doc *d;
    const char *mode = argv[1];
    if (argc < 3) {
        fprintf(stderr, "usage: rozpdf %s FILE.pdf ...  (rozpdf -h)\n", mode);
        return 2;
    }
    d = pdf_open(argv[2], err, sizeof(err));
    if (!d) {
        fprintf(stderr, "rozpdf: %s: %s\n", argv[2], err);
        return 1;
    }
    if (!strcmp(mode, "-t")) {
        int first = argc > 3 ? atoi(argv[3]) : 1, last = argc > 4 ? atoi(argv[4]) : (argc > 3 ? first : d->npages), p;
        if (first < 1) first = 1;
        if (last > d->npages) last = d->npages;
        for (p = first; p <= last; p++) {
            int n, len;
            pdf_textitem *t = page_text(d, p - 1, &n);
            char *s = layout_text(t, n, &len);
            if (s) { fwrite(s, 1, (size_t)len, stdout); free(s); }
            printf("\n%s", p < last ? "\f" : "");
            free(t);
        }
    } else if (!strcmp(mode, "-i")) {
        pobj *info = pdf_get(d, d->trailer, "Info");
        int p;
        printf("file       %s\n", argv[2]);
        printf("version    PDF %s%s\n", d->version, d->repaired ? " (damaged - repaired while reading)" : "");
        printf("pages      %d\n", d->npages);
        print_str_obj(d, "title", info, "Title");
        print_str_obj(d, "author", info, "Author");
        print_str_obj(d, "subject", info, "Subject");
        print_str_obj(d, "creator", info, "Creator");
        print_str_obj(d, "producer", info, "Producer");
        print_str_obj(d, "created", info, "CreationDate");
        for (p = 0; p < d->npages && p < 5; p++) {
            pdf_page *pg = &d->pages[p];
            double w = pg->box[2] - pg->box[0], h = pg->box[3] - pg->box[1];
            printf("page %-5d %.0f x %.0f pt (%.0f x %.0f mm)%s\n", p + 1, w, h, w * 25.4 / 72, h * 25.4 / 72,
                   pg->rotate ? (pg->rotate == 90 ? ", turned 90" : pg->rotate == 180 ? ", turned 180" : ", turned 270") : "");
        }
        if (d->npages > 5) printf("...\n");
    } else if (!strcmp(mode, "-r")) {
        int page = argc > 3 ? atoi(argv[3]) : 1, z = argc > 5 ? atoi(argv[5]) : 100;
        pdf_render_opts o;
        gfx_surface *s;
        if (argc < 5) {
            fprintf(stderr, "usage: rozpdf -r FILE.pdf PAGE OUT.png [ZOOM%%]\n");
            pdf_close(d);
            return 2;
        }
        if (page < 1 || page > d->npages) {
            fprintf(stderr, "rozpdf: page %d: the file has %d pages\n", page, d->npages);
            pdf_close(d);
            return 1;
        }
        if (z < 10) z = 10;
        if (z > 800) z = 800;
        o.scale = z / 100.0 * 96.0 / 72.0;
        o.want_text = 0;
        o.no_draw = 0;
        s = pdf_render_page(d, page - 1, &o, NULL, NULL);
        if (!s || gfx_save_png(s, argv[4]) < 0) {
            fprintf(stderr, "rozpdf: could not render page %d\n", page);
            gfx_surface_free(s);
            pdf_close(d);
            return 1;
        }
        printf("%s: page %d, %dx%d%s%s\n", argv[4], page, s->w, s->h, pdf_render_warning()[0] ? " - " : "", pdf_render_warning());
        gfx_surface_free(s);
    }
    pdf_close(d);
    return 0;
}

/* ---------- উইন্ডো ---------- */

static void cache_clear(void)
{
    int i;
    for (i = 0; i < NCACHE; i++) {
        gfx_surface_free(cache[i].s);
        cache[i].s = NULL;
        cache[i].page = -1;
    }
}

static gfx_surface *page_surface(int p)
{
    int i, victim = 0;
    pdf_render_opts o;
    for (i = 0; i < NCACHE; i++) {
        if (cache[i].s && cache[i].page == p && cache[i].zoom == zoom) {
            cache[i].used = ++use_clock;
            return cache[i].s;
        }
    }
    for (i = 1; i < NCACHE; i++) if (!cache[i].s || (cache[victim].s && cache[i].used < cache[victim].used)) victim = i;
    gfx_surface_free(cache[victim].s);
    o.scale = scale_of();
    o.want_text = 0;
    o.no_draw = 0;
    cache[victim].s = pdf_render_page(doc, p, &o, NULL, NULL);
    cache[victim].page = p;
    cache[victim].zoom = zoom;
    cache[victim].used = ++use_clock;
    if (pdf_render_warning()[0]) snprintf(status_msg, sizeof(status_msg), "%s", pdf_render_warning());
    return cache[victim].s;
}

static void layout(void)
{
    int i;
    long y = GAP;
    if (!doc) return;
    if (fit_mode && doc->npages > 0) {                 /* প্রস্থ/পাতা মেলানো zoom */
        int w, h, z;
        pdf_page_size(doc, 0, 96.0 / 72.0, &w, &h);
        if (fit_mode == 1) z = (int)((view_w - 2 * GAP) * 100.0 / w);
        else {
            int zw = (int)((view_w - 2 * GAP) * 100.0 / w), zh = (int)((view_h - 2 * GAP) * 100.0 / h);
            z = zw < zh ? zw : zh;
        }
        if (z < 10) z = 10;
        if (z > 400) z = 400;
        zoom = z;
    }
    if (layout_zoom == zoom && page_top) return;
    free(page_top); free(page_w); free(page_h);
    page_top = (int *)malloc(sizeof(int) * (size_t)doc->npages);
    page_w = (int *)malloc(sizeof(int) * (size_t)doc->npages);
    page_h = (int *)malloc(sizeof(int) * (size_t)doc->npages);
    if (!page_top || !page_w || !page_h) return;
    doc_w = 0;
    for (i = 0; i < doc->npages; i++) {
        pdf_page_size(doc, i, scale_of(), &page_w[i], &page_h[i]);
        page_top[i] = (int)y;
        y += page_h[i] + GAP;
        if (page_w[i] > doc_w) doc_w = page_w[i];
    }
    doc_h = y;
    layout_zoom = zoom;
}

static int current_page(void)
{
    int i, best = 0;
    long mid = scroll_y + view_h / 3;
    if (!doc || !page_top) return 0;
    for (i = 0; i < doc->npages; i++) if (page_top[i] <= mid) best = i;
    return best;
}

static void goto_page(int p)
{
    if (!doc || !page_top) return;
    if (p < 0) p = 0;
    if (p >= doc->npages) p = doc->npages - 1;
    scroll_y = page_top[p] - GAP;
}

static void set_zoom(int z)
{
    int cp;
    long off;
    if (!doc) return;
    if (z < 10) z = 10;
    if (z > 400) z = 400;
    cp = current_page();
    off = page_top ? scroll_y - page_top[cp] : 0;
    off = off * z / (zoom ? zoom : 100);
    fit_mode = 0;
    zoom = z;
    layout();
    if (page_top) scroll_y = page_top[cp] + off;
}

static void free_find(void)
{
    free(find_items);
    find_items = NULL;
    find_nitems = 0;
    find_page = -1;
    find_item = -1;
}

static int open_doc(const char *p)
{
    char err[200];
    pdf_doc *d = pdf_open(p, err, sizeof(err));
    if (!d) {
        char msg[800];
        snprintf(msg, sizeof(msg), "Cannot open %s:\n%s", p, err);
        ui_msgbox("rozpdf", msg, UI_MB_OK | UI_MB_ERROR);
        return -1;
    }
    cache_clear();
    free_find();
    if (doc) pdf_close(doc);
    doc = d;
    snprintf(path, sizeof(path), "%s", p);
    scroll_y = scroll_x = 0;
    layout_zoom = -1;
    fit_mode = 1;
    status_msg[0] = 0;
    {
        const char *b = strrchr(path, '/');
        char t[600];
        snprintf(t, sizeof(t), "rozpdf - %s", b ? b + 1 : path);
        gfx_set_title(t);
    }
    return 0;
}

/* বড়-ছোট হাতের অক্ষর না দেখে (ASCII) মেলানো */
static uint32_t fold(uint32_t c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int find_next(int from_start)
{
    uint32_t want[200];
    int nw = 0, p, startp, i;
    const unsigned char *s = (const unsigned char *)find_str;
    if (!doc || !find_str[0]) return 0;
    while (*s && nw < 200) {                           /* UTF-8 → codepoint */
        uint32_t c = *s++;
        if (c >= 0xF0 && s[0] && s[1] && s[2]) { c = ((c & 7) << 18) | ((s[0] & 63u) << 12) | ((s[1] & 63u) << 6) | (s[2] & 63u); s += 3; }
        else if (c >= 0xE0 && s[0] && s[1]) { c = ((c & 15) << 12) | ((s[0] & 63u) << 6) | (s[1] & 63u); s += 2; }
        else if (c >= 0xC0 && s[0]) { c = ((c & 31) << 6) | (s[0] & 63u); s += 1; }
        want[nw++] = fold(c);
    }
    startp = (from_start || find_page < 0) ? current_page() : find_page;
    for (p = startp; p < startp + doc->npages; p++) {
        int pg = p % doc->npages, first = (pg == find_page && !from_start && p == startp) ? find_item + 1 : 0;
        if (pg != find_page) {
            free(find_items);
            find_items = page_text(doc, pg, &find_nitems);
            find_page = pg;
        }
        for (i = first; i + nw <= find_nitems; i++) {
            int k, j = i, ok = 1;
            for (k = 0; k < nw && ok; k++) {
                while (j < find_nitems && find_items[j].ch == ' ' && want[k] != ' ') j++;   /* ফাঁকা বাদ */
                if (want[k] == ' ') { if (j < find_nitems && (find_items[j].ch == ' ' || find_items[j].ch == '\t')) j++; continue; }
                if (j >= find_nitems || fold(find_items[j].ch) != want[k]) ok = 0;
                j++;
            }
            if (ok) {
                find_item = i;
                find_len = j - i;
                if (page_top) {
                    double sc = scale_of();
                    long y = page_top[pg] + (long)(find_items[i].y0 * sc);
                    if (y < scroll_y + 20 || y > scroll_y + view_h - 40) scroll_y = y - view_h / 3;
                }
                snprintf(status_msg, sizeof(status_msg), "found on page %d", pg + 1);
                return 1;
            }
        }
        first = 0;
        if (p == startp + doc->npages - 1) break;
    }
    snprintf(status_msg, sizeof(status_msg), "\"%s\" not found", find_str);
    find_item = -1;
    return 0;
}

static void copy_page_text(void)
{
    int n, len, p;
    pdf_textitem *t;
    char *s;
    if (!doc) return;
    p = current_page();
    t = page_text(doc, p, &n);
    s = layout_text(t, n, &len);
    if (s && len > 0) {
        ui_clip_set(s, len);
        snprintf(status_msg, sizeof(status_msg), "page %d text copied (%d bytes)", p + 1, len);
    } else {
        snprintf(status_msg, sizeof(status_msg), "page %d has no text (scanned?)", p + 1);
    }
    free(s);
    free(t);
}

enum {
    M_OPEN = 1, M_INFO, M_QUIT, M_COPY, M_FIND, M_FINDNEXT, M_ZIN, M_ZOUT, M_FITW, M_FITP, M_Z100,
    M_NEXT, M_PREV, M_FIRST, M_LAST, M_GOTO, M_KEYS, M_ABOUT
};
static const struct ui_menuitem file_items[] = {
    { "Open...", "Ctrl+O", M_OPEN }, { "Document info", NULL, M_INFO }, { "-", NULL, 0 }, { "Quit", "Ctrl+Q", M_QUIT },
};
static const struct ui_menuitem edit_items[] = {
    { "Copy page text", "Ctrl+C", M_COPY }, { "-", NULL, 0 }, { "Find...", "Ctrl+F", M_FIND }, { "Find next", "F3", M_FINDNEXT },
};
static const struct ui_menuitem view_items[] = {
    { "Zoom in", "+", M_ZIN }, { "Zoom out", "-", M_ZOUT }, { "Actual size", NULL, M_Z100 }, { "-", NULL, 0 },
    { "Fit width", "Ctrl+W", M_FITW }, { "Fit page", "Ctrl+P", M_FITP },
};
static const struct ui_menuitem go_items[] = {
    { "Next page", "PgDn", M_NEXT }, { "Previous page", "PgUp", M_PREV }, { "First page", "Home", M_FIRST },
    { "Last page", "End", M_LAST }, { "-", NULL, 0 }, { "Go to page...", "Ctrl+G", M_GOTO },
};
static const struct ui_menuitem help_items[] = { { "Keys", "F1", M_KEYS }, { "About rozpdf", NULL, M_ABOUT } };
static const struct ui_menu menus[] = {
    { "File", file_items, 4 }, { "Edit", edit_items, 4 }, { "View", view_items, 6 }, { "Go", go_items, 6 }, { "Help", help_items, 2 },
};

static void show_info(void)
{
    char msg[1200];
    int k = 0;
    if (!doc) return;
    k += snprintf(msg + k, sizeof(msg) - (size_t)k, "%s\n\nPDF %s, %d pages%s\n", path, doc->version, doc->npages,
                  doc->repaired ? "\n(damaged file - repaired while reading)" : "");
    {
        pdf_page *pg = &doc->pages[current_page()];
        double w = pg->box[2] - pg->box[0], h = pg->box[3] - pg->box[1];
        k += snprintf(msg + k, sizeof(msg) - (size_t)k, "This page: %.0f x %.0f mm\n", w * 25.4 / 72, h * 25.4 / 72);
    }
    {
        pobj *info = pdf_get(doc, doc->trailer, "Info");
        static const char *const keys[] = { "Title", "Author", "Creator", "Producer" };
        int i;
        for (i = 0; i < 4; i++) {
            pobj *o = pdf_get(doc, info, keys[i]);
            if (o && o->type == P_STR && o->u.str.len > 0 && !(o->u.str.s[0] == 0xFE)) {
                k += snprintf(msg + k, sizeof(msg) - (size_t)k, "%s: %.*s\n", keys[i], o->u.str.len > 80 ? 80 : o->u.str.len,
                              (const char *)o->u.str.s);
            }
        }
    }
    ui_msgbox("Document info", msg, UI_MB_OK | UI_MB_INFO);
}

static void do_action(int a)
{
    char buf[512];
    switch (a) {
    case M_OPEN:
        snprintf(buf, sizeof(buf), "%s", path[0] ? path : "/home");
        if (ui_filedlg("Open PDF", buf, sizeof(buf), 0)) open_doc(buf);
        break;
    case M_INFO: show_info(); break;
    case M_QUIT: ui_quit(&U); break;
    case M_COPY: copy_page_text(); break;
    case M_FIND:
        if (!doc) break;
        if (ui_inputbox("Find", "Find text in the document:", find_str, sizeof(find_str)) && find_str[0]) {
            free_find();
            find_next(1);
        }
        break;
    case M_FINDNEXT:
        if (find_str[0]) find_next(0);
        else do_action(M_FIND);
        break;
    case M_ZIN: set_zoom(zoom < 100 ? zoom + 10 : zoom + 25); break;
    case M_ZOUT: set_zoom(zoom <= 100 ? zoom - 10 : zoom - 25); break;
    case M_Z100: set_zoom(100); break;
    case M_FITW: fit_mode = 1; layout_zoom = -1; break;
    case M_FITP: fit_mode = 2; layout_zoom = -1; break;
    case M_NEXT: goto_page(current_page() + 1); break;
    case M_PREV: goto_page(current_page() - 1); break;
    case M_FIRST: goto_page(0); break;
    case M_LAST: if (doc) goto_page(doc->npages - 1); break;
    case M_GOTO:
        if (!doc) break;
        snprintf(buf, sizeof(buf), "%d", current_page() + 1);
        if (ui_inputbox("Go to page", "Page number:", buf, 16)) goto_page(atoi(buf) - 1);
        break;
    case M_KEYS:
        ui_msgbox("rozpdf keys",
                  "Wheel, arrows, PgUp/PgDn, Space   move\n"
                  "Home / End          first / last page\n"
                  "Ctrl+G              go to page\n"
                  "+ / -, Ctrl+wheel   zoom\n"
                  "Ctrl+W / Ctrl+P     fit width / whole page\n"
                  "Drag with the mouse  move the page\n"
                  "Ctrl+F, F3          find, find next (Esc clears)\n"
                  "Ctrl+C              copy this page's text\n"
                  "Ctrl+O              open    Ctrl+Q  quit\n\n"
                  "In a terminal: rozpdf -t FILE (text), -i FILE (info),\n"
                  "-r FILE PAGE OUT.png (picture). See: help rozpdf",
                  UI_MB_OK | UI_MB_INFO);
        break;
    case M_ABOUT:
        ui_msgbox("About rozpdf", "rozpdf " VERSION " - the PDF reader for rozOS (phase 26b-1)\n\n"
                  "Text with the PDF's own fonts (FreeType), Bangla,\n"
                  "pictures (JPEG and others), lines and shapes.\n"
                  "Not yet: encrypted PDFs, JPEG 2000, colour gradients.",
                  UI_MB_OK | UI_MB_INFO);
        break;
    }
}

static void handle_key(int k, int mods)
{
    int ctrl = (mods & GFX_MOD_CTRL) != 0;
    switch (k) {
    case GFX_KEY_DOWN: scroll_y += 40; break;
    case GFX_KEY_UP: scroll_y -= 40; break;
    case GFX_KEY_RIGHT: scroll_x += 40; break;
    case GFX_KEY_LEFT: scroll_x -= 40; break;
    case GFX_KEY_PAGE_DOWN: case ' ': scroll_y += view_h - 40; break;
    case GFX_KEY_PAGE_UP: scroll_y -= view_h - 40; break;
    case GFX_KEY_HOME: if (ctrl || 1) goto_page(0); break;
    case GFX_KEY_END: if (doc) goto_page(doc->npages - 1); break;
    case '+': case '=': do_action(M_ZIN); break;
    case '-': case '_': do_action(M_ZOUT); break;
    case GFX_KEY_ESC: free_find(); status_msg[0] = 0; break;
    case GFX_KEY_F1: do_action(M_KEYS); break;
    case 7: do_action(M_GOTO); break;                  /* Ctrl+G */
    case 23: do_action(M_FITW); break;                 /* Ctrl+W */
    case 16: do_action(M_FITP); break;                 /* Ctrl+P */
    default: break;
    }
}

static void toolbar(void)
{
    const struct ui_theme *t = ui_theme_get();
    int x = 4, y = UI_MENUBAR_H + 3, h = TOOL_H - 6;
    char lab[64];
    gfx_fill_rect(U.s, 0, UI_MENUBAR_H, U.w, TOOL_H, t->menubar);
    if (ui_do_tool(&U, x, y, 30, h, "open", 'f', NULL)) do_action(M_OPEN);
    x += 36;
    if (ui_do_button(&U, x, y, 28, h, "<")) do_action(M_PREV);
    x += 30;
    snprintf(lab, sizeof(lab), "%d / %d", doc ? current_page() + 1 : 0, doc ? doc->npages : 0);
    if (ui_do_button(&U, x, y, 90, h, lab)) do_action(M_GOTO);
    x += 92;
    if (ui_do_button(&U, x, y, 28, h, ">")) do_action(M_NEXT);
    x += 40;
    if (ui_do_button(&U, x, y, 28, h, "-")) do_action(M_ZOUT);
    x += 30;
    snprintf(lab, sizeof(lab), "%d%%", zoom);
    gfx_text(U.s, NULL, x + 4, y + (h - 16) / 2, lab, t->text);
    x += 50;
    if (ui_do_button(&U, x, y, 28, h, "+")) do_action(M_ZIN);
    x += 34;
    if (ui_do_button(&U, x, y, 70, h, "Width")) do_action(M_FITW);
    x += 72;
    if (ui_do_button(&U, x, y, 60, h, "Page")) do_action(M_FITP);
    x += 70;
    if (ui_do_button(&U, x, y, 60, h, "Find")) do_action(M_FIND);
}

static void draw_view(void)
{
    gfx_surface *s = U.s;
    int i;
    gfx_fill_rect(s, view_x, view_y, view_w, view_h, GFX_RGB(70, 72, 76));
    if (!doc) {
        gfx_text(s, NULL, view_x + 30, view_y + 30, "No document - File > Open (Ctrl+O)", GFX_RGB(220, 220, 220));
        return;
    }
    gfx_set_clip(s, view_x, view_y, view_w, view_h);
    for (i = 0; i < doc->npages; i++) {
        long top = page_top[i] - scroll_y;
        int x = view_x + (doc_w < view_w ? (view_w - page_w[i]) / 2 : (int)(-scroll_x + (doc_w - page_w[i]) / 2));
        int y = view_y + (int)top;
        gfx_surface *ps;
        if (top + page_h[i] < 0) continue;
        if (top > view_h) break;
        gfx_fill_rect(s, x + 3, y + 3, page_w[i], page_h[i], GFX_RGB(40, 40, 42));   /* ছায়া */
        ps = page_surface(i);
        if (ps) gfx_blit(s, x, y, ps, 0, 0, ps->w, ps->h);
        else {
            gfx_fill_rect(s, x, y, page_w[i], page_h[i], GFX_WHITE);
            gfx_text(s, NULL, x + 20, y + 20, "(this page could not be drawn)", GFX_RGB(150, 0, 0));
        }
        if (find_page == i && find_item >= 0 && find_items) {   /* খোঁজার ফল হলুদে */
            double sc = scale_of();
            int k;
            for (k = find_item; k < find_item + find_len && k < find_nitems; k++) {
                pdf_textitem *t = &find_items[k];
                gfx_fill_rect_alpha(s, x + (int)(t->x0 * sc), y + (int)(t->y0 * sc), (int)((t->x1 - t->x0) * sc) + 1,
                                    (int)((t->y1 - t->y0) * sc) + 1, GFX_RGBA(255, 210, 0, 110));
            }
        }
    }
    gfx_reset_clip(s);
}

static void frame(void)
{
    int W = U.w, H = U.h, act;
    static int dragging, dmx, dmy;
    static long dsy, dsx;
    act = ui_menubar(&U, 0, 0, W, menus, 5);
    if (act) do_action(act);
    view_x = 0;
    view_y = UI_MENUBAR_H + TOOL_H;
    view_w = W - 16;
    view_h = H - view_y - STATUS_H;
    if (view_h < 40) view_h = 40;
    layout();
    toolbar();
    if (U.key) {
        handle_key(U.key, U.mods);
        U.key = 0;
        layout();
    }
    if (ui_hover(&U, view_x, view_y, view_w, view_h)) {
        if (U.wheel) {
            if (U.mods & GFX_MOD_CTRL) do_action(U.wheel < 0 ? M_ZIN : M_ZOUT);
            else if (U.mods & GFX_MOD_SHIFT) scroll_x += U.wheel * 60;
            else scroll_y += U.wheel * 80;
            layout();
        }
        if (U.pressed) { dragging = 1; dmx = U.mx; dmy = U.my; dsy = scroll_y; dsx = scroll_x; }
    }
    if (dragging && (U.buttons & GFX_BUTTON_LEFT)) {
        scroll_y = dsy - (U.my - dmy);
        scroll_x = dsx - (U.mx - dmx);
    }
    if (!(U.buttons & GFX_BUTTON_LEFT)) dragging = 0;
    if (doc) {
        if (scroll_y > doc_h - view_h) scroll_y = doc_h - view_h;
        if (scroll_y < 0) scroll_y = 0;
        if (doc_w <= view_w) scroll_x = 0;
        else {
            if (scroll_x > doc_w - view_w + GAP) scroll_x = doc_w - view_w + GAP;
            if (scroll_x < -GAP) scroll_x = -GAP;
        }
    }
    draw_view();
    if (doc) {
        int pos = (int)scroll_y;
        if (ui_do_vscroll(&U, W - 16, view_y, 16, view_h, &pos, (int)doc_h, view_h)) scroll_y = pos;
    } else {
        gfx_fill_rect(U.s, W - 16, view_y, 16, view_h, GFX_RGB(60, 60, 60));
    }
    {
        char st[400];
        const char *b = strrchr(path, '/');
        if (doc) snprintf(st, sizeof(st), "Page %d of %d  |  %d%%  |  %s%s%s", current_page() + 1, doc->npages, zoom,
                          b ? b + 1 : path, status_msg[0] ? "  |  " : "", status_msg);
        else snprintf(st, sizeof(st), "rozpdf %s  |  F1 keys", VERSION);
        ui_statusbar(U.s, 0, H - STATUS_H, W, STATUS_H, st);
    }
}

int main(int argc, char **argv)
{
    int first_page = 0;
    if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        printf("rozpdf %s - PDF reader\n"
               "  rozpdf [FILE.pdf [PAGE]]              read in a window\n"
               "  rozpdf -t FILE.pdf [FIRST [LAST]]     print the text\n"
               "  rozpdf -i FILE.pdf                    information\n"
               "  rozpdf -r FILE.pdf PAGE OUT.png [ZOOM%%]   save a page as a picture\n"
               "help rozpdf for more\n", VERSION);
        return 0;
    }
    if (argc > 1 && (!strcmp(argv[1], "-t") || !strcmp(argv[1], "-i") || !strcmp(argv[1], "-r"))) return cli(argc, argv);
    if (gfx_init_window("rozpdf", 900, 630) < 0) {
        perror("rozpdf: no screen (in a terminal use rozpdf -t FILE.pdf)");
        return 1;
    }
    ui_init(&U);
    {
        int i;
        for (i = 0; i < NCACHE; i++) cache[i].page = -1;
    }
    view_w = U.w - 16;
    view_h = U.h - UI_MENUBAR_H - TOOL_H - STATUS_H;
    if (argc > 1) {
        if (open_doc(argv[1]) == 0 && argc > 2) first_page = atoi(argv[2]) - 1;
    }
    while (ui_run(&U)) {
        frame();
        if (first_page > 0 && doc && page_top) { goto_page(first_page); first_page = 0; ui_redraw(&U); }
    }
    cache_clear();
    if (doc) pdf_close(doc);
    gfx_quit();
    return 0;
}
