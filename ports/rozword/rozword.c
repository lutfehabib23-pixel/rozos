/* rozword — rozOS-এর word processor (ধাপ ২৬ক)
 *
 * সাদা A4 পাতায় লেখা, Word-এর মতো: ফন্ট, মাপ, Bold/Italic/Underline/Strike, রং,
 * অনুচ্ছেদের alignment, Title/Heading, বুলেট আর নম্বর তালিকা, লাইনের ফাঁক, পাতা ভাঙা।
 * Undo/Redo, Find/Replace, Cut/Copy/Paste (rich — rozword থেকে rozword-এ ফরম্যাট সহ),
 * zoom, শব্দ গণনা, বাংলা টাইপ (Ctrl+Space, অভ্র phonetic — librozui-র rozbn)।
 * বাংলা লেখা আঁকে librozgfx-এর FreeType + shaping (যুক্তাক্ষর), বাংলা ফন্ট আপনিই।
 *
 * ফাইল: .rwd (rozword-এর নিজের ফরম্যাট — সাধারণ লেখা, নিচে বিবরণ) আর .txt (খোলা/সেভ)।
 * ধাপ ২৬খ-২: টেবিল, ছবি, header/footer (পাতা নম্বর)। ধাপ ২৬খ-৩: Word (.docx) খোলা আর সেভ।
 *
 * .rwd ফরম্যাট (UTF-8, লাইনে লাইনে):
 *   rozword 1
 *   p STYLE ALIGN LIST SPACING PAGEBREAK       অনুচ্ছেদ শুরু
 *   r FONT SIZE2 FLAGS RRGGBB TEXT             একই ফরম্যাটের লেখা (TEXT-এ \\ আর \n escape)
 *   e FONT SIZE2 FLAGS RRGGBB                  খালি অনুচ্ছেদের ফরম্যাট
 *   STYLE 0 Normal 1 Title 2-4 Heading 1-3; ALIGN 0 বাঁ 1 মাঝ 2 ডান 3 দুই পাশ;
 *   LIST 0 নেই 1 বুলেট 2 নম্বর; SPACING ১০০ = একক লাইন; SIZE2 = পয়েন্ট × ২;
 *   FLAGS 1 bold 2 italic 4 underline 8 strike; FONT-এর নামে ফাঁকা জায়গা = '_'
 * ধাপ ২৬খ-২ ("rozword 2"; "rozword 1"-ও খোলে):
 *   p ... PAGEBREAK TBL ROW COL                 টেবিলের ঘরে হলে TBL > 0
 *   T ID COLS W1 W2 ...                         টেবিল (কলামের চওড়া, হাজার ভাগে)
 *   i ID EXT BASE64                             ছবির আসল ফাইল (png/jpg ...)
 *   g ID W H                                    ছবির অনুচ্ছেদ (W H = px, ৯৬ dpi)
 *   h ALIGN TEXT / f ALIGN TEXT                 header / footer ({page} {pages})
 *
 * বানানো (rozOS-এ):  tcc rozword.c -lrozui -lrozgfx -o rozword   (বা gcc, একই)
 *   /usr/src/rozword/Makefile-এ: make  বা  make CC=gcc */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <rozui.h>
#include <rozbn.h>
#include <zlib.h>                  /* ধাপ ২৬খ-৩: .docx (ZIP) */

#define RW_VERSION "1.2 (phase 26b-3)"

/* ================= গঠন ================= */

#define A_BOLD   1
#define A_ITALIC 2
#define A_UNDER  4
#define A_STRIKE 8

typedef struct {
    unsigned char font, flags;
    unsigned short size2;          /* পয়েন্ট × ২ (১১ pt = ২২) */
    uint32_t color;                /* 0xRRGGBB */
} Attr;

typedef struct {
    int s, e;                      /* বাইট [s, e) */
    int w;                         /* লেখার চওড়া (justify-এর আগে) */
    int asc, h;                    /* বেসলাইন পর্যন্ত, পুরো উচ্চতা (লাইনের ফাঁকসহ) */
    int x;                         /* পাতার লেখার বাঁ ধার থেকে (alignment, তালিকার indent) */
    int nsp;                       /* লাইনের ভেতরের ফাঁকা (justify) */
    int extra;                     /* প্রতি ফাঁকে বাড়তি (justify), পিক্সেল × ১০০ */
    int page, y;                   /* কোন পাতা, পাতার লেখার ওপর থেকে */
} Line;

typedef struct {
    char *t;
    unsigned short *a;             /* প্রতি বাইটের Attr নম্বর */
    int n, cap;
    unsigned short eattr;          /* খালি হলে / শেষে লেখার ফরম্যাট */
    unsigned char style, align, list, pbreak;
    int spacing;                   /* ১০০ = একক */
    /* লেআউট */
    Line *ln;
    int nl, lcap;
    int dirty;
    int before, after;             /* অনুচ্ছেদের আগে/পরে ফাঁক (px) */
    /* ধাপ ২৬খ-২ */
    unsigned char kind;            /* 0 লেখা, 1 ছবি (লেখা নেই; img, img_w/h) */
    int img, img_w, img_h;         /* ছবির নম্বর (imgs[]), মাপ px-এ (১০০% zoom-এ, ৯৬ dpi) */
    int tbl, row, col;             /* টেবিলের ঘর (tbl = 0 মানে টেবিলে নয়) */
    int cell_x, cell_w;            /* লেআউটের সময়: ঘরের বাঁ ধার আর চওড়া (px) */
    int lay_w, lay_x;              /* শেষ লেআউট কোন চওড়া/x-এ হয়েছিল */
} Para;

/* ধাপ ২৬খ-২: ছবির ভাণ্ডার — দলিলে ছবি নম্বর দিয়ে (undo/কপিতে শুধু নম্বর, ফাইলে আসল ছবি) */
typedef struct {
    unsigned char *data;           /* আসল ফাইলের বাইট (PNG/JPEG ...) */
    long len;
    char ext[8];
    gfx_surface *s;                /* খোলা ছবি */
    gfx_surface *scaled;           /* শেষ যে মাপে আঁকা হয়েছিল */
} RImg;
static RImg *imgs;
static int nimgs;

/* ধাপ ২৬খ-২: টেবিল — কলাম আর প্রতি কলামের চওড়া (লেখার জায়গার হাজার ভাগে) */
#define TBL_MAXCOL 16
typedef struct { int cols; int w[TBL_MAXCOL]; } RTable;
static RTable *tbls;               /* tbls[0] ফাঁকা (tbl = 0 মানে টেবিল নয়) */
static int ntbls;

/* ধাপ ২৬খ-২: header/footer — {page} আর {pages} বসে */
static char hdr_text[256], ftr_text[256];
static int hdr_align = 1, ftr_align = 1;

/* টেবিলের ঘরের দাগ আঁকার জন্য (paginate ভরে) */
typedef struct { int page, x, y, w, h; } CellRect;
static CellRect *crects;
static int ncrects, crcap;

static Attr attrs[4096];
static int nattr;

static Para *P;                    /* অনুচ্ছেদ */
static int np, pcap;

/* কার্সর আর নির্বাচন: (অনুচ্ছেদ, বাইট) */
static int cp, ci, sp, si;
static int typing_attr = -1;       /* Ctrl+B ইত্যাদি নির্বাচন ছাড়া — পরের লেখার ফরম্যাট */
static int typing_cp = -1, typing_ci = -1;
static int want_x = -1;            /* ওপর/নিচে যাওয়ার সময় x */

static char path[512];
static int untitled = 1, modified;
static char status_msg[600];
static int zoom = 100;             /* শতাংশ */
static long scroll_y;              /* পাতাগুলোর ওপরে কতটা স্ক্রল (px) */
static int follow_cursor, dragging, layout_all = 1;
static int npages = 1;
static ui_ctx U;
static rozbn_comp bcomp;

/* ================= ফন্ট ================= */

struct family {
    const char *name;
    const char *regular, *bold, *italic;   /* /usr/share/fonts-এর নিচে; NULL = নেই */
    int present;
};

static struct family fams[] = {
    { "Noto Sans",          "NotoSans.ttf",           NULL,                    NULL,              0 },
    { "Noto Serif",         "NotoSerif.ttf",          NULL,                    NULL,              0 },
    { "Inter",              "Inter-Regular.ttf",      "Inter-Bold.ttf",        "Inter-Italic.ttf", 0 },
    { "Roboto",             "Roboto.ttf",             NULL,                    NULL,              0 },
    { "Open Sans",          "OpenSans.ttf",           NULL,                    NULL,              0 },
    { "Source Sans 3",      "SourceSans3.ttf",        NULL,                    NULL,              0 },
    { "Source Serif 4",     "SourceSerif4.ttf",       NULL,                    NULL,              0 },
    { "DejaVu Sans",        "DejaVuSans.ttf",         NULL,                    NULL,              0 },
    { "Fira Code",          "FiraCode.ttf",           NULL,                    NULL,              0 },
    { "JetBrains Mono",     "JetBrainsMono-Regular.ttf", "JetBrainsMono-Bold.ttf", NULL,         0 },
    { "Noto Sans Bengali",  "NotoSansBengali.ttf",    NULL,                    NULL,              0 },
    { "Noto Serif Bengali", "NotoSerifBengali.ttf",   NULL,                    NULL,              0 },
    { "Hind Siliguri",      "HindSiliguri-Regular.ttf", "HindSiliguri-Bold.ttf", NULL,            0 },
    { "Tiro Bangla",        "TiroBangla-Regular.ttf", NULL,                    NULL,              0 },
};
#define NFAM ((int)(sizeof(fams) / sizeof(fams[0])))
#define FONT_DIR "/usr/share/fonts/"

typedef struct {
    int fam, px, variant;          /* variant 0 সাধারণ, 1 bold ফাইল, 2 italic ফাইল */
    gfx_font *f;
    int h;
} FontEnt;

static FontEnt fcache[96];
static int nfcache;

typedef struct {
    gfx_font *f;
    int h, asc;
    int fake_bold, fake_italic;
} Face;

static int file_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

static void fonts_init(void)
{
    int i;
    char p[256];
    for (i = 0; i < NFAM; i++) {
        snprintf(p, sizeof(p), FONT_DIR "%s", fams[i].regular);
        fams[i].present = file_exists(p);
        if (fams[i].bold) {
            snprintf(p, sizeof(p), FONT_DIR "%s", fams[i].bold);
            if (!file_exists(p)) fams[i].bold = NULL;
        }
        if (fams[i].italic) {
            snprintf(p, sizeof(p), FONT_DIR "%s", fams[i].italic);
            if (!file_exists(p)) fams[i].italic = NULL;
        }
    }
}

static int fam_by_name(const char *name)
{
    int i;
    for (i = 0; i < NFAM; i++) if (!strcasecmp(fams[i].name, name)) return i;
    return -1;
}

static int default_family(void)
{
    int i;
    for (i = 0; i < NFAM; i++) if (fams[i].present) return i;
    return 0;
}

static int pt_to_px(int size2)
{
    int px = (size2 * zoom * 2 + 150) / 300;       /* pt × ৯৬/৭২ × zoom/১০০ */
    return px < 4 ? 4 : px;
}

static gfx_font *font_load(int fam, int px, int variant, int *h)
{
    int i;
    char p[256];
    const char *file;
    gfx_font *f;
    for (i = 0; i < nfcache; i++)
        if (fcache[i].fam == fam && fcache[i].px == px && fcache[i].variant == variant) {
            *h = fcache[i].h;
            return fcache[i].f;
        }
    file = variant == 1 ? fams[fam].bold : variant == 2 ? fams[fam].italic : fams[fam].regular;
    snprintf(p, sizeof(p), FONT_DIR "%s", file ? file : fams[fam].regular);
    f = fams[fam].present ? gfx_font_load_ttf(p, px, 0) : NULL;
    if (!f) f = gfx_font_ui();
    if (nfcache == (int)(sizeof(fcache) / sizeof(fcache[0]))) {   /* ভরা — পুরোনোগুলো ফেলে */
        for (i = 0; i < nfcache; i++)
            if (fcache[i].f && fcache[i].f != gfx_font_ui()) gfx_font_free(fcache[i].f);
        nfcache = 0;
    }
    fcache[nfcache].fam = fam;
    fcache[nfcache].px = px;
    fcache[nfcache].variant = variant;
    fcache[nfcache].f = f;
    fcache[nfcache].h = f ? gfx_font_height(f) : px;
    *h = fcache[nfcache].h;
    nfcache++;
    return f;
}

static void face_of(int ai, Face *fc)
{
    const Attr *a = &attrs[ai];
    int fam = a->font < NFAM ? a->font : 0, px = pt_to_px(a->size2), variant = 0;
    fc->fake_bold = fc->fake_italic = 0;
    if (a->flags & A_BOLD) {
        if (fams[fam].bold) variant = 1;
        else fc->fake_bold = 1;
    }
    if (a->flags & A_ITALIC) {
        if (!variant && fams[fam].italic) variant = 2;
        else fc->fake_italic = 1;
    }
    fc->f = font_load(fam, px, variant, &fc->h);
    fc->asc = fc->h * 4 / 5;
}

/* ================= Attr টেবিল ================= */

static int attr_id(const Attr *a)
{
    int i;
    for (i = 0; i < nattr; i++)
        if (attrs[i].font == a->font && attrs[i].flags == a->flags && attrs[i].size2 == a->size2 &&
            attrs[i].color == a->color) return i;
    if (nattr == (int)(sizeof(attrs) / sizeof(attrs[0]))) return 0;
    attrs[nattr] = *a;
    return nattr++;
}

static int default_attr(void)
{
    Attr a;
    a.font = (unsigned char)default_family();
    a.flags = 0;
    a.size2 = 22;
    a.color = 0x000000;
    return attr_id(&a);
}

/* ================= অনুচ্ছেদ ================= */

static void para_reserve(Para *p, int need)
{
    if (need <= p->cap) return;
    p->cap = need + need / 2 + 32;
    p->t = (char *)realloc(p->t, (size_t)p->cap + 1);
    p->a = (unsigned short *)realloc(p->a, (size_t)p->cap * sizeof(unsigned short));
    if (!p->t || !p->a) { fprintf(stderr, "rozword: out of memory\n"); exit(1); }
}

static void para_init(Para *p, int eattr)
{
    memset(p, 0, sizeof(*p));
    p->eattr = (unsigned short)eattr;
    p->spacing = 115;
    p->dirty = 1;
    para_reserve(p, 16);
    p->t[0] = '\0';
}

static void para_free(Para *p)
{
    free(p->t);
    free(p->a);
    free(p->ln);
    memset(p, 0, sizeof(*p));
}

static void para_insert_bytes(Para *p, int at, const char *s, const unsigned short *a, int n, int one_attr)
{
    int i;
    para_reserve(p, p->n + n);
    memmove(p->t + at + n, p->t + at, (size_t)(p->n - at));
    memmove(p->a + at + n, p->a + at, (size_t)(p->n - at) * sizeof(unsigned short));
    memcpy(p->t + at, s, (size_t)n);
    for (i = 0; i < n; i++) p->a[at + i] = a ? a[i] : (unsigned short)one_attr;
    p->n += n;
    p->t[p->n] = '\0';
    p->dirty = 1;
}

static void para_delete_bytes(Para *p, int from, int to)
{
    if (to <= from) return;
    if (from == 0 && to == p->n && p->n) p->eattr = p->a[0];   /* খালি হলেও ফরম্যাট থাকে */
    memmove(p->t + from, p->t + to, (size_t)(p->n - to));
    memmove(p->a + from, p->a + to, (size_t)(p->n - to) * sizeof(unsigned short));
    p->n -= to - from;
    p->t[p->n] = '\0';
    p->dirty = 1;
}

static void paras_insert_at(int at, int count)
{
    if (np + count > pcap) {
        pcap = np + count + 64;
        P = (Para *)realloc(P, (size_t)pcap * sizeof(Para));
        if (!P) { fprintf(stderr, "rozword: out of memory\n"); exit(1); }
    }
    memmove(P + at + count, P + at, (size_t)(np - at) * sizeof(Para));
    np += count;
    layout_all = 1;
}

static void paras_remove(int from, int to)    /* [from, to) */
{
    int i;
    for (i = from; i < to; i++) para_free(&P[i]);
    memmove(P + from, P + to, (size_t)(np - to) * sizeof(Para));
    np -= to - from;
    layout_all = 1;
}

static void doc_clear(void)
{
    int i;
    for (i = 0; i < np; i++) para_free(&P[i]);
    np = 0;
    nattr = 0;
    paras_insert_at(0, 1);
    para_init(&P[0], default_attr());
    cp = ci = sp = si = 0;
    typing_attr = -1;
    scroll_y = 0;
    layout_all = 1;
}

/* বাইটের Attr (শেষে/খালি হলে eattr) — লেখা বসানোর ফরম্যাট: কার্সরের আগের অক্ষর */
static int attr_before(int p, int i)
{
    if (i > 0 && i <= P[p].n) return P[p].a[i - 1];
    if (P[p].n > 0) return P[p].a[0];
    return P[p].eattr;
}

/* ================= UTF-8 আর বাংলা ক্লাস্টার ================= */

static int utf8_decode(const char *s, int n, int i, int *len)
{
    unsigned char c = (unsigned char)s[i];
    int cpv, k, l;
    if (c < 0x80) { *len = 1; return c; }
    l = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    if (l == 1 || i + l > n) { *len = 1; return c; }
    cpv = c & (0xFF >> (l + 1));
    for (k = 1; k < l; k++) cpv = (cpv << 6) | ((unsigned char)s[i + k] & 0x3F);
    *len = l;
    return cpv;
}

/* বাংলার কার-চিহ্ন, হসন্ত, চন্দ্রবিন্দু ইত্যাদি — আগের অক্ষরের সাথে এক ঘর */
static int is_mark(int c)
{
    return (c >= 0x0981 && c <= 0x0983) || c == 0x09BC || (c >= 0x09BE && c <= 0x09CD) || c == 0x09D7 ||
           (c >= 0x09E2 && c <= 0x09E3) || c == 0x200C || c == 0x200D || (c >= 0x0300 && c <= 0x036F);
}

/* i কি কার্সর দাঁড়ানোর জায়গা */
static int is_stop(const Para *p, int i)
{
    int len, c, j, pc;
    if (i <= 0 || i >= p->n) return 1;
    if (((unsigned char)p->t[i] & 0xC0) == 0x80) return 0;
    c = utf8_decode(p->t, p->n, i, &len);
    if (is_mark(c)) return 0;
    j = i - 1;
    while (j > 0 && ((unsigned char)p->t[j] & 0xC0) == 0x80) j--;
    pc = utf8_decode(p->t, p->n, j, &len);
    return pc != 0x09CD;                 /* হসন্তের পরের ব্যঞ্জন যুক্তাক্ষরের অংশ */
}

static int next_stop(const Para *p, int i)
{
    if (i >= p->n) return p->n;
    i++;
    while (i < p->n && !is_stop(p, i)) i++;
    return i;
}

static int prev_stop(const Para *p, int i)
{
    if (i <= 0) return 0;
    i--;
    while (i > 0 && !is_stop(p, i)) i--;
    return i;
}

/* ================= মাপ ================= */

#define PAGE_W      794            /* A4, ৯৬ dpi (২১০ মিমি) */
#define PAGE_H      1123           /* ২৯৭ মিমি */
#define MARGIN      96             /* ১ ইঞ্চি */
#define LIST_INDENT 36
#define PAGE_GAP    24

static int zpx(int v) { return v * zoom / 100; }
static int content_w(void) { return zpx(PAGE_W - 2 * MARGIN); }
static int content_h(void) { return zpx(PAGE_H - 2 * MARGIN); }

static char mbuf[4100];

/* এক ফরম্যাটের লেখার চওড়া */
static int run_w(int ai, const char *s, int n)
{
    Face fc;
    int w = 0;
    face_of(ai, &fc);
    if (!fc.f) return n * 8;
    while (n > 0) {
        int k = n > 4000 ? 4000 : n;
        memcpy(mbuf, s, (size_t)k);
        mbuf[k] = '\0';
        w += gfx_text_width(fc.f, mbuf);
        s += k;
        n -= k;
    }
    return w + fc.fake_bold;
}

/* [a, b) — ফরম্যাট বদলালে আলাদা করে মেপে যোগ */
static int text_w(const Para *p, int a, int b)
{
    int w = 0;
    while (a < b) {
        int j = a;
        while (j < b && p->a[j] == p->a[a]) j++;
        w += run_w(p->a[a], p->t + a, j - a);
        a = j;
    }
    return w;
}

/* ================= লেআউট ================= */

static void add_line(Para *p, int s, int e)
{
    if (p->nl == p->lcap) {
        p->lcap = p->lcap ? p->lcap * 2 : 4;
        p->ln = (Line *)realloc(p->ln, (size_t)p->lcap * sizeof(Line));
        if (!p->ln) { fprintf(stderr, "rozword: out of memory\n"); exit(1); }
    }
    memset(&p->ln[p->nl], 0, sizeof(Line));
    p->ln[p->nl].s = s;
    p->ln[p->nl].e = e;
    p->nl++;
}

static int list_indent(const Para *p) { return p->list ? zpx(LIST_INDENT) : 0; }

/* ধাপ ২৬খ-২: টেবিলের ঘরে লেখার জায়গা ঘরের ভেতরে (দুই পাশে CELL_PAD) */
#define CELL_PAD 6
static int para_x0(const Para *p) { return p->tbl ? p->cell_x + zpx(CELL_PAD) : 0; }
static int para_wrap(const Para *p)
{
    int w = p->tbl ? p->cell_w - 2 * zpx(CELL_PAD) : content_w();
    return w < 20 ? 20 : w;
}

/* ছবির আঁকার মাপ (zoom সহ, জায়গার চেয়ে বড় হলে ছোট করে) */
static void img_disp(const Para *p, int *w, int *h)
{
    int iw = zpx(p->img_w > 0 ? p->img_w : 100), ih = zpx(p->img_h > 0 ? p->img_h : 100), avail = para_wrap(p);
    if (iw > avail) { ih = (int)((long)ih * avail / iw); iw = avail; }
    if (iw < 1) iw = 1;
    if (ih < 1) ih = 1;
    *w = iw;
    *h = ih;
}

static void finish_line(Para *p, Line *L, int last)
{
    int asc = 0, desc = 0, i, te, avail = para_wrap(p) - list_indent(p);
    Face fc;
    if (L->s == L->e) {                              /* খালি লাইন: কার্সরের ফরম্যাটের উচ্চতা */
        face_of(L->s > 0 ? p->a[L->s - 1] : (p->n ? p->a[0] : p->eattr), &fc);
        asc = fc.asc;
        desc = fc.h - fc.asc;
    }
    for (i = L->s; i < L->e;) {
        int j = i;
        while (j < L->e && p->a[j] == p->a[i]) j++;
        face_of(p->a[i], &fc);
        if (fc.asc > asc) asc = fc.asc;
        if (fc.h - fc.asc > desc) desc = fc.h - fc.asc;
        i = j;
    }
    L->asc = asc;
    L->h = (asc + desc) * p->spacing / 100;
    if (L->h < asc + desc) L->h = asc + desc;
    te = L->e;
    while (te > L->s && p->t[te - 1] == ' ') te--;
    L->w = text_w(p, L->s, te);
    L->nsp = 0;
    for (i = L->s; i < te; i++) if (p->t[i] == ' ') L->nsp++;
    L->extra = 0;
    L->x = para_x0(p) + list_indent(p);
    if (p->align == 1) L->x += (avail - L->w) / 2;
    else if (p->align == 2) L->x += avail - L->w;
    else if (p->align == 3 && !last && L->nsp > 0 && avail > L->w) L->extra = (avail - L->w) * 100 / L->nsp;
    if (L->x < para_x0(p)) L->x = para_x0(p);
}

static void layout_para(Para *p)
{
    int avail = para_wrap(p) - list_indent(p), ls = 0, lw = 0, i = 0, k;
    p->nl = 0;
    p->lay_w = para_wrap(p);
    p->lay_x = para_x0(p);
    if (p->kind == 1) {                              /* ধাপ ২৬খ-২: ছবি — একটা লাইন, ছবির মাপের */
        int w, h;
        Line *L;
        add_line(p, 0, 0);
        L = &p->ln[0];
        img_disp(p, &w, &h);
        L->asc = h;
        L->h = h + zpx(4);
        L->w = w;
        L->x = para_x0(p);
        if (p->align == 1) L->x += (avail - w) / 2;
        else if (p->align == 2) L->x += avail - w;
        p->dirty = 0;
        return;
    }
    while (i < p->n) {
        int we = i, se, ww, sw;
        while (we < p->n && p->t[we] != ' ') we++;
        se = we;
        while (se < p->n && p->t[se] == ' ') se++;
        ww = text_w(p, i, we);
        sw = text_w(p, we, se);
        if (lw > 0 && lw + ww > avail) {             /* শব্দটা পরের লাইনে */
            add_line(p, ls, i);
            ls = i;
            lw = 0;
        }
        if (lw == 0 && ww > avail) {                 /* এক লাইনের চেয়ে লম্বা শব্দ — ভেঙে */
            int j = i;
            while (j < we) {
                int nj = next_stop(p, j);
                if (j > ls && text_w(p, ls, nj) > avail) {
                    add_line(p, ls, j);
                    ls = j;
                }
                j = nj;
            }
            lw = text_w(p, ls, we);
        } else {
            lw += ww;
        }
        lw += sw;
        i = se;
    }
    add_line(p, ls, p->n);
    for (k = 0; k < p->nl; k++) finish_line(p, &p->ln[k], k == p->nl - 1);
    p->dirty = 0;
}

/* সব লাইনের ক্রম (ওপর/নিচে যাওয়া, ক্লিক) */
static int *gl_p, *gl_l, ngl, glcap;

static void gl_add(int pi, int k)
{
    if (ngl == glcap) {
        glcap = glcap ? glcap * 2 : 256;
        gl_p = (int *)realloc(gl_p, (size_t)glcap * sizeof(int));
        gl_l = (int *)realloc(gl_l, (size_t)glcap * sizeof(int));
        if (!gl_p || !gl_l) { fprintf(stderr, "rozword: out of memory\n"); exit(1); }
    }
    gl_p[ngl] = pi;
    gl_l[ngl] = k;
    ngl++;
}

static void crect_add(int page, int x, int y, int w, int h)
{
    if (ncrects == crcap) {
        crcap = crcap ? crcap * 2 : 64;
        crects = (CellRect *)realloc(crects, (size_t)crcap * sizeof(CellRect));
        if (!crects) { fprintf(stderr, "rozword: out of memory\n"); exit(1); }
    }
    crects[ncrects].page = page;
    crects[ncrects].x = x;
    crects[ncrects].y = y;
    crects[ncrects].w = w;
    crects[ncrects].h = h;
    ncrects++;
}

static void relayout_if(Para *p)
{
    if (layout_all || p->dirty || p->lay_w != para_wrap(p) || p->lay_x != para_x0(p)) layout_para(p);
}

/* ধাপ ২৬খ-২: টেবিল (P[i] থেকে একই tbl-এর অনুচ্ছেদগুলো) — সারি ধরে; সারি পাতায় না আঁটলে
 * পরের পাতায় (সারি ভাঙে না)। ফেরত: টেবিলের পরের অনুচ্ছেদ */
static int paginate_table(int i, int *page, int *y)
{
    int tid = P[i].tbl, j = i, c, ch = content_h(), cw = content_w(), pad = zpx(CELL_PAD);
    int cx[TBL_MAXCOL + 1], cols;
    RTable *T = (tid > 0 && tid < ntbls) ? &tbls[tid] : NULL;
    while (j < np && P[j].tbl == tid) j++;
    cols = T ? T->cols : 1;
    if (cols < 1) cols = 1;
    if (cols > TBL_MAXCOL) cols = TBL_MAXCOL;
    {
        int tot = 0, x = 0;
        for (c = 0; c < cols; c++) tot += T && T->w[c] > 0 ? T->w[c] : 1000 / cols;
        for (c = 0; c < cols; c++) {
            cx[c] = x;
            x += (int)((long)cw * (T && T->w[c] > 0 ? T->w[c] : 1000 / cols) / (tot > 0 ? tot : 1));
        }
        cx[cols] = cw;
    }
    if (*y > 0) *y += zpx(4);
    while (i < j) {
        int r = P[i].row, k, e = i, cellh[TBL_MAXCOL], cury[TBL_MAXCOL], rowh = 0;
        while (e < j && P[e].row == r) e++;              /* [i, e) = এই সারি */
        for (c = 0; c < cols; c++) cellh[c] = 0;
        for (k = i; k < e; k++) {
            Para *p = &P[k];
            int col = p->col < 0 ? 0 : p->col >= cols ? cols - 1 : p->col, l;
            p->cell_x = cx[col];
            p->cell_w = cx[col + 1] - cx[col];
            relayout_if(p);
            p->before = 0;
            p->after = zpx(2);
            for (l = 0; l < p->nl; l++) cellh[col] += p->ln[l].h;
            cellh[col] += p->after;
        }
        for (c = 0; c < cols; c++) if (cellh[c] + 2 * pad > rowh) rowh = cellh[c] + 2 * pad;
        if (rowh < zpx(16)) rowh = zpx(16);
        if (*y > 0 && *y + rowh > ch) { (*page)++; *y = 0; }
        for (c = 0; c < cols; c++) cury[c] = *y + pad;
        for (k = i; k < e; k++) {
            Para *p = &P[k];
            int col = p->col < 0 ? 0 : p->col >= cols ? cols - 1 : p->col, l;
            for (l = 0; l < p->nl; l++) {
                p->ln[l].page = *page;
                p->ln[l].y = cury[col];
                cury[col] += p->ln[l].h;
                gl_add(k, l);
            }
            cury[col] += p->after;
        }
        for (c = 0; c < cols; c++) crect_add(*page, cx[c], *y, cx[c + 1] - cx[c], rowh);
        *y += rowh;
        i = e;
    }
    *y += zpx(10);
    return j;
}

static void paginate(void)
{
    int page = 0, y = 0, ch = content_h(), i, k;
    ngl = 0;
    ncrects = 0;
    for (i = 0; i < np;) {
        Para *p = &P[i];
        if (p->tbl) {                                 /* ধাপ ২৬খ-২ */
            if (p->pbreak && (y > 0 || i > 0)) { page++; y = 0; }
            i = paginate_table(i, &page, &y);
            continue;
        }
        relayout_if(p);
        p->before = p->style ? zpx(p->style == 1 ? 6 : 14) : 0;
        p->after = zpx(p->style == 1 ? 10 : 8);
        if (p->pbreak && (y > 0 || i > 0)) { page++; y = 0; }
        if (y > 0) y += p->before;
        for (k = 0; k < p->nl; k++) {
            Line *L = &p->ln[k];
            if (y > 0 && y + L->h > ch) { page++; y = 0; }
            L->page = page;
            L->y = y;
            y += L->h;
            gl_add(i, k);
        }
        y += p->after;
        i++;
    }
    npages = page + 1;
    layout_all = 0;
}

/* i কোন লাইনে (লাইনের শেষ = পরের লাইনের শুরু, তাই পরেরটা) */
static int line_of(const Para *p, int i)
{
    int k;
    for (k = 0; k < p->nl - 1; k++) if (i < p->ln[k].e) return k;
    return p->nl - 1;
}

static int gl_index(int p, int k)
{
    int g;
    for (g = 0; g < ngl; g++) if (gl_p[g] == p && gl_l[g] == k) return g;
    return 0;
}

/* লাইনের ভেতরে pos-এর x (লাইনের বাঁ ধার = লেখার জায়গার বাঁ ধার থেকে) */
static int x_in_line(const Para *p, const Line *L, int pos)
{
    int x = L->x + text_w(p, L->s, pos), i, sp = 0;
    if (L->extra) {
        int te = L->e;
        while (te > L->s && p->t[te - 1] == ' ') te--;
        for (i = L->s; i < pos && i < te; i++) if (p->t[i] == ' ') sp++;
        x += sp * L->extra / 100;
    }
    return x;
}

/* ================= পর্দার জায়গা ================= */

static int view_x0, view_y0, view_w, view_h;

static int page_left(void)
{
    int x = (view_w - zpx(PAGE_W)) / 2;
    return view_x0 + (x < 10 ? 10 : x);
}

static long page_top_doc(int page) { return (long)page * (zpx(PAGE_H) + PAGE_GAP) + PAGE_GAP; }
static long doc_height(void) { return page_top_doc(npages); }

static long line_top_doc(const Line *L) { return page_top_doc(L->page) + zpx(MARGIN) + L->y; }

/* পর্দার (x, y) → (অনুচ্ছেদ, বাইট) */
static void hit_test(int x, int y, int *op, int *oi)
{
    long dy = y - view_y0 + scroll_y;
    int g, best = -1, k, pos, bestd = 1 << 30, s, e;
    Para *p;
    Line *L;
    {
        /* ধাপ ২৬খ-২: টেবিলে একই উচ্চতায় কয়েকটা ঘর — আগে x দেখে ঘর বাছা */
        int cx = x - page_left() - zpx(MARGIN), cell_best = -1, r;
        long cell_d = 1L << 40;
        CellRect *in = NULL;
        for (r = 0; r < ncrects; r++) {              /* মাউস কোন ঘরের ভেতরে */
            long t = page_top_doc(crects[r].page) + zpx(MARGIN) + crects[r].y;
            if (dy >= t && dy < t + crects[r].h && cx >= crects[r].x && cx < crects[r].x + crects[r].w) { in = &crects[r]; break; }
        }
        for (g = 0; in && g < ngl; g++) {
            Para *q = &P[gl_p[g]];
            Line *l = &q->ln[gl_l[g]];
            long top = line_top_doc(l), d;
            if (!q->tbl || l->page != in->page || q->cell_x != in->x || l->y < in->y || l->y >= in->y + in->h) continue;
            d = dy < top ? top - dy : dy >= top + l->h ? dy - top - l->h + 1 : 0;
            if (d < cell_d) { cell_d = d; cell_best = g; }
        }
        if (cell_best >= 0) best = cell_best;
    }
    for (g = 0; g < ngl && best < 0; g++) {
        Line *l = &P[gl_p[g]].ln[gl_l[g]];
        long top = line_top_doc(l);
        if (dy < top + l->h || (g + 1 < ngl && P[gl_p[g + 1]].ln[gl_l[g + 1]].page > l->page &&
                                dy < page_top_doc(l->page + 1))) { best = g; break; }
    }
    if (best < 0) best = ngl - 1;
    p = &P[gl_p[best]];
    k = gl_l[best];
    L = &p->ln[k];
    x -= page_left() + zpx(MARGIN);
    s = L->s;
    e = (k < p->nl - 1 && L->e > L->s) ? prev_stop(p, L->e) : L->e;
    pos = s;
    for (;;) {
        int d = x_in_line(p, L, s) - x;
        if (d < 0) d = -d;
        if (d < bestd) { bestd = d; pos = s; }
        if (s >= e) break;
        s = next_stop(p, s);
        if (s > e) s = e;
    }
    *op = gl_p[best];
    *oi = pos;
}

/* ================= আঁকা ================= */

static gfx_surface *itmp;          /* বাঁকা (italic) লেখার জন্য — নিজস্ব italic ফাইল না থাকলে */

static int draw_piece(gfx_surface *s, int ai, const char *t, int n, int x, int baseline)
{
    Face fc;
    const Attr *a = &attrs[ai];
    uint32_t col = 0xFF000000u | a->color;
    int w, y;
    face_of(ai, &fc);
    if (n > 4000) n = 4000;
    memcpy(mbuf, t, (size_t)n);
    mbuf[n] = '\0';
    y = baseline - fc.asc;
    w = gfx_text_width(fc.f, mbuf) + fc.fake_bold;
    if (fc.fake_italic) {
        int tw = w + fc.h / 4 + 4, th = fc.h, r, c;
        if (!itmp || itmp->w < tw || itmp->h < th) {
            if (itmp) gfx_surface_free(itmp);
            itmp = gfx_surface_new(tw > 512 ? tw : 512, th > 96 ? th : 96);
        }
        if (itmp) {
            gfx_reset_clip(itmp);
            gfx_fill_rect(itmp, 0, 0, tw, th, GFX_WHITE);
            gfx_text(itmp, fc.f, 0, 0, mbuf, col);
            if (fc.fake_bold) gfx_text(itmp, fc.f, 1, 0, mbuf, col);
            for (r = 0; r < th; r++) {
                int shift = (fc.asc - r) / 4;
                uint32_t *row = itmp->px + (size_t)r * itmp->stride;
                for (c = 0; c < tw; c++)
                    if ((row[c] & 0xFFFFFF) != 0xFFFFFF) gfx_pixel(s, x + c + shift, y + r, row[c] | 0xFF000000u);
            }
        }
    } else {
        gfx_text(s, fc.f, x, y, mbuf, col);
        if (fc.fake_bold) gfx_text(s, fc.f, x + 1, y, mbuf, col);
    }
    if (a->flags & A_UNDER) {
        int th = fc.h / 16 > 1 ? fc.h / 16 : 1;
        gfx_fill_rect(s, x, baseline + (fc.h / 12 > 1 ? fc.h / 12 : 1), w, th, col);
    }
    if (a->flags & A_STRIKE) {
        int th = fc.h / 16 > 1 ? fc.h / 16 : 1;
        gfx_fill_rect(s, x, baseline - fc.asc * 3 / 10, w, th, col);
    }
    return w;
}

/* নির্বাচনের পরিসর: (p1,i1) < (p2,i2); নির্বাচন না থাকলে 0 */
static int sel_range(int *p1, int *i1, int *p2, int *i2)
{
    if (cp == sp && ci == si) return 0;
    if (sp < cp || (sp == cp && si < ci)) { *p1 = sp; *i1 = si; *p2 = cp; *i2 = ci; }
    else { *p1 = cp; *i1 = ci; *p2 = sp; *i2 = si; }
    return 1;
}

static int list_number(int pi)
{
    int n = 1;
    while (pi > 0 && P[pi - 1].list == 2) { pi--; n++; }
    return n;
}

/* ধাপ ২৬খ-২: ছবি আঁকা (মাপমতো ছোট-বড় করা কপি মনে রাখা হয়) */
static void draw_image(gfx_surface *s, Para *p, int x, int y, int w, int h)
{
    RImg *im = (p->img >= 0 && p->img < nimgs) ? &imgs[p->img] : NULL;
    if (!im || !im->s) {
        gfx_fill_rect(s, x, y, w, h, GFX_RGB(230, 230, 230));
        gfx_rect(s, x, y, w, h, GFX_RGB(150, 150, 150));
        gfx_text(s, NULL, x + 6, y + 6, "(picture)", GFX_RGB(100, 100, 100));
        return;
    }
    if (!im->scaled || im->scaled->w != w || im->scaled->h != h) {
        if (im->scaled) gfx_surface_free(im->scaled);
        im->scaled = gfx_surface_new(w, h);
        if (im->scaled) {
            gfx_clear(im->scaled, GFX_WHITE);
            gfx_blit_scaled(im->scaled, 0, 0, w, h, im->s);
        }
    }
    if (im->scaled) gfx_blit(s, x, y, im->scaled, 0, 0, w, h);
}

static void draw_line(gfx_surface *s, int pi, int k, int sx, int sy)
{
    Para *p = &P[pi];
    Line *L = &p->ln[k];
    int p1, i1, p2, i2, x100, i, baseline = sy + L->asc, te;
    if (p->kind == 1) {                              /* ধাপ ২৬খ-২: ছবি */
        int w, h;
        img_disp(p, &w, &h);
        draw_image(s, p, sx + L->x, sy, w, h);
        if (sel_range(&p1, &i1, &p2, &i2) && pi >= p1 && pi <= p2 && !(pi == p2 && i2 == 0 && p1 < p2)) {
            gfx_fill_rect_alpha(s, sx + L->x, sy, w, h, GFX_RGBA(60, 120, 230, 90));
            gfx_rect(s, sx + L->x - 1, sy - 1, w + 2, h + 2, GFX_RGB(60, 120, 230));
        }
        return;
    }
    /* নির্বাচন */
    if (sel_range(&p1, &i1, &p2, &i2) && pi >= p1 && pi <= p2) {
        int a = pi == p1 ? i1 : 0, b = pi == p2 ? i2 : p->n, xa, xb;
        int to_end = pi < p2 && k == p->nl - 1;          /* অনুচ্ছেদের শেষ — পরের লাইনেও যায় */
        if (a < L->s) a = L->s;
        if (b > L->e) b = L->e;
        if (a < b || (to_end && a <= b)) {
            xa = x_in_line(p, L, a);
            xb = x_in_line(p, L, b) + (to_end ? zpx(6) : 0);
            gfx_fill_rect(s, sx + xa, sy, xb - xa, L->h, GFX_RGB(173, 206, 250));
        }
    }
    /* তালিকার চিহ্ন */
    if (k == 0 && p->list) {
        int ai = p->n ? p->a[0] : p->eattr;
        char mark[16];
        if (p->list == 1) strcpy(mark, "\xE2\x80\xA2");
        else snprintf(mark, sizeof(mark), "%d.", list_number(pi));
        draw_piece(s, ai, mark, (int)strlen(mark), sx + L->x - zpx(LIST_INDENT) + zpx(10), baseline);
    }
    te = L->e;
    while (te > L->s && p->t[te - 1] == ' ') te--;
    x100 = (sx + L->x) * 100;
    for (i = L->s; i < L->e;) {
        if (p->t[i] == ' ') {
            int w = run_w(p->a[i], " ", 1) * 100 + (i < te ? L->extra : 0);
            const Attr *a = &attrs[p->a[i]];
            if ((a->flags & (A_UNDER | A_STRIKE)) && i < te) {      /* শব্দের মাঝের ফাঁকেও দাগ */
                Face fc;
                int th;
                face_of(p->a[i], &fc);
                th = fc.h / 16 > 1 ? fc.h / 16 : 1;
                if (a->flags & A_UNDER)
                    gfx_fill_rect(s, x100 / 100, baseline + (fc.h / 12 > 1 ? fc.h / 12 : 1), w / 100 + 1, th,
                                  0xFF000000u | a->color);
                if (a->flags & A_STRIKE)
                    gfx_fill_rect(s, x100 / 100, baseline - fc.asc * 3 / 10, w / 100 + 1, th, 0xFF000000u | a->color);
            }
            x100 += w;
            i++;
            continue;
        }
        {
            int j = i;
            while (j < L->e && p->t[j] != ' ' && p->a[j] == p->a[i]) j++;
            x100 += draw_piece(s, p->a[i], p->t + i, j - i, x100 / 100, baseline) * 100;
            i = j;
        }
    }
}

/* {page}/{pages} বসানো */
static void hf_expand(const char *src, int page, char *out, size_t n)
{
    size_t o = 0;
    while (*src && o + 12 < n) {
        if (!strncmp(src, "{page}", 6)) { o += (size_t)snprintf(out + o, n - o, "%d", page + 1); src += 6; }
        else if (!strncmp(src, "{pages}", 7)) { o += (size_t)snprintf(out + o, n - o, "%d", npages); src += 7; }
        else out[o++] = *src++;
    }
    out[o] = 0;
}

static int hf_attr(void)
{
    Attr a;
    a.font = (unsigned char)default_family();
    a.flags = 0;
    a.size2 = 20;
    a.color = 0x404040;
    return attr_id(&a);
}

static void draw_header_footer(gfx_surface *s, int pg, int pl, int top)
{
    char t[400];
    int which;
    for (which = 0; which < 2; which++) {
        const char *src = which ? ftr_text : hdr_text;
        int align = which ? ftr_align : hdr_align, ai = hf_attr(), w, x, base;
        Face fc;
        if (!src[0]) continue;
        hf_expand(src, pg, t, sizeof(t));
        w = run_w(ai, t, (int)strlen(t));
        face_of(ai, &fc);
        x = pl + zpx(MARGIN) + (align == 1 ? (content_w() - w) / 2 : align == 2 ? content_w() - w : 0);
        base = which ? top + zpx(PAGE_H) - zpx(MARGIN) / 2 + fc.asc / 2 : top + zpx(MARGIN) / 2 + fc.asc / 2;
        draw_piece(s, ai, t, (int)strlen(t), x, base);
    }
}

static void draw_pages(gfx_surface *s)
{
    int pg, g, pl = page_left(), pw = zpx(PAGE_W), ph = zpx(PAGE_H);
    gfx_fill_rect(s, view_x0, view_y0, view_w, view_h, GFX_RGB(150, 155, 162));
    for (pg = 0; pg < npages; pg++) {
        long top = page_top_doc(pg) - scroll_y + view_y0;
        if (top > view_y0 + view_h || top + ph < view_y0) continue;
        gfx_fill_rect(s, pl + 4, (int)top + 4, pw, ph, GFX_RGB(110, 114, 120));    /* ছায়া */
        gfx_fill_rect(s, pl, (int)top, pw, ph, GFX_WHITE);
    }
    /* ধাপ ২৬খ-২: header/footer আর টেবিলের দাগ */
    for (pg = 0; pg < npages; pg++) {
        long top = page_top_doc(pg) - scroll_y + view_y0;
        int c;
        if (top > view_y0 + view_h || top + ph < view_y0) continue;
        draw_header_footer(s, pg, pl, (int)top);
        for (c = 0; c < ncrects; c++) {
            if (crects[c].page != pg) continue;
            gfx_rect(s, pl + zpx(MARGIN) + crects[c].x, (int)top + zpx(MARGIN) + crects[c].y, crects[c].w + 1,
                     crects[c].h + 1, GFX_RGB(90, 90, 90));
        }
    }
    for (g = 0; g < ngl; g++) {
        Para *p = &P[gl_p[g]];
        Line *L = &p->ln[gl_l[g]];
        long y = line_top_doc(L) - scroll_y + view_y0;
        if (y > view_y0 + view_h || y + L->h < view_y0) continue;
        draw_line(s, gl_p[g], gl_l[g], pl + zpx(MARGIN), (int)y);
    }
    /* কার্সর */
    if (U.blink || dragging) {
        Para *p = &P[cp];
        Line *L = &p->ln[line_of(p, ci)];
        int x = pl + zpx(MARGIN) + x_in_line(p, L, ci);
        long y = line_top_doc(L) - scroll_y + view_y0;
        int h = L->h * 100 / p->spacing;
        gfx_fill_rect(s, x, (int)y, zpx(2) > 1 ? zpx(2) : 1, h, GFX_BLACK);
    }
}

/* ================= লেখা জমানোর বাফার ================= */

typedef struct { char *s; size_t n, cap; } SB;

static void sb_add(SB *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2 + 256;
        b->s = (char *)realloc(b->s, b->cap);
        if (!b->s) { fprintf(stderr, "rozword: out of memory\n"); exit(1); }
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = '\0';
}

static void sb_str(SB *b, const char *s) { sb_add(b, s, strlen(s)); }

/* ================= .rwd লেখা/পড়া ================= */

static void ser_attr(SB *b, int ai)
{
    const Attr *a = &attrs[ai];
    char tmp[128], name[64];
    int i;
    snprintf(name, sizeof(name), "%s", fams[a->font < NFAM ? a->font : 0].name);
    for (i = 0; name[i]; i++) if (name[i] == ' ') name[i] = '_';
    snprintf(tmp, sizeof(tmp), "%s %d %d %06X", name, a->size2, a->flags, (unsigned)(a->color & 0xFFFFFF));
    sb_str(b, tmp);
}

/* ধাপ ২৬খ-২: base64 (ছবি .rwd-তে) */
static const char b64c[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void sb_base64(SB *b, const unsigned char *d, long n)
{
    long i;
    char q[4];
    for (i = 0; i + 2 < n; i += 3) {
        q[0] = b64c[d[i] >> 2]; q[1] = b64c[((d[i] & 3) << 4) | (d[i + 1] >> 4)];
        q[2] = b64c[((d[i + 1] & 15) << 2) | (d[i + 2] >> 6)]; q[3] = b64c[d[i + 2] & 63];
        sb_add(b, q, 4);
    }
    if (n - i == 1) { q[0] = b64c[d[i] >> 2]; q[1] = b64c[(d[i] & 3) << 4]; q[2] = q[3] = '='; sb_add(b, q, 4); }
    else if (n - i == 2) {
        q[0] = b64c[d[i] >> 2]; q[1] = b64c[((d[i] & 3) << 4) | (d[i + 1] >> 4)]; q[2] = b64c[(d[i + 1] & 15) << 2]; q[3] = '=';
        sb_add(b, q, 4);
    }
}

static unsigned char *base64_decode(const char *s, long *outn)
{
    long n = (long)strlen(s), o = 0;
    unsigned char *out = (unsigned char *)malloc((size_t)(n / 4 * 3 + 4));
    int v = 0, bits = 0;
    if (!out) return NULL;
    for (; *s; s++) {
        const char *k = strchr(b64c, *s);
        if (*s == '=' || !k) continue;
        v = (v << 6) | (int)(k - b64c);
        bits += 6;
        if (bits >= 8) { bits -= 8; out[o++] = (unsigned char)(v >> bits); }
    }
    *outn = o;
    return out;
}

/* ছবি ভাণ্ডারে (data-র মালিকানা নেয়); ফেরত নম্বর, ভুলে -1 */
static int img_add(unsigned char *data, long len, const char *ext)
{
    RImg *n;
    if (nimgs % 16 == 0) {
        n = (RImg *)realloc(imgs, sizeof(RImg) * (size_t)(nimgs + 16));
        if (!n) { free(data); return -1; }
        imgs = n;
    }
    n = &imgs[nimgs];
    memset(n, 0, sizeof(*n));
    n->data = data;
    n->len = len;
    snprintf(n->ext, sizeof(n->ext), "%s", ext && *ext ? ext : "png");
    n->s = gfx_load_image_stb(data, len);
    return nimgs++;
}

static int tbl_new(int cols)
{
    RTable *n;
    int c;
    if (ntbls == 0) ntbls = 1;                       /* ০ নম্বর ফাঁকা */
    n = (RTable *)realloc(tbls, sizeof(RTable) * (size_t)(ntbls + 1));
    if (!n) return 0;
    tbls = n;
    if (cols < 1) cols = 1;
    if (cols > TBL_MAXCOL) cols = TBL_MAXCOL;
    memset(&tbls[ntbls], 0, sizeof(RTable));
    tbls[ntbls].cols = cols;
    for (c = 0; c < cols; c++) tbls[ntbls].w[c] = 1000 / cols;
    return ntbls++;
}

static void ser_text(SB *b, const char *t, int n)
{
    int k;
    for (k = 0; k < n; k++) {
        if (t[k] == '\\') sb_str(b, "\\\\");
        else if (t[k] == '\n') sb_str(b, "\\n");
        else sb_add(b, t + k, 1);
    }
}

#define SER_META  1                /* header/footer (পুরো দলিল: সেভ, undo) */
#define SER_EMBED 2                /* ছবির আসল বাইট (ফাইলে সেভ) */

/* (p1,i1)–(p2,i2) অংশ .rwd লেখায় (পুরো দলিল: 0,0 – np-1,শেষ) */
static void serialize_range(SB *b, int p1, int i1, int p2, int i2, int flags)
{
    int p, k;
    char tmp[200];
    unsigned char *tdone = (unsigned char *)calloc((size_t)(ntbls + 1), 1);
    unsigned char *idone = (unsigned char *)calloc((size_t)(nimgs + 1), 1);
    sb_str(b, "rozword 2\n");
    if (flags & SER_META) {
        snprintf(tmp, sizeof(tmp), "h %d ", hdr_align);
        sb_str(b, tmp);
        ser_text(b, hdr_text, (int)strlen(hdr_text));
        snprintf(tmp, sizeof(tmp), "\nf %d ", ftr_align);
        sb_str(b, tmp);
        ser_text(b, ftr_text, (int)strlen(ftr_text));
        sb_str(b, "\n");
    }
    for (p = p1; p <= p2; p++) {
        Para *q = &P[p];
        int a = p == p1 ? i1 : 0, e = p == p2 ? i2 : q->n;
        if (q->tbl > 0 && q->tbl < ntbls && tdone && !tdone[q->tbl]) {   /* টেবিলের বিবরণ আগে */
            RTable *T = &tbls[q->tbl];
            tdone[q->tbl] = 1;
            snprintf(tmp, sizeof(tmp), "T %d %d", q->tbl, T->cols);
            sb_str(b, tmp);
            for (k = 0; k < T->cols; k++) { snprintf(tmp, sizeof(tmp), " %d", T->w[k]); sb_str(b, tmp); }
            sb_str(b, "\n");
        }
        if (q->kind == 1 && (flags & SER_EMBED) && q->img >= 0 && q->img < nimgs && idone && !idone[q->img]) {
            idone[q->img] = 1;                          /* ছবির বাইট (base64) প্রথমবার */
            snprintf(tmp, sizeof(tmp), "i %d %s ", q->img, imgs[q->img].ext);
            sb_str(b, tmp);
            sb_base64(b, imgs[q->img].data, imgs[q->img].len);
            sb_str(b, "\n");
        }
        snprintf(tmp, sizeof(tmp), "p %d %d %d %d %d %d %d %d\n", q->style, q->align, q->list, q->spacing, q->pbreak,
                 q->tbl, q->row, q->col);
        sb_str(b, tmp);
        if (q->kind == 1) {
            snprintf(tmp, sizeof(tmp), "g %d %d %d\n", q->img, q->img_w, q->img_h);
            sb_str(b, tmp);
            continue;
        }
        if (a >= e) {
            sb_str(b, "e ");
            ser_attr(b, q->n ? attr_before(p, a) : q->eattr);
            sb_str(b, "\n");
            continue;
        }
        while (a < e) {
            int j = a;
            while (j < e && q->a[j] == q->a[a]) j++;
            sb_str(b, "r ");
            ser_attr(b, q->a[a]);
            sb_str(b, " ");
            ser_text(b, q->t + a, j - a);
            sb_str(b, "\n");
            a = j;
        }
    }
    free(tdone);
    free(idone);
}

/* "FONT SIZE2 FLAGS COLOR" পড়ে Attr; *rest = পরের অংশ */
static int parse_attr(char *s, char **rest)
{
    Attr a;
    char name[64];
    int size2 = 22, flags = 0, n = 0, f, i;
    unsigned col = 0;
    if (sscanf(s, "%63s %d %d %x%n", name, &size2, &flags, &col, &n) < 4) return default_attr();
    for (i = 0; name[i]; i++) if (name[i] == '_') name[i] = ' ';
    f = fam_by_name(name);
    a.font = (unsigned char)(f >= 0 && fams[f].present ? f : default_family());
    a.size2 = (unsigned short)(size2 < 2 ? 2 : size2 > 800 ? 800 : size2);
    a.flags = (unsigned char)(flags & 15);
    a.color = col & 0xFFFFFF;
    *rest = s + n;
    return attr_id(&a);
}

/* .rwd লেখা → অনুচ্ছেদের নতুন সারি (out); ফেরত সংখ্যা, ভুলে -1 */
/* apply_meta = 1: header/footer-ও বসানো (ফাইল খোলা, undo); paste-এ 0 */
static int map_id(int *from, int *to, int n, int id)
{
    int i;
    for (i = 0; i < n; i++) if (from[i] == id) return to[i];
    return -1;
}

static void unescape(char *s)
{
    char *d = s;
    for (; *s; s++) {
        if (*s == '\\' && s[1] == 'n') { *d++ = '\n'; s++; }
        else if (*s == '\\' && s[1] == '\\') { *d++ = '\\'; s++; }
        else *d++ = *s;
    }
    *d = 0;
}

static int parse_rwd(const char *text, Para **out, int apply_meta)
{
    char *buf = strdup(text), *line, *next;
    Para *A = NULL;
    int n = 0, cap = 0, ok = 0;
    int tfrom[64], tto[64], ntm = 0, ifrom[256], ito[256], nim = 0;
    if (!buf) return -1;
    for (line = buf; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = '\0';
        if (line[0] && line[strlen(line) - 1] == '\r') line[strlen(line) - 1] = '\0';
        if (!ok) {
            if (strncmp(line, "rozword ", 8)) break;
            ok = 1;
            continue;
        }
        if (line[0] == 'p' && line[1] == ' ') {
            int st = 0, al = 0, li = 0, spc = 115, pb = 0;
            if (n == cap) {
                cap = cap ? cap * 2 : 16;
                A = (Para *)realloc(A, (size_t)cap * sizeof(Para));
                if (!A) { free(buf); return -1; }
            }
            para_init(&A[n], default_attr());
            {
                int tb = 0, rw = 0, cl = 0;
                if (sscanf(line + 2, "%d %d %d %d %d %d %d %d", &st, &al, &li, &spc, &pb, &tb, &rw, &cl) >= 8 && tb > 0) {
                    int m = map_id(tfrom, tto, ntm, tb);
                    A[n].tbl = m > 0 ? m : (tb < ntbls ? tb : 0);   /* T লাইন না থাকলে (undo) চলতি নম্বর */
                    A[n].row = rw < 0 ? 0 : rw;
                    A[n].col = cl < 0 ? 0 : cl;
                }
            }
            A[n].style = (unsigned char)(st >= 0 && st <= 4 ? st : 0);
            A[n].align = (unsigned char)(al >= 0 && al <= 3 ? al : 0);
            A[n].list = (unsigned char)(li >= 0 && li <= 2 ? li : 0);
            A[n].spacing = spc >= 80 && spc <= 400 ? spc : 115;
            A[n].pbreak = (unsigned char)(pb != 0);
            n++;
        } else if (line[0] == 'T' && line[1] == ' ') {          /* ধাপ ২৬খ-২: টেবিল */
            int id = 0, cols = 1, k = 0, off = 0, w, nt;
            char *q;
            if (sscanf(line + 2, "%d %d%n", &id, &cols, &off) >= 2 && ntm < 64) {
                nt = tbl_new(cols);
                q = line + 2 + off;
                while (k < tbls[nt].cols && sscanf(q, "%d%n", &w, &off) == 1) { tbls[nt].w[k++] = w > 0 ? w : 1; q += off; }
                tfrom[ntm] = id;
                tto[ntm] = nt;
                ntm++;
            }
        } else if (line[0] == 'i' && line[1] == ' ') {          /* ধাপ ২৬খ-২: ছবির বাইট */
            int id = 0, off = 0;
            char ext[8] = "png";
            if (sscanf(line + 2, "%d %7s %n", &id, ext, &off) >= 2 && nim < 256) {
                long dl = 0;
                unsigned char *d = base64_decode(line + 2 + off, &dl);
                int ni = d ? img_add(d, dl, ext) : -1;
                if (ni >= 0) { ifrom[nim] = id; ito[nim] = ni; nim++; }
            }
        } else if (n > 0 && line[0] == 'g' && line[1] == ' ') {  /* ছবির অনুচ্ছেদ */
            int id = -1, w = 0, h = 0, m;
            sscanf(line + 2, "%d %d %d", &id, &w, &h);
            m = map_id(ifrom, ito, nim, id);
            A[n - 1].kind = 1;
            A[n - 1].img = m >= 0 ? m : (id >= 0 && id < nimgs ? id : -1);
            A[n - 1].img_w = w > 0 ? w : 100;
            A[n - 1].img_h = h > 0 ? h : 100;
        } else if ((line[0] == 'h' || line[0] == 'f') && line[1] == ' ') {   /* header/footer */
            int al = 1, off = 0;
            if (apply_meta && sscanf(line + 2, "%d %n", &al, &off) >= 1) {
                char *t = line + 2 + off;
                unescape(t);
                if (line[0] == 'h') { snprintf(hdr_text, sizeof(hdr_text), "%s", t); hdr_align = al; }
                else { snprintf(ftr_text, sizeof(ftr_text), "%s", t); ftr_align = al; }
            }
        } else if (n > 0 && (line[0] == 'r' || line[0] == 'e') && line[1] == ' ') {
            char *rest = line + 2;
            int ai = parse_attr(line + 2, &rest);
            if (line[0] == 'e') {
                A[n - 1].eattr = (unsigned short)ai;
            } else {
                char *s = rest, *d, *txt;
                if (*s == ' ') s++;
                txt = s;
                for (d = s; *s; s++) {                 /* escape খোলা (একই জায়গায়) */
                    if (*s == '\\' && s[1] == 'n') { *d++ = '\n'; s++; }
                    else if (*s == '\\' && s[1] == '\\') { *d++ = '\\'; s++; }
                    else *d++ = *s;
                }
                *d = '\0';
                para_insert_bytes(&A[n - 1], A[n - 1].n, txt, NULL, (int)(d - txt), ai);
                A[n - 1].eattr = (unsigned short)ai;
            }
        }
    }
    free(buf);
    if (!ok) { free(A); return -1; }
    if (n == 0) {
        A = (Para *)malloc(sizeof(Para));
        para_init(&A[0], default_attr());
        n = 1;
    }
    *out = A;
    return n;
}

/* সাধারণ লেখা → অনুচ্ছেদ (প্রতি লাইনে একটা) */
static int parse_plain(const char *text, size_t len, int ai, const Para *like, Para **out)
{
    Para *A = NULL;
    int n = 0, cap = 0;
    size_t i = 0;
    for (;;) {
        size_t e = i;
        while (e < len && text[e] != '\n') e++;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            A = (Para *)realloc(A, (size_t)cap * sizeof(Para));
            if (!A) return -1;
        }
        para_init(&A[n], ai);
        if (like) {
            A[n].align = like->align;
            A[n].list = like->list;
            A[n].spacing = like->spacing;
            A[n].style = like->style;
        }
        {
            size_t l = e - i;
            if (l > 0 && text[e - 1] == '\r') l--;
            {
                /* ট্যাব → ৪টা ফাঁকা */
                size_t k, st = i;
                for (k = i; k < i + l; k++) {
                    if (text[k] == '\t') {
                        para_insert_bytes(&A[n], A[n].n, text + st, NULL, (int)(k - st), ai);
                        para_insert_bytes(&A[n], A[n].n, "    ", NULL, 4, ai);
                        st = k + 1;
                    }
                }
                para_insert_bytes(&A[n], A[n].n, text + st, NULL, (int)(i + l - st), ai);
            }
        }
        n++;
        if (e >= len) break;
        i = e + 1;
        if (i == len) break;                     /* শেষে '\n' — বাড়তি খালি অনুচ্ছেদ নয় */
    }
    *out = A;
    return n;
}

static void plain_range(SB *b, int p1, int i1, int p2, int i2, int marks)
{
    int p;
    for (p = p1; p <= p2; p++) {
        int a = p == p1 ? i1 : 0, e = p == p2 ? i2 : P[p].n;
        if (marks && P[p].list && a == 0) {
            char m[16];
            if (P[p].list == 1) strcpy(m, "\xE2\x80\xA2 ");
            else snprintf(m, sizeof(m), "%d. ", list_number(p));
            sb_str(b, m);
        }
        if (P[p].kind == 1) sb_str(b, "[picture]");   /* ধাপ ২৬খ-২ */
        else sb_add(b, P[p].t + a, (size_t)(e - a));
        if (p < p2) {                                 /* টেবিলের একই সারির পরের ঘর: Tab */
            if (P[p].tbl && P[p + 1].tbl == P[p].tbl && P[p + 1].row == P[p].row && P[p + 1].col != P[p].col) sb_str(b, "\t");
            else sb_str(b, "\n");
        }
    }
}

static void doc_replace(Para *A, int n)
{
    int i;
    for (i = 0; i < np; i++) para_free(&P[i]);
    free(P);
    P = A;
    np = pcap = n;
    layout_all = 1;
    typing_attr = -1;
}

/* ================= Undo / Redo ================= */

#define UNDO_MAX 100
typedef struct { char *doc; int cp, ci; } Snap;
static Snap undo_s[UNDO_MAX], redo_s[UNDO_MAX];
static int nundo, nredo, last_kind;
static long last_edit_ms;

static void snap_take(Snap *s)
{
    SB b = { 0, 0, 0 };
    serialize_range(&b, 0, 0, np - 1, P[np - 1].n, SER_META);
    s->doc = b.s;
    s->cp = cp;
    s->ci = ci;
}

static void snap_restore(Snap *s)
{
    Para *A;
    int n = parse_rwd(s->doc, &A, 1);
    if (n > 0) doc_replace(A, n);
    cp = sp = s->cp < np ? s->cp : np - 1;
    ci = si = s->ci <= P[cp].n ? s->ci : P[cp].n;
}

static void stack_push(Snap *st, int *n, Snap s)
{
    if (*n == UNDO_MAX) {
        free(st[0].doc);
        memmove(st, st + 1, (UNDO_MAX - 1) * sizeof(Snap));
        (*n)--;
    }
    st[(*n)++] = s;
}

/* পরিবর্তনের আগে ডাকো। kind 1 = লেখা টাইপ (একটানা টাইপ এক ধাপে) */
static void undo_point(int kind)
{
    Snap s;
    long now = gfx_ticks_ms();
    modified = 1;
    if (kind == 1 && last_kind == 1 && now - last_edit_ms < 1500) { last_edit_ms = now; return; }
    last_kind = kind;
    last_edit_ms = now;
    snap_take(&s);
    stack_push(undo_s, &nundo, s);
    while (nredo > 0) free(redo_s[--nredo].doc);
}

static void do_undo(int redo)
{
    Snap cur, s;
    Snap *from = redo ? redo_s : undo_s, *to = redo ? undo_s : redo_s;
    int *nf = redo ? &nredo : &nundo, *nt = redo ? &nundo : &nredo;
    if (*nf == 0) { snprintf(status_msg, sizeof(status_msg), redo ? "Nothing to redo" : "Nothing to undo"); return; }
    snap_take(&cur);
    stack_push(to, nt, cur);
    s = from[--(*nf)];
    snap_restore(&s);
    free(s.doc);
    last_kind = 0;
    modified = 1;
    follow_cursor = 1;
}

/* ================= সম্পাদনা ================= */

/* ধাপ ২৬খ-২ */
static int same_cell(int a, int b)
{
    return P[a].tbl == P[b].tbl && (!P[a].tbl || (P[a].row == P[b].row && P[a].col == P[b].col));
}

/* অনুচ্ছেদ i সরানো — টেবিলের ঘরের শেষ অনুচ্ছেদ হলে খালি লেখার অনুচ্ছেদ করে রাখা (ঘর থাকে);
 * দলিল কখনো খালি হয় না */
static void remove_para_keep_cell(int i)
{
    int alone = P[i].tbl && !((i > 0 && same_cell(i - 1, i)) || (i + 1 < np && same_cell(i + 1, i)));
    if (alone || np == 1) {
        int ea = default_attr();
        Para keep = P[i];
        para_free(&P[i]);
        para_init(&P[i], ea);
        P[i].tbl = keep.tbl;
        P[i].row = keep.row;
        P[i].col = keep.col;
        layout_all = 1;
        return;
    }
    paras_remove(i, i + 1);
}

static void delete_range(int p1, int i1, int p2, int i2)
{
    int p, structural = 0;
    for (p = p1; p <= p2; p++) if (!same_cell(p, p1) || P[p].kind == 1) structural = 1;
    if (structural && p1 != p2) {
        /* ধাপ ২৬খ-২: টেবিল/ছবি জুড়ে নির্বাচন — গঠন রেখে শুধু লেখা মোছা; পুরো বাছা ছবি সরে যায় */
        for (p = p2; p >= p1; p--) {
            int a = p == p1 ? i1 : 0, e = p == p2 ? i2 : P[p].n;
            if (P[p].kind == 1) {
                if ((p > p1 || i1 == 0) && (p < p2 || i2 > 0 || p1 == p2)) remove_para_keep_cell(p);
                continue;
            }
            para_delete_bytes(&P[p], a, e);
            P[p].dirty = 1;
        }
        if (p1 >= np) p1 = np - 1;
        cp = sp = p1;
        ci = si = i1 <= P[p1].n ? i1 : P[p1].n;
        layout_all = 1;
        return;
    }
    if (p1 == p2) {
        para_delete_bytes(&P[p1], i1, i2);
    } else {
        Para *a = &P[p1], *b = &P[p2];
        para_delete_bytes(a, i1, a->n);
        para_insert_bytes(a, a->n, b->t + i2, b->a + i2, b->n - i2, 0);
        paras_remove(p1 + 1, p2 + 1);
    }
    cp = sp = p1;
    ci = si = i1;
}

static int delete_selection(void)
{
    int p1, i1, p2, i2;
    if (!sel_range(&p1, &i1, &p2, &i2)) return 0;
    delete_range(p1, i1, p2, i2);
    return 1;
}

static int cur_attr(void)
{
    if (typing_attr >= 0 && typing_cp == cp && typing_ci == ci) return typing_attr;
    return attr_before(cp, ci);
}

static void split_para(void);

static void insert_plain(const char *s, int n)
{
    int ai;
    if (P[cp].kind == 1) split_para();              /* ধাপ ২৬খ-২: ছবিতে লিখলে পরের নতুন অনুচ্ছেদে */
    ai = cur_attr();
    para_insert_bytes(&P[cp], ci, s, NULL, n, ai);
    ci += n;
    sp = cp;
    si = ci;
    if (typing_attr >= 0) { typing_cp = cp; typing_ci = ci; }
}

/* অনুচ্ছেদের সারি কার্সরে বসানো (paste); F-এর মালিকানা নেয় */
static void insert_paras(Para *F, int nf)
{
    int i, endlen, structural = 0;
    Para *cur;
    if (nf <= 0) return;
    /* ধাপ ২৬খ-২: টেবিলের ঘরে বসালে সব অনুচ্ছেদ সেই ঘরের (টেবিলের ভেতরে টেবিল নয়) */
    for (i = 0; i < nf; i++) {
        if (P[cp].tbl) { F[i].tbl = P[cp].tbl; F[i].row = P[cp].row; F[i].col = P[cp].col; }
        if (F[i].tbl || F[i].kind == 1) structural = 1;
    }
    if (P[cp].kind == 1) structural = 1;
    if (structural) {                                 /* গোটা অনুচ্ছেদ হিসেবে: কার্সরে ভেঙে মাঝে বসানো */
        int at;
        if (P[cp].kind == 1) at = cp + 1;
        else if (ci == 0 && P[cp].n > 0) at = cp;
        else if (ci >= P[cp].n) at = cp + 1;
        else { split_para(); at = cp; }
        paras_insert_at(at, nf);
        for (i = 0; i < nf; i++) P[at + i] = F[i];
        free(F);
        if (!P[cp].tbl && at + nf - 1 == np - 1 && (P[np - 1].tbl || P[np - 1].kind == 1)) {   /* শেষে লেখার জায়গা */
            paras_insert_at(np, 1);
            para_init(&P[np - 1], default_attr());
        }
        cp = at + nf - 1;
        ci = P[cp].n;
        sp = cp;
        si = ci;
        return;
    }
    if (nf == 1) {
        para_insert_bytes(&P[cp], ci, F[0].t, F[0].a, F[0].n, 0);
        ci += F[0].n;
        para_free(&F[0]);
        free(F);
        sp = cp;
        si = ci;
        return;
    }
    cur = &P[cp];
    endlen = F[nf - 1].n;
    /* শেষের অংশ শেষ নতুন অনুচ্ছেদে যায় */
    para_insert_bytes(&F[nf - 1], F[nf - 1].n, cur->t + ci, cur->a + ci, cur->n - ci, 0);
    para_delete_bytes(cur, ci, cur->n);
    para_insert_bytes(cur, cur->n, F[0].t, F[0].a, F[0].n, 0);
    if (cur->n == 0) cur->eattr = F[0].eattr;
    para_free(&F[0]);
    paras_insert_at(cp + 1, nf - 1);
    for (i = 1; i < nf; i++) P[cp + i] = F[i];
    free(F);
    cp += nf - 1;
    ci = endlen;
    sp = cp;
    si = ci;
}

static void split_para(void)
{
    Para *a = &P[cp], *b;
    int ai = cur_attr();                         /* Enter-এর আগে বাছা ফরম্যাট (Ctrl+B বন্ধ ইত্যাদি) সহ */
    int at_end = ci == a->n;
    if (a->kind == 1) {                          /* ধাপ ২৬খ-২: ছবির পরে নতুন খালি অনুচ্ছেদ (একই ঘরে) */
        paras_insert_at(cp + 1, 1);
        a = &P[cp];
        para_init(&P[cp + 1], default_attr());
        P[cp + 1].align = a->align;
        P[cp + 1].tbl = a->tbl;
        P[cp + 1].row = a->row;
        P[cp + 1].col = a->col;
        cp++;
        ci = 0;
        sp = cp;
        si = 0;
        typing_attr = -1;
        return;
    }
    if (a->n == 0 && a->list) { a->list = 0; a->dirty = 1; layout_all = 1; return; }   /* খালি বুলেটে Enter = তালিকা শেষ */
    paras_insert_at(cp + 1, 1);
    a = &P[cp];
    b = &P[cp + 1];
    para_init(b, ai);
    b->align = a->align;
    b->list = a->list;
    b->spacing = a->spacing;
    b->style = a->style;
    b->tbl = a->tbl;                             /* ধাপ ২৬খ-২: একই ঘরে */
    b->row = a->row;
    b->col = a->col;
    para_insert_bytes(b, 0, a->t + ci, a->a + ci, a->n - ci, 0);
    para_delete_bytes(a, ci, a->n);
    if (a->n == 0) a->eattr = (unsigned short)ai;
    if (a->style && at_end) {                    /* শিরোনামের পরে সাধারণ লেখা */
        Attr x = attrs[ai];
        x.size2 = 22;
        x.flags &= (unsigned char)~A_BOLD;
        x.color = 0;
        b->style = 0;
        b->eattr = (unsigned short)attr_id(&x);
    }
    cp++;
    ci = 0;
    sp = cp;
    si = 0;
    typing_attr = -1;
}

static void join_with_next(int p)
{
    Para *a = &P[p], *b = &P[p + 1];
    para_insert_bytes(a, a->n, b->t, b->a, b->n, 0);
    paras_remove(p + 1, p + 2);
}

/* ================= ফরম্যাট ================= */

enum { F_FONT, F_SIZE, F_FLAG_ON, F_FLAG_OFF, F_COLOR, F_SIZE_STEP };

static void attr_change(Attr *a, int op, int arg)
{
    switch (op) {
    case F_FONT: a->font = (unsigned char)arg; break;
    case F_SIZE: a->size2 = (unsigned short)arg; break;
    case F_FLAG_ON: a->flags |= (unsigned char)arg; break;
    case F_FLAG_OFF: a->flags &= (unsigned char)~arg; break;
    case F_COLOR: a->color = (uint32_t)arg; break;
    case F_SIZE_STEP: {
        int s = a->size2 + arg * 2;
        a->size2 = (unsigned short)(s < 2 ? 2 : s > 400 ? 400 : s);
        break;
    }
    }
}

static void apply_attr(int op, int arg)
{
    int p1, i1, p2, i2, p, i;
    if (sel_range(&p1, &i1, &p2, &i2)) {
        undo_point(2);
        for (p = p1; p <= p2; p++) {
            int a = p == p1 ? i1 : 0, e = p == p2 ? i2 : P[p].n;
            Attr x;
            for (i = a; i < e; i++) {
                x = attrs[P[p].a[i]];
                attr_change(&x, op, arg);
                P[p].a[i] = (unsigned short)attr_id(&x);
            }
            if (P[p].n == 0 || (a == 0 && e == P[p].n)) {
                x = attrs[P[p].eattr];
                attr_change(&x, op, arg);
                P[p].eattr = (unsigned short)attr_id(&x);
            }
            P[p].dirty = 1;
        }
    } else {
        Attr x = attrs[cur_attr()];
        attr_change(&x, op, arg);
        typing_attr = attr_id(&x);
        typing_cp = cp;
        typing_ci = ci;
        if (P[cp].n == 0) { P[cp].eattr = (unsigned short)typing_attr; P[cp].dirty = 1; }
    }
}

static void toggle_flag(int flag)
{
    int p1, i1, p2, i2, on;
    if (sel_range(&p1, &i1, &p2, &i2)) on = !(attrs[P[p1].n ? P[p1].a[i1 < P[p1].n ? i1 : P[p1].n - 1] : P[p1].eattr].flags & flag);
    else on = !(attrs[cur_attr()].flags & flag);
    apply_attr(on ? F_FLAG_ON : F_FLAG_OFF, flag);
}

static void para_span(int *a, int *b)
{
    int p1, i1, p2, i2;
    if (sel_range(&p1, &i1, &p2, &i2)) { *a = p1; *b = p2; }
    else { *a = *b = cp; }
}

enum { PA_ALIGN, PA_LIST, PA_SPACING, PA_STYLE, PA_PBREAK };

static void apply_para(int op, int arg)
{
    int a, b, p, i;
    para_span(&a, &b);
    undo_point(2);
    if (op == PA_LIST && P[a].list == arg) arg = 0;          /* আবার চাপলে তালিকা বন্ধ */
    for (p = a; p <= b; p++) {
        Para *q = &P[p];
        switch (op) {
        case PA_ALIGN: q->align = (unsigned char)arg; break;
        case PA_LIST: q->list = (unsigned char)arg; break;
        case PA_SPACING: q->spacing = arg; break;
        case PA_PBREAK: q->pbreak = (unsigned char)!q->pbreak; break;
        case PA_STYLE: {
            static const int size2[5] = { 22, 52, 40, 32, 26 };
            Attr x;
            q->style = (unsigned char)arg;
            for (i = -1; i < q->n; i++) {
                x = attrs[i < 0 ? q->eattr : q->a[i]];
                x.size2 = (unsigned short)size2[arg];
                if (arg >= 2) x.flags |= A_BOLD; else x.flags &= (unsigned char)~A_BOLD;
                x.color = arg >= 2 ? 0x2F5496 : 0x000000;
                if (i < 0) q->eattr = (unsigned short)attr_id(&x);
                else q->a[i] = (unsigned short)attr_id(&x);
            }
            break;
        }
        }
        q->dirty = 1;
    }
    typing_attr = -1;
    layout_all = 1;
}

/* ================= খোঁজা ================= */

static char find_text[256], repl_text[256];

static int match_at(const Para *p, int i, const char *q, int ql)
{
    int k;
    if (i + ql > p->n) return 0;
    for (k = 0; k < ql; k++) {
        unsigned char a = (unsigned char)p->t[i + k], b = (unsigned char)q[k];
        if (a < 0x80 && b < 0x80) { if (tolower(a) != tolower(b)) return 0; }
        else if (a != b) return 0;
    }
    return 1;
}

static int find_next(void)
{
    int ql = (int)strlen(find_text), p, i, start, pass;
    int p1, i1, p2, i2;
    if (!ql) return 0;
    if (sel_range(&p1, &i1, &p2, &i2)) { p = p2; start = i2; } else { p = cp; start = ci; }
    for (pass = 0; pass <= np; pass++) {
        int pp = (p + pass) % np;
        for (i = pass == 0 ? start : 0; i + ql <= P[pp].n; i++) {
            if (match_at(&P[pp], i, find_text, ql)) {
                sp = pp; si = i; cp = pp; ci = i + ql;
                follow_cursor = 1;
                snprintf(status_msg, sizeof(status_msg), "Found \"%s\" - F3 for the next one", find_text);
                return 1;
            }
        }
    }
    snprintf(status_msg, sizeof(status_msg), "\"%s\" not found", find_text);
    return 0;
}

static int replace_all(void)
{
    int ql = (int)strlen(find_text), rl = (int)strlen(repl_text), p, i, count = 0;
    if (!ql) return 0;
    undo_point(2);
    for (p = 0; p < np; p++) {
        for (i = 0; i + ql <= P[p].n;) {
            if (match_at(&P[p], i, find_text, ql)) {
                int ai = P[p].a[i];
                para_delete_bytes(&P[p], i, i + ql);
                para_insert_bytes(&P[p], i, repl_text, NULL, rl, ai);
                i += rl;
                count++;
            } else {
                i++;
            }
        }
    }
    cp = sp = 0;
    ci = si = 0;
    snprintf(status_msg, sizeof(status_msg), "%d replaced", count);
    return count;
}

static int word_count(void)
{
    int p, i, n = 0;
    for (p = 0; p < np; p++) {
        int in = 0;
        for (i = 0; i < P[p].n; i++) {
            int sp_ = P[p].t[i] == ' ';
            if (!sp_ && !in) n++;
            in = !sp_;
        }
    }
    return n;
}

/* ================= ফাইল ================= */

static int has_ext(const char *n, const char *e)
{
    size_t a = strlen(n), b = strlen(e);
    return a > b && !strcasecmp(n + a - b, e);
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static char *read_file(const char *p, size_t *len)
{
    FILE *f = fopen(p, "rb");
    char *b;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 64L * 1024 * 1024) { fclose(f); return NULL; }
    b = (char *)malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    if (b) { b[n] = '\0'; *len = (size_t)n; }
    return b;
}

static void reset_undo(void)
{
    while (nundo > 0) free(undo_s[--nundo].doc);
    while (nredo > 0) free(redo_s[--nredo].doc);
    last_kind = 0;
}

/* ================= ধাপ ২৬খ-৩: Word (.docx) খোলা আর সেভ =================
 *
 * .docx = ZIP-এর ভেতরে XML (Office Open XML)। খোলা: word/document.xml-এর অনুচ্ছেদ
 * (w:p), লেখার অংশ (w:r — bold, italic, underline, strike, রং, মাপ, ফন্ট), alignment,
 * Title/Heading (styles.xml-এর নাম দেখে), বুলেট/নম্বর (numbering.xml), লাইনের ফাঁক, পাতা ভাঙা,
 * টেবিল (w:tbl — কলামের চওড়া tblGrid থেকে), ছবি (w:drawing → media/…), header/footer
 * (PAGE/NUMPAGES ফিল্ড → {page}/{pages})। যা rozword চেনে না তা বাদ পড়ে (লেখা থাকে)।
 * সেভ: একই জিনিসগুলো দিয়ে নতুন .docx (Word আর LibreOffice খোলে)। */

static int cli_mode;               /* rozword -c (উইন্ডো ছাড়া রূপান্তর) — বার্তা stderr-এ */
static void table_span(int i, int *a, int *b);
static int table_rows(int a, int b);

static void report_error(const char *msg)
{
    if (cli_mode) fprintf(stderr, "rozword: %s\n", msg);
    else ui_msgbox("rozword", msg, UI_MB_OK | UI_MB_ERROR);
}

/* ---------- ZIP পড়া ---------- */

typedef struct { char name[200]; long off, csize, usize; int method; } ZEnt;
typedef struct { const unsigned char *b; long n; ZEnt *e; int ne; } Zip;

static unsigned rd16(const unsigned char *p) { return (unsigned)(p[0] | p[1] << 8); }
static unsigned long rd32(const unsigned char *p) { return (unsigned long)p[0] | (unsigned long)p[1] << 8 | (unsigned long)p[2] << 16 | (unsigned long)p[3] << 24; }

static int zip_open(Zip *z, const unsigned char *b, long n)
{
    long i, cd, cnt, k, pos;
    memset(z, 0, sizeof(*z));
    z->b = b;
    z->n = n;
    for (i = n - 22; i >= 0 && i >= n - 65600; i--) if (rd32(b + i) == 0x06054b50UL) break;
    if (i < 0 || i < n - 65600) return -1;
    cnt = (long)rd16(b + i + 10);
    cd = (long)rd32(b + i + 16);
    if (cd < 0 || cd >= n) return -1;
    z->e = (ZEnt *)calloc((size_t)(cnt > 0 ? cnt : 1), sizeof(ZEnt));
    if (!z->e) return -1;
    for (pos = cd, k = 0; k < cnt && pos + 46 <= n && rd32(b + pos) == 0x02014b50UL; k++) {
        unsigned nl = rd16(b + pos + 28), el = rd16(b + pos + 30), cl = rd16(b + pos + 32);
        ZEnt *e = &z->e[z->ne++];
        e->method = (int)rd16(b + pos + 10);
        e->csize = (long)rd32(b + pos + 20);
        e->usize = (long)rd32(b + pos + 24);
        e->off = (long)rd32(b + pos + 42);
        snprintf(e->name, sizeof(e->name), "%.*s", (int)(nl < sizeof(e->name) ? nl : sizeof(e->name) - 1), (const char *)b + pos + 46);
        pos += 46 + nl + el + cl;
    }
    return z->ne > 0 ? 0 : -1;
}

/* ZIP-এর ভেতরের ফাইল (malloc, শেষে '\0'); না থাকলে NULL */
static char *zip_get(Zip *z, const char *name, long *len)
{
    int i;
    for (i = 0; i < z->ne; i++) {
        ZEnt *e = &z->e[i];
        const unsigned char *lh;
        long data;
        char *out;
        if (strcasecmp(e->name, name)) continue;
        if (e->off + 30 > z->n) return NULL;
        lh = z->b + e->off;
        if (rd32(lh) != 0x04034b50UL) return NULL;
        data = e->off + 30 + (long)rd16(lh + 26) + (long)rd16(lh + 28);
        if (data + e->csize > z->n || e->usize > 256L * 1024 * 1024) return NULL;
        out = (char *)malloc((size_t)e->usize + 1);
        if (!out) return NULL;
        if (e->method == 0) {
            memcpy(out, z->b + data, (size_t)e->csize);
        } else if (e->method == 8) {
            z_stream s;
            memset(&s, 0, sizeof(s));
            if (inflateInit2(&s, -15) != Z_OK) { free(out); return NULL; }
            s.next_in = (unsigned char *)(z->b + data);
            s.avail_in = (uInt)e->csize;
            s.next_out = (unsigned char *)out;
            s.avail_out = (uInt)e->usize;
            inflate(&s, Z_FINISH);
            inflateEnd(&s);
        } else {
            free(out);
            return NULL;
        }
        out[e->usize] = '\0';
        if (len) *len = e->usize;
        return out;
    }
    return NULL;
}

/* ---------- XML পড়া (ছোট, শুধু যা লাগে) ---------- */

enum { X_EOF, X_OPEN, X_CLOSE, X_EMPTY, X_TEXT };
typedef struct {
    int t;
    char name[64];                 /* "w:p" — namespace prefix সহ */
    const char *at;                /* attribute-এর শুরু (tag-এর ভেতরে) */
    int atn;
    const char *tx;                /* X_TEXT */
    int txn;
} XTok;
typedef struct { const char *p, *e; } XRd;

static int x_next(XRd *r, XTok *t)
{
    const char *p = r->p;
    memset(t, 0, sizeof(*t));
    if (p >= r->e) return t->t = X_EOF;
    if (*p != '<') {
        const char *q = memchr(p, '<', (size_t)(r->e - p));
        if (!q) q = r->e;
        t->tx = p;
        t->txn = (int)(q - p);
        r->p = q;
        return t->t = X_TEXT;
    }
    if (p + 4 <= r->e && !strncmp(p, "<!--", 4)) {                 /* মন্তব্য */
        const char *q = strstr(p, "-->");
        r->p = q ? q + 3 : r->e;
        return x_next(r, t);
    }
    if (p + 9 <= r->e && !strncmp(p, "<![CDATA[", 9)) {
        const char *q = strstr(p, "]]>");
        t->tx = p + 9;
        t->txn = (int)((q ? q : r->e) - (p + 9));
        r->p = q ? q + 3 : r->e;
        return t->t = X_TEXT;
    }
    if (p + 1 < r->e && (p[1] == '?' || p[1] == '!')) {             /* <?xml ...?>, <!DOCTYPE> */
        const char *q = memchr(p, '>', (size_t)(r->e - p));
        r->p = q ? q + 1 : r->e;
        return x_next(r, t);
    }
    {
        const char *q = p + 1, *ns, *end;
        int close = 0, n = 0;
        char quote = 0;
        if (*q == '/') { close = 1; q++; }
        ns = q;
        while (q < r->e && *q != '>' && *q != '/' && *q != ' ' && *q != '\t' && *q != '\r' && *q != '\n') q++;
        n = (int)(q - ns);
        snprintf(t->name, sizeof(t->name), "%.*s", n < 63 ? n : 63, ns);
        t->at = q;
        for (end = q; end < r->e; end++) {                          /* '>' (উদ্ধৃতির বাইরে) */
            if (quote) { if (*end == quote) quote = 0; continue; }
            if (*end == '"' || *end == '\'') quote = *end;
            else if (*end == '>') break;
        }
        t->atn = (int)(end - q);
        r->p = end < r->e ? end + 1 : r->e;
        if (close) return t->t = X_CLOSE;
        if (end > q && end[-1] == '/') { t->atn--; return t->t = X_EMPTY; }
        return t->t = X_OPEN;
    }
}

/* &amp; ইত্যাদি খুলে out-এ (UTF-8) */
static void x_decode(SB *out, const char *s, int n)
{
    int i = 0;
    while (i < n) {
        if (s[i] == '&') {
            const char *semi = memchr(s + i, ';', (size_t)(n - i));
            if (semi && semi - (s + i) < 12) {
                int l = (int)(semi - (s + i)) + 1;
                char buf[8];
                unsigned long c = 0;
                int ok = 1;
                if (!strncmp(s + i, "&lt;", 4)) c = '<';
                else if (!strncmp(s + i, "&gt;", 4)) c = '>';
                else if (!strncmp(s + i, "&amp;", 5)) c = '&';
                else if (!strncmp(s + i, "&quot;", 6)) c = '"';
                else if (!strncmp(s + i, "&apos;", 6)) c = '\'';
                else if (s[i + 1] == '#') c = s[i + 2] == 'x' ? strtoul(s + i + 3, NULL, 16) : strtoul(s + i + 2, NULL, 10);
                else ok = 0;
                if (ok && c) {
                    int k = 0;
                    if (c < 0x80) buf[k++] = (char)c;
                    else if (c < 0x800) { buf[k++] = (char)(0xC0 | (c >> 6)); buf[k++] = (char)(0x80 | (c & 63)); }
                    else if (c < 0x10000) { buf[k++] = (char)(0xE0 | (c >> 12)); buf[k++] = (char)(0x80 | ((c >> 6) & 63)); buf[k++] = (char)(0x80 | (c & 63)); }
                    else { buf[k++] = (char)(0xF0 | (c >> 18)); buf[k++] = (char)(0x80 | ((c >> 12) & 63)); buf[k++] = (char)(0x80 | ((c >> 6) & 63)); buf[k++] = (char)(0x80 | (c & 63)); }
                    sb_add(out, buf, (size_t)k);
                    i += l;
                    continue;
                }
            }
        }
        sb_add(out, s + i, 1);
        i++;
    }
}

/* attribute-এর মান (decode করে); না থাকলে 0। name-এর prefix না মিললেও local নাম মিলে যায় */
static int x_attr(const XTok *t, const char *name, char *out, int size)
{
    const char *p = t->at, *e = t->at + t->atn;
    size_t nl = strlen(name);
    const char *local = strchr(name, ':');
    local = local ? local + 1 : name;
    while (p < e) {
        const char *ks, *ke, *vs, *ve;
        char q;
        while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == '/')) p++;
        ks = p;
        while (p < e && *p != '=' && *p != ' ') p++;
        ke = p;
        while (p < e && *p != '"' && *p != '\'') p++;
        if (p >= e) break;
        q = *p++;
        vs = p;
        while (p < e && *p != q) p++;
        ve = p;
        if (p < e) p++;
        {
            size_t kl = (size_t)(ke - ks);
            const char *kc = memchr(ks, ':', kl);
            const char *klocal = kc ? kc + 1 : ks;
            if ((kl == nl && !strncmp(ks, name, nl)) || ((size_t)(ke - klocal) == strlen(local) && !strncmp(klocal, local, strlen(local)))) {
                SB d = { 0, 0, 0 };
                x_decode(&d, vs, (int)(ve - vs));
                snprintf(out, (size_t)size, "%s", d.s ? d.s : "");
                free(d.s);
                return 1;
            }
        }
    }
    if (size > 0) out[0] = 0;
    return 0;
}

static int x_is(const XTok *t, const char *name) { return !strcmp(t->name, name); }

/* w:b, w:i — w:val="0"/"false"/"none" হলে বন্ধ */
static int x_onoff(const XTok *t)
{
    char v[16];
    if (!x_attr(t, "w:val", v, sizeof(v))) return 1;
    return !(!strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "off") || !strcmp(v, "none"));
}

/* ---------- .docx → অনুচ্ছেদ ---------- */

typedef struct { char id[32]; char target[160]; } DxRel;
typedef struct { char id[64]; int style, numid, list; } DxStyle;   /* style 1 Title, 2-4 Heading 1-3; তালিকার style-এ numId */
typedef struct { int num, abs; } DxNum;
typedef struct { int abs, fmt; } DxAbs;                        /* fmt 1 bullet, 2 নম্বর */

typedef struct {
    Zip z;
    DxRel *rel; int nrel;
    DxStyle *sty; int nsty;
    DxNum *num; int nnum;
    DxAbs *abs; int nabs;
    int def_size2;
    char def_font[64];
    Para *A; int n, cap;
    int skipped;                   /* চেনা হয়নি এমন জিনিস (বার্তার জন্য) */
} Dx;

static void dx_rels(Dx *x, const char *file)
{
    long len;
    char *s = zip_get(&x->z, file, &len);
    XRd r;
    XTok t;
    if (!s) return;
    r.p = s;
    r.e = s + len;
    while (x_next(&r, &t) != X_EOF) {
        if ((t.t == X_EMPTY || t.t == X_OPEN) && x_is(&t, "Relationship")) {
            DxRel *n = (DxRel *)realloc(x->rel, sizeof(DxRel) * (size_t)(x->nrel + 1));
            if (!n) break;
            x->rel = n;
            x_attr(&t, "Id", n[x->nrel].id, sizeof(n[x->nrel].id));
            x_attr(&t, "Target", n[x->nrel].target, sizeof(n[x->nrel].target));
            x->nrel++;
        }
    }
    free(s);
}

static const char *dx_rel(Dx *x, const char *id)
{
    int i;
    for (i = 0; i < x->nrel; i++) if (!strcmp(x->rel[i].id, id)) return x->rel[i].target;
    return NULL;
}

/* word/ থেকে আপেক্ষিক পথ → ZIP-এর নাম */
static void dx_path(const char *target, char *out, int size)
{
    if (target[0] == '/') snprintf(out, (size_t)size, "%s", target + 1);
    else if (!strncmp(target, "../", 3)) snprintf(out, (size_t)size, "%s", target + 3);
    else snprintf(out, (size_t)size, "word/%s", target);
}

static void dx_styles(Dx *x)
{
    long len;
    char *s = zip_get(&x->z, "word/styles.xml", &len), v[64];
    XRd r;
    XTok t;
    int in_def = 0;
    DxStyle cur;
    int in_style = 0;
    if (!s) return;
    memset(&cur, 0, sizeof(cur));
    r.p = s;
    r.e = s + len;
    while (x_next(&r, &t) != X_EOF) {
        if (t.t == X_OPEN && x_is(&t, "w:rPrDefault")) in_def = 1;
        else if (t.t == X_CLOSE && x_is(&t, "w:rPrDefault")) in_def = 0;
        else if (in_def && (t.t == X_EMPTY || t.t == X_OPEN) && x_is(&t, "w:sz") && x_attr(&t, "w:val", v, sizeof(v))) {
            int sz = atoi(v);
            if (sz >= 8 && sz <= 144) x->def_size2 = sz;
        } else if (in_def && (t.t == X_EMPTY || t.t == X_OPEN) && x_is(&t, "w:rFonts")) {
            if (!x_attr(&t, "w:ascii", x->def_font, sizeof(x->def_font))) x_attr(&t, "w:hAnsi", x->def_font, sizeof(x->def_font));
        } else if (t.t == X_OPEN && x_is(&t, "w:style")) {
            memset(&cur, 0, sizeof(cur));
            x_attr(&t, "w:styleId", cur.id, sizeof(cur.id));
            in_style = 1;
        } else if (in_style && (t.t == X_EMPTY || t.t == X_OPEN) && x_is(&t, "w:name") && x_attr(&t, "w:val", v, sizeof(v))) {
            if (!strcasecmp(v, "title")) cur.style = 1;
            else if (!strcasecmp(v, "heading 1")) cur.style = 2;
            else if (!strcasecmp(v, "heading 2")) cur.style = 3;
            else if (!strcasecmp(v, "heading 3")) cur.style = 4;
            else if (!strncasecmp(v, "list bullet", 11)) cur.list = 1;   /* numbering না থাকলেও */
            else if (!strncasecmp(v, "list number", 11)) cur.list = 2;
        } else if (in_style && (t.t == X_EMPTY || t.t == X_OPEN) && x_is(&t, "w:numId") && x_attr(&t, "w:val", v, sizeof(v))) {
            cur.numid = atoi(v);
        } else if (t.t == X_CLOSE && x_is(&t, "w:style")) {
            in_style = 0;
            if (cur.id[0] && (cur.style || cur.numid || cur.list)) {
                DxStyle *n = (DxStyle *)realloc(x->sty, sizeof(DxStyle) * (size_t)(x->nsty + 1));
                if (n) { x->sty = n; n[x->nsty++] = cur; }
            }
        }
    }
    free(s);
}

static void dx_numbering(Dx *x)
{
    long len;
    char *s = zip_get(&x->z, "word/numbering.xml", &len), v[32];
    XRd r;
    XTok t;
    int cur_abs = -1, cur_num = -1, lvl = -1, got = 0;
    if (!s) return;
    r.p = s;
    r.e = s + len;
    while (x_next(&r, &t) != X_EOF) {
        if (t.t == X_OPEN && x_is(&t, "w:abstractNum") && x_attr(&t, "w:abstractNumId", v, sizeof(v))) { cur_abs = atoi(v); got = 0; }
        else if (t.t == X_OPEN && x_is(&t, "w:lvl") && x_attr(&t, "w:ilvl", v, sizeof(v))) lvl = atoi(v);
        else if ((t.t == X_EMPTY || t.t == X_OPEN) && x_is(&t, "w:numFmt") && cur_abs >= 0 && lvl == 0 && !got &&
                 x_attr(&t, "w:val", v, sizeof(v))) {
            DxAbs *n = (DxAbs *)realloc(x->abs, sizeof(DxAbs) * (size_t)(x->nabs + 1));
            if (n) {
                x->abs = n;
                n[x->nabs].abs = cur_abs;
                n[x->nabs].fmt = !strcmp(v, "bullet") || !strcmp(v, "none") ? 1 : 2;
                x->nabs++;
                got = 1;
            }
        } else if (t.t == X_OPEN && x_is(&t, "w:num") && x_attr(&t, "w:numId", v, sizeof(v))) cur_num = atoi(v);
        else if ((t.t == X_EMPTY || t.t == X_OPEN) && x_is(&t, "w:abstractNumId") && cur_num >= 0 && x_attr(&t, "w:val", v, sizeof(v))) {
            DxNum *n = (DxNum *)realloc(x->num, sizeof(DxNum) * (size_t)(x->nnum + 1));
            if (n) {
                x->num = n;
                n[x->nnum].num = cur_num;
                n[x->nnum].abs = atoi(v);
                x->nnum++;
            }
            cur_num = -1;
        }
    }
    free(s);
}

static int dx_list_kind(Dx *x, int numid)
{
    int i, j;
    if (numid <= 0) return 0;
    for (i = 0; i < x->nnum; i++)
        if (x->num[i].num == numid)
            for (j = 0; j < x->nabs; j++) if (x->abs[j].abs == x->num[i].abs) return x->abs[j].fmt;
    return 1;
}

/* বড়-ছোট হাতের অক্ষর না দেখে খোঁজা */
static int strcasestr_(const char *h, const char *n)
{
    size_t nl = strlen(n);
    for (; *h; h++) if (!strncasecmp(h, n, nl)) return 1;
    return 0;
}

/* Word-এর ফন্টের নাম → rozword-এর পরিবার */
static int font_for_name(const char *n)
{
    int f;
    if (!n || !*n) return -1;
    if ((f = fam_by_name(n)) >= 0 && fams[f].present) return f;
    if (strcasestr_(n, "mono") || strcasestr_(n, "courier") || strcasestr_(n, "consolas")) f = fam_by_name("JetBrains Mono");
    else if (strcasestr_(n, "bangla") || strcasestr_(n, "bengali") || strcasestr_(n, "vrinda") || strcasestr_(n, "nikosh") ||
             strcasestr_(n, "solaiman") || strcasestr_(n, "kalpurush") || strcasestr_(n, "siyam")) f = fam_by_name("Noto Sans Bengali");
    else if (strcasestr_(n, "times") || strcasestr_(n, "georgia") || strcasestr_(n, "cambria") || strcasestr_(n, "garamond") ||
             strcasestr_(n, "serif") || strcasestr_(n, "book")) f = fam_by_name("Noto Serif");
    else f = -1;
    return (f >= 0 && fams[f].present) ? f : -1;
}

static Para *dx_new_para(Dx *x, int ai)
{
    if (x->n == x->cap) {
        Para *n;
        x->cap = x->cap ? x->cap * 2 : 64;
        n = (Para *)realloc(x->A, sizeof(Para) * (size_t)x->cap);
        if (!n) { fprintf(stderr, "rozword: out of memory\n"); exit(1); }
        x->A = n;
    }
    para_init(&x->A[x->n], ai);
    return &x->A[x->n++];
}

static const int style_size2[5] = { 22, 52, 40, 32, 26 };

/* একটা অংশের (document বা header/footer) XML → অনুচ্ছেদ; hf != NULL হলে শুধু লেখা জমায় */
typedef struct {
    Attr base;                     /* অনুচ্ছেদের style থেকে */
    Attr run;
    char ascii[64], cs[64];
    int has_font, has_size, has_bold, has_color;
} DxRun;

static int is_bangla_family(int f);

/* বাংলা লেখায় w:cs ফন্টটা বাংলা হলে সেটা, নইলে w:ascii (বাংলার অক্ষর এমনিতেই বাংলা ফন্ট থেকে আসে) */
static int dx_run_attr(DxRun *rn, int bengali)
{
    Attr a = rn->run;
    int f = -1;
    if (bengali && rn->cs[0]) {
        f = font_for_name(rn->cs);
        if (f >= 0 && !is_bangla_family(f)) f = -1;
    }
    if (f < 0) f = font_for_name(rn->ascii[0] ? rn->ascii : rn->cs);
    if (f >= 0) a.font = (unsigned char)f;
    return attr_id(&a);
}

static int has_bengali(const char *s, int n)
{
    int i;
    for (i = 0; i + 2 < n; i++) if ((unsigned char)s[i] == 0xE0 && ((unsigned char)s[i + 1] == 0xA6 || (unsigned char)s[i + 1] == 0xA7)) return 1;
    return 0;
}

static void dx_body(Dx *x, const char *xml, long len, SB *hf, int *hf_align)
{
    XRd r;
    XTok t;
    char v[200];
    Para *cur = NULL;
    DxRun rn;
    int in_ppr = 0, in_rpr = 0, in_t = 0, skip = 0, in_instr = 0, fld_skip = 0, fld_kind = 0, pend_pbreak = 0;
    int tdepth = 0, tid = 0, row = -1, col = -1, cell_paras = 0, gspan = 1, grid_w[TBL_MAXCOL], ngrid = 0, maxcol = 0;
    int para_style = 0, para_align = 0, para_list = 0, para_spacing = 115, para_pb = 0;
    char img_id[64] = "";
    long img_cx = 0, img_cy = 0;
    memset(&rn, 0, sizeof(rn));
    r.p = xml;
    r.e = xml + len;
    while (x_next(&r, &t) != X_EOF) {
        int open = t.t == X_OPEN || t.t == X_EMPTY;
        if (skip) {                                    /* w:del (মুছে ফেলা লেখা), w:instrText ইত্যাদি */
            if (t.t == X_OPEN) skip++;
            else if (t.t == X_CLOSE) skip--;
            continue;
        }
        if (t.t == X_TEXT) {
            if (in_instr) {                            /* ফিল্ড: PAGE / NUMPAGES */
                SB d = { 0, 0, 0 };
                x_decode(&d, t.tx, t.txn);
                if (d.s && strstr(d.s, "NUMPAGES")) fld_kind = 2;
                else if (d.s && strstr(d.s, "PAGE")) fld_kind = 1;
                free(d.s);
            } else if (in_t && !fld_skip) {
                SB d = { 0, 0, 0 };
                x_decode(&d, t.tx, t.txn);
                if (d.s && d.n) {
                    if (hf) sb_add(hf, d.s, d.n);
                    else if (cur) {
                        int ai = dx_run_attr(&rn, has_bengali(d.s, (int)d.n));
                        para_insert_bytes(cur, cur->n, d.s, NULL, (int)d.n, ai);
                        cur->eattr = (unsigned short)ai;
                    }
                }
                free(d.s);
            }
            continue;
        }
        if (open && (x_is(&t, "w:del") || x_is(&t, "w:delText") || x_is(&t, "w:moveFrom"))) {
            if (t.t == X_OPEN) skip = 1;
            continue;
        }
        /* ---- টেবিল ---- */
        if (x_is(&t, "w:tbl")) {
            if (t.t == X_OPEN) {
                tdepth++;
                if (tdepth == 1 && !hf) { tid = tbl_new(1); row = -1; ngrid = 0; maxcol = 0; }
            } else if (t.t == X_CLOSE && tdepth > 0) {
                if (tdepth == 1 && tid) {
                    int c, cols = maxcol > 0 ? maxcol : 1, tot = 0;
                    if (cols > TBL_MAXCOL) cols = TBL_MAXCOL;
                    tbls[tid].cols = cols;
                    for (c = 0; c < cols; c++) tot += c < ngrid ? grid_w[c] : 0;
                    for (c = 0; c < cols; c++) tbls[tid].w[c] = (c < ngrid && tot > 0 && grid_w[c] > 0) ? (int)((long)grid_w[c] * 1000 / tot) : 1000 / cols;
                    tid = 0;
                }
                tdepth--;
                cur = NULL;
            }
            continue;
        }
        if (tdepth == 1 && !hf) {
            if (open && x_is(&t, "w:gridCol") && ngrid < TBL_MAXCOL && x_attr(&t, "w:w", v, sizeof(v))) { grid_w[ngrid++] = atoi(v); continue; }
            if (t.t == X_OPEN && x_is(&t, "w:tr")) { row++; col = -1; gspan = 1; continue; }
            if (t.t == X_OPEN && x_is(&t, "w:tc")) { col += gspan; gspan = 1; cell_paras = 0; continue; }
            if (open && x_is(&t, "w:gridSpan") && x_attr(&t, "w:val", v, sizeof(v))) { gspan = atoi(v) > 1 ? atoi(v) : 1; continue; }
            if (t.t == X_CLOSE && x_is(&t, "w:tc")) {
                if (!cell_paras) {                     /* খালি ঘর */
                    Para *q = dx_new_para(x, attr_id(&rn.base));
                    q->tbl = tid; q->row = row; q->col = col < TBL_MAXCOL ? col : TBL_MAXCOL - 1;
                }
                if (col + gspan > maxcol) maxcol = col + gspan;
                cur = NULL;
                continue;
            }
        }
        /* ---- অনুচ্ছেদ ---- */
        if (x_is(&t, "w:p")) {
            if (open) {
                para_style = 0; para_align = 0; para_list = 0; para_spacing = 115; para_pb = pend_pbreak;
                pend_pbreak = 0;
                memset(&rn.base, 0, sizeof(rn.base));
                rn.base.font = (unsigned char)(font_for_name(x->def_font) >= 0 ? font_for_name(x->def_font) : default_family());
                rn.base.size2 = (unsigned short)(x->def_size2 ? x->def_size2 : 22);
                if (!hf) {
                    cur = dx_new_para(x, attr_id(&rn.base));
                    cur->pbreak = (unsigned char)para_pb;
                    if (tdepth >= 1 && tid) {
                        cur->tbl = tid; cur->row = row; cur->col = col < TBL_MAXCOL ? col : TBL_MAXCOL - 1;
                        cell_paras++;
                    }
                } else if (hf->n) {
                    sb_str(hf, " ");
                }
                if (t.t == X_EMPTY) cur = NULL;
            } else {
                cur = NULL;
            }
            continue;
        }
        if (x_is(&t, "w:pPr")) { in_ppr = t.t == X_OPEN; continue; }
        if (in_ppr && open && !in_rpr) {
            if (x_is(&t, "w:pStyle") && x_attr(&t, "w:val", v, sizeof(v))) {
                int i;
                for (i = 0; i < x->nsty; i++) {
                    if (strcmp(x->sty[i].id, v)) continue;
                    para_style = x->sty[i].style;
                    if (x->sty[i].numid > 0) para_list = dx_list_kind(x, x->sty[i].numid);   /* "List Bullet" ধাঁচের style */
                    else if (x->sty[i].list) para_list = x->sty[i].list;
                }
                if (!para_style) {                     /* id নিজেই "Heading1"/"Title" */
                    if (!strcasecmp(v, "Title")) para_style = 1;
                    else if (!strncasecmp(v, "Heading", 7) && v[7] >= '1' && v[7] <= '3' && !v[8]) para_style = v[7] - '0' + 1;
                }
                if (para_style) {
                    rn.base.size2 = (unsigned short)style_size2[para_style];
                    if (para_style >= 2) { rn.base.flags |= A_BOLD; rn.base.color = 0x2F5496; }
                }
            } else if (x_is(&t, "w:jc") && x_attr(&t, "w:val", v, sizeof(v))) {
                para_align = !strcmp(v, "center") ? 1 : (!strcmp(v, "right") || !strcmp(v, "end")) ? 2 :
                             (!strcmp(v, "both") || !strcmp(v, "distribute")) ? 3 : 0;
            } else if (x_is(&t, "w:numId") && x_attr(&t, "w:val", v, sizeof(v))) {
                para_list = dx_list_kind(x, atoi(v));
            } else if (x_is(&t, "w:spacing") && x_attr(&t, "w:line", v, sizeof(v))) {
                char rule[16];
                int l = atoi(v);
                if (!x_attr(&t, "w:lineRule", rule, sizeof(rule)) || !strcmp(rule, "auto")) {
                    l = l * 100 / 240;
                    if (l >= 80 && l <= 400) para_spacing = l;
                }
            } else if (x_is(&t, "w:pageBreakBefore") && x_onoff(&t)) {
                para_pb = 1;
            }
            if (cur && !hf) {
                cur->style = (unsigned char)para_style;
                cur->align = (unsigned char)para_align;
                cur->list = (unsigned char)para_list;
                cur->spacing = para_spacing;
                if (para_pb) cur->pbreak = 1;
                cur->eattr = (unsigned short)attr_id(&rn.base);
            }
            if (hf && x_is(&t, "w:jc") && hf_align) *hf_align = para_align < 3 ? para_align : 0;
            continue;
        }
        /* ---- লেখার অংশ ---- */
        if (x_is(&t, "w:r")) {
            if (t.t == X_OPEN) { rn.run = rn.base; rn.ascii[0] = rn.cs[0] = 0; }
            continue;
        }
        if (x_is(&t, "w:rPr")) { in_rpr = t.t == X_OPEN; continue; }
        if (in_rpr && open) {
            if (in_ppr) continue;                       /* অনুচ্ছেদ-চিহ্নের ফরম্যাট — বাদ */
            if (x_is(&t, "w:b")) { if (x_onoff(&t)) rn.run.flags |= A_BOLD; else rn.run.flags &= (unsigned char)~A_BOLD; }
            else if (x_is(&t, "w:i")) { if (x_onoff(&t)) rn.run.flags |= A_ITALIC; else rn.run.flags &= (unsigned char)~A_ITALIC; }
            else if (x_is(&t, "w:u")) { if (x_onoff(&t)) rn.run.flags |= A_UNDER; else rn.run.flags &= (unsigned char)~A_UNDER; }
            else if (x_is(&t, "w:strike") || x_is(&t, "w:dstrike")) { if (x_onoff(&t)) rn.run.flags |= A_STRIKE; }
            else if (x_is(&t, "w:color") && x_attr(&t, "w:val", v, sizeof(v)) && strcmp(v, "auto")) rn.run.color = (uint32_t)strtoul(v, NULL, 16) & 0xFFFFFF;
            else if (x_is(&t, "w:sz") && x_attr(&t, "w:val", v, sizeof(v))) {
                int sz = atoi(v);
                if (sz >= 2 && sz <= 800) rn.run.size2 = (unsigned short)sz;
            } else if (x_is(&t, "w:rFonts")) {
                if (!x_attr(&t, "w:ascii", rn.ascii, sizeof(rn.ascii))) x_attr(&t, "w:hAnsi", rn.ascii, sizeof(rn.ascii));
                x_attr(&t, "w:cs", rn.cs, sizeof(rn.cs));
            }
            continue;
        }
        if (x_is(&t, "w:t")) { in_t = t.t == X_OPEN; continue; }
        if (x_is(&t, "w:instrText")) { in_instr = t.t == X_OPEN; continue; }
        if (open && x_is(&t, "w:fldChar") && x_attr(&t, "w:fldCharType", v, sizeof(v))) {
            if (!strcmp(v, "begin")) fld_kind = 0;
            else if (!strcmp(v, "separate")) {
                if (hf && fld_kind) { sb_str(hf, fld_kind == 2 ? "{pages}" : "{page}"); fld_skip = 1; }
            } else if (!strcmp(v, "end")) { fld_skip = 0; fld_kind = 0; }
            continue;
        }
        if (x_is(&t, "w:fldSimple")) {
            if (open && hf && x_attr(&t, "w:instr", v, sizeof(v))) {
                sb_str(hf, strstr(v, "NUMPAGES") ? "{pages}" : strstr(v, "PAGE") ? "{page}" : "");
                if (t.t == X_OPEN && (strstr(v, "PAGE"))) fld_skip = 1;
            } else if (t.t == X_CLOSE) fld_skip = 0;
            continue;
        }
        if (open && x_is(&t, "w:tab")) {
            if (in_ppr) continue;                       /* tab stop-এর সংজ্ঞা */
            if (hf) sb_str(hf, "    ");
            else if (cur) para_insert_bytes(cur, cur->n, "    ", NULL, 4, dx_run_attr(&rn, 0));
            continue;
        }
        if (open && (x_is(&t, "w:br") || x_is(&t, "w:cr"))) {
            if (hf) { sb_str(hf, " "); continue; }
            if (x_attr(&t, "w:type", v, sizeof(v)) && !strcmp(v, "page")) {
                if (cur && cur->n == 0) cur->pbreak = 1;   /* লেখার আগে: এই অনুচ্ছেদই নতুন পাতায় */
                else if (cur) {                          /* লেখার মাঝে: বাকিটা নতুন অনুচ্ছেদে, নতুন পাতায় */
                    Para *q, *prev;
                    int idx = (int)(cur - x->A);
                    q = dx_new_para(x, attr_id(&rn.run));
                    prev = &x->A[idx];
                    q->style = prev->style; q->align = prev->align; q->spacing = prev->spacing;
                    q->tbl = prev->tbl; q->row = prev->row; q->col = prev->col;
                    q->pbreak = 1;
                    cur = q;
                } else {
                    pend_pbreak = 1;
                }
            } else if (cur && !tdepth) {                 /* লাইন ভাঙা: নতুন অনুচ্ছেদ (একই ফরম্যাট) */
                Para *q, *prev;
                int idx = (int)(cur - x->A);
                q = dx_new_para(x, attr_id(&rn.run));
                prev = &x->A[idx];
                q->style = prev->style; q->align = prev->align; q->spacing = prev->spacing; q->list = 0;
                cur = q;
            } else if (cur) {
                para_insert_bytes(cur, cur->n, " ", NULL, 1, dx_run_attr(&rn, 0));
            }
            continue;
        }
        /* ---- ছবি ---- */
        if (open && x_is(&t, "wp:extent")) {
            if (x_attr(&t, "cx", v, sizeof(v))) img_cx = atol(v);
            if (x_attr(&t, "cy", v, sizeof(v))) img_cy = atol(v);
            continue;
        }
        if (open && x_is(&t, "a:blip")) {
            x_attr(&t, "r:embed", img_id, sizeof(img_id));
            continue;
        }
        if (t.t == X_CLOSE && x_is(&t, "w:drawing")) {
            const char *tg = img_id[0] ? dx_rel(x, img_id) : NULL;
            if (tg && !hf) {
                char zp[200];
                long ilen = 0;
                char *data;
                dx_path(tg, zp, sizeof(zp));
                data = zip_get(&x->z, zp, &ilen);
                if (data) {
                    const char *dot = strrchr(zp, '.');
                    int id = img_add((unsigned char *)data, ilen, dot ? dot + 1 : "png");
                    if (id >= 0) {
                        int idx = cur ? (int)(cur - x->A) : -1;
                        Para *q = dx_new_para(x, attr_id(&rn.base));
                        q->kind = 1;
                        q->img = id;
                        q->img_w = img_cx > 0 ? (int)(img_cx / 9525) : (imgs[id].s ? imgs[id].s->w : 100);
                        q->img_h = img_cy > 0 ? (int)(img_cy / 9525) : (imgs[id].s ? imgs[id].s->h : 100);
                        if (idx >= 0) {
                            Para *prev = &x->A[idx];
                            q->align = prev->align;
                            q->tbl = prev->tbl; q->row = prev->row; q->col = prev->col;
                            if (prev->n == 0 && prev->kind == 0) {        /* ছবির আগে খালি অনুচ্ছেদ — ছবিটাই সেটা */
                                Para tmp = *q;
                                x->n--;
                                para_free(&x->A[idx]);
                                x->A[idx] = tmp;
                                q = &x->A[idx];
                            }
                        }
                        {                                                  /* বাকি লেখার জন্য নতুন অনুচ্ছেদ */
                            Para *after = dx_new_para(x, attr_id(&rn.base));
                            Para *img_p = &x->A[x->n - 2];
                            after->align = img_p->align;
                            after->tbl = img_p->tbl; after->row = img_p->row; after->col = img_p->col;
                            after->style = (unsigned char)para_style;
                            cur = after;
                        }
                        if (tdepth >= 1 && tid) cell_paras++;
                    }
                }
            } else if (!hf) {
                x->skipped++;
            }
            img_id[0] = 0;
            img_cx = img_cy = 0;
            continue;
        }
        if (open && (x_is(&t, "w:pict") || x_is(&t, "w:object") || x_is(&t, "mc:Fallback"))) {
            if (t.t == X_OPEN) skip = 1;               /* পুরোনো VML ছবি / mc:Fallback-এর দ্বিতীয় কপি */
            if (!x_is(&t, "mc:Fallback")) x->skipped++;
            continue;
        }
    }
    (void)maxcol;
}

/* ছবির পরে রেখে দেওয়া খালি অনুচ্ছেদ (শেষে বা অন্য অনুচ্ছেদের ঠিক আগে) বাদ */
static void dx_cleanup(Dx *x)
{
    int i;
    for (i = x->n - 1; i > 0; i--) {
        Para *p = &x->A[i];
        if (p->kind == 0 && p->n == 0 && x->A[i - 1].kind == 1 && p->tbl == x->A[i - 1].tbl &&
            (!p->tbl || (p->row == x->A[i - 1].row && p->col == x->A[i - 1].col)) && !p->pbreak) {
            int keep = i + 1 < x->n && x->A[i + 1].kind == 0 && x->A[i + 1].tbl == p->tbl &&
                       (!p->tbl || (x->A[i + 1].row == p->row && x->A[i + 1].col == p->col));
            if (keep || i == x->n - 1) {
                if (i == x->n - 1 && !p->tbl) continue;   /* শেষে লেখার জায়গা থাকুক */
                para_free(p);
                memmove(&x->A[i], &x->A[i + 1], sizeof(Para) * (size_t)(x->n - i - 1));
                x->n--;
            }
        }
    }
}

static void dx_header_footer(Dx *x, const char *docxml, long dlen)
{
    /* শেষ sectPr-এর default header/footer */
    const char *p = docxml, *last = NULL;
    while ((p = strstr(p, "<w:sectPr")) != NULL) { last = p; p++; }
    if (!last) return;
    {
        XRd r;
        XTok t;
        char ty[32], id[64];
        r.p = last;
        r.e = docxml + dlen;
        while (x_next(&r, &t) != X_EOF) {
            int footer;
            if (t.t == X_CLOSE && x_is(&t, "w:sectPr")) break;
            if (t.t != X_EMPTY && t.t != X_OPEN) continue;
            footer = x_is(&t, "w:footerReference");
            if (!footer && !x_is(&t, "w:headerReference")) continue;
            if (x_attr(&t, "w:type", ty, sizeof(ty)) && strcmp(ty, "default")) continue;
            if (x_attr(&t, "r:id", id, sizeof(id))) {
                const char *tg = dx_rel(x, id);
                char zp[200];
                long hl = 0;
                char *hx;
                SB hb = { 0, 0, 0 };
                int al = 0;                            /* Word-এ jc না থাকলে বাঁয়ে */
                if (!tg) continue;
                dx_path(tg, zp, sizeof(zp));
                hx = zip_get(&x->z, zp, &hl);
                if (!hx) continue;
                dx_body(x, hx, hl, &hb, &al);
                free(hx);
                if (hb.s) {
                    char *s = hb.s, *e;
                    while (*s == ' ') s++;
                    e = s + strlen(s);
                    while (e > s && e[-1] == ' ') *--e = 0;
                    if (footer) { snprintf(ftr_text, sizeof(ftr_text), "%s", s); ftr_align = al; }
                    else { snprintf(hdr_text, sizeof(hdr_text), "%s", s); hdr_align = al; }
                    free(hb.s);
                }
            }
        }
    }
}

/* .docx → অনুচ্ছেদ (header/footer গ্লোবালে); ফেরত সংখ্যা, ভুলে -1 */
static int load_docx(const unsigned char *buf, long len, Para **out, int *skipped)
{
    Dx x;
    long dlen = 0;
    char *doc;
    memset(&x, 0, sizeof(x));
    if (zip_open(&x.z, buf, len) < 0) return -1;
    doc = zip_get(&x.z, "word/document.xml", &dlen);
    if (!doc) { free(x.z.e); return -1; }
    dx_rels(&x, "word/_rels/document.xml.rels");
    dx_styles(&x);
    dx_numbering(&x);
    dx_body(&x, doc, dlen, NULL, NULL);
    dx_header_footer(&x, doc, dlen);
    free(doc);
    dx_cleanup(&x);
    if (x.n == 0) dx_new_para(&x, default_attr());
    *out = x.A;
    if (skipped) *skipped = x.skipped;
    free(x.z.e); free(x.rel); free(x.sty); free(x.num); free(x.abs);
    return x.n;
}

/* ---------- ZIP লেখা ---------- */

typedef struct { char name[64]; unsigned long crc; long csize, usize, off; } ZOut;
typedef struct { SB b; ZOut e[64]; int ne; } ZipW;

static void zw16(SB *b, unsigned v) { char c[2] = { (char)(v & 255), (char)(v >> 8) }; sb_add(b, c, 2); }
static void zw32(SB *b, unsigned long v) { char c[4] = { (char)(v & 255), (char)(v >> 8), (char)(v >> 16), (char)(v >> 24) }; sb_add(b, c, 4); }

static int zip_add(ZipW *z, const char *name, const void *data, long n)
{
    ZOut *e;
    unsigned char *cbuf;
    uLong bound = compressBound((uLong)n) + 64;
    z_stream s;
    long cn;
    if (z->ne >= 64) return -1;
    cbuf = (unsigned char *)malloc((size_t)bound);
    if (!cbuf) return -1;
    memset(&s, 0, sizeof(s));
    if (deflateInit2(&s, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) { free(cbuf); return -1; }
    s.next_in = (unsigned char *)data;
    s.avail_in = (uInt)n;
    s.next_out = cbuf;
    s.avail_out = (uInt)bound;
    deflate(&s, Z_FINISH);
    cn = (long)s.total_out;
    deflateEnd(&s);
    e = &z->e[z->ne++];
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->crc = crc32(0L, (const Bytef *)data, (uInt)n);
    e->csize = cn;
    e->usize = n;
    e->off = (long)z->b.n;
    zw32(&z->b, 0x04034b50UL); zw16(&z->b, 20); zw16(&z->b, 0); zw16(&z->b, 8);
    zw16(&z->b, 0); zw16(&z->b, 0x5B21);               /* সময়/তারিখ (২০২৫-০৯-০১) */
    zw32(&z->b, e->crc); zw32(&z->b, (unsigned long)cn); zw32(&z->b, (unsigned long)n);
    zw16(&z->b, (unsigned)strlen(name)); zw16(&z->b, 0);
    sb_str(&z->b, name);
    sb_add(&z->b, (const char *)cbuf, (size_t)cn);
    free(cbuf);
    return 0;
}

static void zip_finish(ZipW *z)
{
    long cd = (long)z->b.n;
    int i;
    for (i = 0; i < z->ne; i++) {
        ZOut *e = &z->e[i];
        zw32(&z->b, 0x02014b50UL); zw16(&z->b, 20); zw16(&z->b, 20); zw16(&z->b, 0); zw16(&z->b, 8);
        zw16(&z->b, 0); zw16(&z->b, 0x5B21);
        zw32(&z->b, e->crc); zw32(&z->b, (unsigned long)e->csize); zw32(&z->b, (unsigned long)e->usize);
        zw16(&z->b, (unsigned)strlen(e->name)); zw16(&z->b, 0); zw16(&z->b, 0); zw16(&z->b, 0); zw16(&z->b, 0);
        zw32(&z->b, 0); zw32(&z->b, (unsigned long)e->off);
        sb_str(&z->b, e->name);
    }
    {
        long cdsize = (long)z->b.n - cd;                 /* EOCD লেখার আগে মাপা */
        zw32(&z->b, 0x06054b50UL); zw16(&z->b, 0); zw16(&z->b, 0); zw16(&z->b, (unsigned)z->ne); zw16(&z->b, (unsigned)z->ne);
        zw32(&z->b, (unsigned long)cdsize); zw32(&z->b, (unsigned long)cd); zw16(&z->b, 0);
    }
}

/* ---------- অনুচ্ছেদ → .docx ---------- */

static void xml_esc(SB *b, const char *s, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (c == '<') sb_str(b, "&lt;");
        else if (c == '>') sb_str(b, "&gt;");
        else if (c == '&') sb_str(b, "&amp;");
        else if (c == '"') sb_str(b, "&quot;");
        else if ((unsigned char)c < 32 && c != '\t') continue;
        else sb_add(b, &c, 1);
    }
}

static int is_bangla_family(int f)
{
    const char *n = fams[f < NFAM ? f : 0].name;
    return strstr(n, "Bengali") || strstr(n, "Bangla") || strstr(n, "Hind");
}

static void dw_rpr(SB *b, int ai)
{
    const Attr *a = &attrs[ai];
    char tmp[200];
    const char *fn = fams[a->font < NFAM ? a->font : 0].name;
    sb_str(b, "<w:rPr>");
    snprintf(tmp, sizeof(tmp), "<w:rFonts w:ascii=\"%s\" w:hAnsi=\"%s\" w:cs=\"%s\"/>", fn, fn,
             is_bangla_family(a->font) ? fn : "Nirmala UI");
    sb_str(b, tmp);
    if (a->flags & A_BOLD) sb_str(b, "<w:b/><w:bCs/>");
    if (a->flags & A_ITALIC) sb_str(b, "<w:i/><w:iCs/>");
    if (a->flags & A_STRIKE) sb_str(b, "<w:strike/>");
    if (a->color & 0xFFFFFF) { snprintf(tmp, sizeof(tmp), "<w:color w:val=\"%06X\"/>", (unsigned)(a->color & 0xFFFFFF)); sb_str(b, tmp); }
    snprintf(tmp, sizeof(tmp), "<w:sz w:val=\"%d\"/><w:szCs w:val=\"%d\"/>", a->size2, a->size2);
    sb_str(b, tmp);
    if (a->flags & A_UNDER) sb_str(b, "<w:u w:val=\"single\"/>");
    sb_str(b, "</w:rPr>");
}

static int dw_numid;               /* নম্বর তালিকার চলতি numId (২ থেকে; ১ = বুলেট) */

static void dw_para(SB *b, int i, int *img_no)
{
    Para *q = &P[i];
    char tmp[300];
    static const char *const sty[5] = { NULL, "Title", "Heading1", "Heading2", "Heading3" };
    static const char *const jc[4] = { "left", "center", "right", "both" };
    sb_str(b, "<w:p><w:pPr>");
    if (q->style > 0 && q->style < 5) { snprintf(tmp, sizeof(tmp), "<w:pStyle w:val=\"%s\"/>", sty[q->style]); sb_str(b, tmp); }
    if (q->pbreak) sb_str(b, "<w:pageBreakBefore/>");
    if (q->list == 1) sb_str(b, "<w:numPr><w:ilvl w:val=\"0\"/><w:numId w:val=\"1\"/></w:numPr>");
    else if (q->list == 2) {
        if (i == 0 || P[i - 1].list != 2) dw_numid++;   /* নতুন নম্বর তালিকা আবার ১ থেকে */
        snprintf(tmp, sizeof(tmp), "<w:numPr><w:ilvl w:val=\"0\"/><w:numId w:val=\"%d\"/></w:numPr>", dw_numid);
        sb_str(b, tmp);
    }
    snprintf(tmp, sizeof(tmp), "<w:spacing w:after=\"%d\" w:line=\"%d\" w:lineRule=\"auto\"/>", q->tbl ? 0 : 120, q->spacing * 240 / 100);
    sb_str(b, tmp);
    if (q->align) { snprintf(tmp, sizeof(tmp), "<w:jc w:val=\"%s\"/>", jc[q->align & 3]); sb_str(b, tmp); }
    sb_str(b, "</w:pPr>");
    if (q->kind == 1 && q->img >= 0 && q->img < nimgs) {
        long cx = (long)q->img_w * 9525, cy = (long)q->img_h * 9525;
        int n = ++*img_no;
        snprintf(tmp, sizeof(tmp), "<w:r><w:drawing><wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\">"
                 "<wp:extent cx=\"%ld\" cy=\"%ld\"/><wp:docPr id=\"%d\" name=\"Picture %d\"/>", cx, cy, n, n);
        sb_str(b, tmp);
        snprintf(tmp, sizeof(tmp), "<a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">"
                 "<pic:pic><pic:nvPicPr><pic:cNvPr id=\"%d\" name=\"image%d\"/><pic:cNvPicPr/></pic:nvPicPr>", n, n);
        sb_str(b, tmp);
        snprintf(tmp, sizeof(tmp), "<pic:blipFill><a:blip r:embed=\"rIdImg%d\"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>"
                 "<pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"%ld\" cy=\"%ld\"/></a:xfrm>", q->img, cx, cy);
        sb_str(b, tmp);
        sb_str(b, "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr></pic:pic></a:graphicData></a:graphic>"
                  "</wp:inline></w:drawing></w:r>");
    } else {
        int a = 0;
        while (a < q->n) {
            int j = a;
            while (j < q->n && q->a[j] == q->a[a]) j++;
            sb_str(b, "<w:r>");
            dw_rpr(b, q->a[a]);
            sb_str(b, "<w:t xml:space=\"preserve\">");
            xml_esc(b, q->t + a, j - a);
            sb_str(b, "</w:t></w:r>");
            a = j;
        }
    }
    sb_str(b, "</w:p>");
}

#define DOCX_NS "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" " \
                "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" " \
                "xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\" " \
                "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" " \
                "xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\""

static void dw_hf(SB *b, const char *text, int align, int footer)
{
    static const char *const jc[3] = { "left", "center", "right" };
    const char *s = text;
    char tmp[200];
    sb_str(b, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n");
    sb_str(b, footer ? "<w:ftr " DOCX_NS ">" : "<w:hdr " DOCX_NS ">");
    snprintf(tmp, sizeof(tmp), "<w:p><w:pPr><w:jc w:val=\"%s\"/></w:pPr>", jc[align >= 0 && align < 3 ? align : 1]);
    sb_str(b, tmp);
    while (*s) {
        const char *f1 = strstr(s, "{page}"), *f2 = strstr(s, "{pages}"), *f = NULL;
        int pages = 0;
        if (f1 && (!f2 || f1 < f2)) f = f1;
        else if (f2) { f = f2; pages = 1; }
        if (f > s || !f) {
            sb_str(b, "<w:r><w:rPr><w:sz w:val=\"20\"/><w:color w:val=\"404040\"/></w:rPr><w:t xml:space=\"preserve\">");
            xml_esc(b, s, f ? (int)(f - s) : (int)strlen(s));
            sb_str(b, "</w:t></w:r>");
        }
        if (!f) break;
        sb_str(b, pages ? "<w:fldSimple w:instr=\" NUMPAGES \">" : "<w:fldSimple w:instr=\" PAGE \">");
        sb_str(b, "<w:r><w:rPr><w:sz w:val=\"20\"/><w:color w:val=\"404040\"/></w:rPr><w:t>1</w:t></w:r></w:fldSimple>");
        s = f + (pages ? 7 : 6);
    }
    sb_str(b, "</w:p>");
    sb_str(b, footer ? "</w:ftr>" : "</w:hdr>");
}

static const char *img_mime(const char *ext)
{
    if (!strcasecmp(ext, "jpg") || !strcasecmp(ext, "jpeg")) return "image/jpeg";
    if (!strcasecmp(ext, "gif")) return "image/gif";
    if (!strcasecmp(ext, "bmp")) return "image/bmp";
    if (!strcasecmp(ext, "tga")) return "image/x-tga";
    return "image/png";
}

static int save_docx(const char *p)
{
    SB d = { 0, 0, 0 }, s = { 0, 0, 0 }, ct = { 0, 0, 0 }, rel = { 0, 0, 0 }, num = { 0, 0, 0 }, h = { 0, 0, 0 }, f = { 0, 0, 0 };
    ZipW z;
    char tmp[400], name[64];
    int i, img_no = 0, k;
    unsigned char *used = (unsigned char *)calloc((size_t)nimgs + 1, 1);
    FILE *fp;
    memset(&z, 0, sizeof(z));
    dw_numid = 1;
    /* document.xml */
    sb_str(&d, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:document " DOCX_NS "><w:body>");
    for (i = 0; i < np;) {
        if (P[i].tbl) {
            int a, b, r, c, rows, cols = tbls[P[i].tbl].cols, tot = 0;
            table_span(i, &a, &b);
            rows = table_rows(a, b);
            for (c = 0; c < cols; c++) tot += tbls[P[i].tbl].w[c] > 0 ? tbls[P[i].tbl].w[c] : 1;
            sb_str(&d, "<w:tbl><w:tblPr><w:tblW w:w=\"5000\" w:type=\"pct\"/><w:tblBorders>"
                       "<w:top w:val=\"single\" w:sz=\"4\" w:color=\"595959\"/><w:left w:val=\"single\" w:sz=\"4\" w:color=\"595959\"/>"
                       "<w:bottom w:val=\"single\" w:sz=\"4\" w:color=\"595959\"/><w:right w:val=\"single\" w:sz=\"4\" w:color=\"595959\"/>"
                       "<w:insideH w:val=\"single\" w:sz=\"4\" w:color=\"595959\"/><w:insideV w:val=\"single\" w:sz=\"4\" w:color=\"595959\"/>"
                       "</w:tblBorders><w:tblLayout w:type=\"fixed\"/></w:tblPr><w:tblGrid>");
            for (c = 0; c < cols; c++) {
                snprintf(tmp, sizeof(tmp), "<w:gridCol w:w=\"%d\"/>", (int)(9026L * (tbls[P[i].tbl].w[c] > 0 ? tbls[P[i].tbl].w[c] : 1) / (tot > 0 ? tot : 1)));
                sb_str(&d, tmp);
            }
            sb_str(&d, "</w:tblGrid>");
            for (r = 0; r < rows; r++) {
                sb_str(&d, "<w:tr>");
                for (c = 0; c < cols; c++) {
                    int any = 0;
                    snprintf(tmp, sizeof(tmp), "<w:tc><w:tcPr><w:tcW w:w=\"%d\" w:type=\"dxa\"/></w:tcPr>",
                             (int)(9026L * (tbls[P[i].tbl].w[c] > 0 ? tbls[P[i].tbl].w[c] : 1) / (tot > 0 ? tot : 1)));
                    sb_str(&d, tmp);
                    for (k = a; k < b; k++) {
                        if (P[k].row != r || P[k].col != c) continue;
                        if (P[k].kind == 1 && P[k].img >= 0 && P[k].img < nimgs) used[P[k].img] = 1;
                        dw_para(&d, k, &img_no);
                        any = 1;
                    }
                    if (!any) sb_str(&d, "<w:p/>");
                    sb_str(&d, "</w:tc>");
                }
                sb_str(&d, "</w:tr>");
            }
            sb_str(&d, "</w:tbl>");
            i = b;
            continue;
        }
        if (P[i].kind == 1 && P[i].img >= 0 && P[i].img < nimgs) used[P[i].img] = 1;
        dw_para(&d, i, &img_no);
        i++;
    }
    sb_str(&d, "<w:sectPr>");
    if (hdr_text[0]) sb_str(&d, "<w:headerReference w:type=\"default\" r:id=\"rIdHdr\"/>");
    if (ftr_text[0]) sb_str(&d, "<w:footerReference w:type=\"default\" r:id=\"rIdFtr\"/>");
    sb_str(&d, "<w:pgSz w:w=\"11906\" w:h=\"16838\"/><w:pgMar w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\" "
               "w:left=\"1440\" w:header=\"720\" w:footer=\"720\" w:gutter=\"0\"/></w:sectPr></w:body></w:document>");
    /* styles.xml */
    snprintf(name, sizeof(name), "%s", fams[default_family()].name);
    sb_str(&s, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:styles " DOCX_NS ">");
    snprintf(tmp, sizeof(tmp), "<w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:ascii=\"%s\" w:hAnsi=\"%s\" w:cs=\"Nirmala UI\"/>"
             "<w:sz w:val=\"22\"/><w:szCs w:val=\"22\"/></w:rPr></w:rPrDefault>", name, name);
    sb_str(&s, tmp);
    sb_str(&s, "<w:pPrDefault><w:pPr><w:spacing w:after=\"120\" w:line=\"276\" w:lineRule=\"auto\"/></w:pPr></w:pPrDefault></w:docDefaults>"
               "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/><w:qFormat/></w:style>"
               "<w:style w:type=\"paragraph\" w:styleId=\"Title\"><w:name w:val=\"Title\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/><w:qFormat/>"
               "<w:rPr><w:sz w:val=\"52\"/><w:szCs w:val=\"52\"/></w:rPr></w:style>"
               "<w:style w:type=\"paragraph\" w:styleId=\"Heading1\"><w:name w:val=\"heading 1\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/><w:qFormat/>"
               "<w:pPr><w:keepNext/><w:spacing w:before=\"240\"/><w:outlineLvl w:val=\"0\"/></w:pPr><w:rPr><w:b/><w:color w:val=\"2F5496\"/><w:sz w:val=\"40\"/></w:rPr></w:style>"
               "<w:style w:type=\"paragraph\" w:styleId=\"Heading2\"><w:name w:val=\"heading 2\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/><w:qFormat/>"
               "<w:pPr><w:keepNext/><w:spacing w:before=\"200\"/><w:outlineLvl w:val=\"1\"/></w:pPr><w:rPr><w:b/><w:color w:val=\"2F5496\"/><w:sz w:val=\"32\"/></w:rPr></w:style>"
               "<w:style w:type=\"paragraph\" w:styleId=\"Heading3\"><w:name w:val=\"heading 3\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/><w:qFormat/>"
               "<w:pPr><w:keepNext/><w:spacing w:before=\"160\"/><w:outlineLvl w:val=\"2\"/></w:pPr><w:rPr><w:b/><w:color w:val=\"2F5496\"/><w:sz w:val=\"26\"/></w:rPr></w:style>"
               "</w:styles>");
    /* numbering.xml */
    sb_str(&num, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<w:numbering " DOCX_NS ">"
                 "<w:abstractNum w:abstractNumId=\"0\"><w:lvl w:ilvl=\"0\"><w:start w:val=\"1\"/><w:numFmt w:val=\"bullet\"/>"
                 "<w:lvlText w:val=\"\xE2\x80\xA2\"/><w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"720\" w:hanging=\"360\"/></w:pPr></w:lvl></w:abstractNum>"
                 "<w:abstractNum w:abstractNumId=\"1\"><w:lvl w:ilvl=\"0\"><w:start w:val=\"1\"/><w:numFmt w:val=\"decimal\"/>"
                 "<w:lvlText w:val=\"%1.\"/><w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"720\" w:hanging=\"360\"/></w:pPr></w:lvl></w:abstractNum>"
                 "<w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num>");
    for (k = 2; k <= dw_numid; k++) {
        snprintf(tmp, sizeof(tmp), "<w:num w:numId=\"%d\"><w:abstractNumId w:val=\"1\"/><w:lvlOverride w:ilvl=\"0\"><w:startOverride w:val=\"1\"/></w:lvlOverride></w:num>", k);
        sb_str(&num, tmp);
    }
    sb_str(&num, "</w:numbering>");
    /* সম্পর্ক আর ধরন */
    sb_str(&rel, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                 "<Relationship Id=\"rIdStyles\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
                 "<Relationship Id=\"rIdNum\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering\" Target=\"numbering.xml\"/>");
    if (hdr_text[0]) sb_str(&rel, "<Relationship Id=\"rIdHdr\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/header\" Target=\"header1.xml\"/>");
    if (ftr_text[0]) sb_str(&rel, "<Relationship Id=\"rIdFtr\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/footer\" Target=\"footer1.xml\"/>");
    sb_str(&ct, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
                "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
                "<Default Extension=\"xml\" ContentType=\"application/xml\"/>");
    {
        static const char *const exts[] = { "png", "jpg", "jpeg", "gif", "bmp", "tga" };
        int e;
        for (e = 0; e < 6; e++) {
            snprintf(tmp, sizeof(tmp), "<Default Extension=\"%s\" ContentType=\"%s\"/>", exts[e], img_mime(exts[e]));
            sb_str(&ct, tmp);
        }
    }
    sb_str(&ct, "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
                "<Override PartName=\"/word/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>"
                "<Override PartName=\"/word/numbering.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml\"/>");
    if (hdr_text[0]) sb_str(&ct, "<Override PartName=\"/word/header1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.header+xml\"/>");
    if (ftr_text[0]) sb_str(&ct, "<Override PartName=\"/word/footer1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.footer+xml\"/>");
    sb_str(&ct, "</Types>");
    {
        static const char rels[] = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
            "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
            "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>"
            "</Relationships>";
        zip_add(&z, "[Content_Types].xml", ct.s, (long)ct.n);
        zip_add(&z, "_rels/.rels", rels, (long)strlen(rels));
    }
    for (k = 0; k < nimgs; k++) {
        if (!used || !used[k]) continue;
        snprintf(tmp, sizeof(tmp), "<Relationship Id=\"rIdImg%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"media/image%d.%s\"/>",
                 k, k, imgs[k].ext);
        sb_str(&rel, tmp);
        snprintf(name, sizeof(name), "word/media/image%d.%s", k, imgs[k].ext);
        zip_add(&z, name, imgs[k].data, imgs[k].len);
    }
    sb_str(&rel, "</Relationships>");
    zip_add(&z, "word/document.xml", d.s, (long)d.n);
    zip_add(&z, "word/styles.xml", s.s, (long)s.n);
    zip_add(&z, "word/numbering.xml", num.s, (long)num.n);
    zip_add(&z, "word/_rels/document.xml.rels", rel.s, (long)rel.n);
    if (hdr_text[0]) { dw_hf(&h, hdr_text, hdr_align, 0); zip_add(&z, "word/header1.xml", h.s, (long)h.n); }
    if (ftr_text[0]) { dw_hf(&f, ftr_text, ftr_align, 1); zip_add(&z, "word/footer1.xml", f.s, (long)f.n); }
    zip_finish(&z);
    free(d.s); free(s.s); free(ct.s); free(rel.s); free(num.s); free(h.s); free(f.s); free(used);
    fp = fopen(p, "wb");
    if (!fp || fwrite(z.b.s, 1, z.b.n, fp) != z.b.n) {
        if (fp) fclose(fp);
        free(z.b.s);
        return -1;
    }
    fclose(fp);
    free(z.b.s);
    return 0;
}

static int is_docx, docx_skipped;  /* ধাপ ২৬খ-৩: খোলা ফাইলটা .docx */

static int load_doc(const char *p)
{
    size_t len = 0;
    char *b = read_file(p, &len);
    Para *A = NULL;
    int n;
    if (!b) return -1;
    hdr_text[0] = ftr_text[0] = 0;                   /* ধাপ ২৬খ-২: ফাইলে থাকলে parse বসাবে */
    hdr_align = ftr_align = 1;
    is_docx = 0;
    if (!strncmp(b, "rozword ", 8)) n = parse_rwd(b, &A, 1);
    else if (len > 4 && b[0] == 'P' && b[1] == 'K' && b[2] == 3 && b[3] == 4) {   /* ধাপ ২৬খ-৩: .docx */
        int skipped = 0;
        n = load_docx((const unsigned char *)b, (long)len, &A, &skipped);
        is_docx = 1;
        docx_skipped = skipped;
    } else n = parse_plain(b, len, default_attr(), NULL, &A);
    free(b);
    if (n <= 0) return -1;
    doc_replace(A, n);
    if (P[np - 1].tbl || P[np - 1].kind == 1) {       /* শেষে লেখার জায়গা */
        paras_insert_at(np, 1);
        para_init(&P[np - 1], default_attr());
    }
    cp = ci = sp = si = 0;
    scroll_y = 0;
    snprintf(path, sizeof(path), "%s", p);
    untitled = 0;
    modified = 0;
    reset_undo();
    if (is_docx) snprintf(status_msg, sizeof(status_msg), "Opened Word document %s%s", base_name(p),
                          docx_skipped ? " - some objects rozword cannot show were left out" : "");
    else snprintf(status_msg, sizeof(status_msg), "Opened %s", base_name(p));
    return 0;
}

static int save_doc(const char *p)
{
    SB b = { 0, 0, 0 };
    FILE *f;
    int as_text = has_ext(p, ".txt");
    if (has_ext(p, ".docx")) {                         /* ধাপ ২৬খ-৩ */
        if (save_docx(p) < 0) {
            char msg[700];
            snprintf(msg, sizeof(msg), "Cannot save %s", p);
            report_error(msg);
            return -1;
        }
        snprintf(path, sizeof(path), "%s", p);
        untitled = 0;
        modified = 0;
        is_docx = 1;
        snprintf(status_msg, sizeof(status_msg), "Saved as a Word document: %s", base_name(p));
        return 0;
    }
    if (as_text) { plain_range(&b, 0, 0, np - 1, P[np - 1].n, 1); sb_str(&b, "\n"); }
    else serialize_range(&b, 0, 0, np - 1, P[np - 1].n, SER_META | SER_EMBED);
    f = fopen(p, "wb");
    if (!f || fwrite(b.s, 1, b.n, f) != b.n) {
        char msg[700];
        if (f) fclose(f);
        free(b.s);
        snprintf(msg, sizeof(msg), "Cannot save %s", p);
        report_error(msg);
        return -1;
    }
    fclose(f);
    free(b.s);
    if (!as_text) { snprintf(path, sizeof(path), "%s", p); untitled = 0; modified = 0; is_docx = 0; }
    snprintf(status_msg, sizeof(status_msg), as_text ? "Exported as text: %s" : "Saved %s", base_name(p));
    return 0;
}

/* as_text: 0 = .rwd (বা নাম .docx/.txt দিলে সেটা), 1 = লেখা হিসেবে, 2 = Word (.docx) — ধাপ ২৬খ-৩ */
static int save_as(int as_text)
{
    char p[512];
    snprintf(p, sizeof(p), "%s", path);
    if (as_text) {
        char *dot = strrchr(p, '.');
        if (dot && dot > strrchr(p, '/')) *dot = '\0';
        strncat(p, as_text == 2 ? ".docx" : ".txt", sizeof(p) - strlen(p) - 1);
    }
    if (!ui_filedlg(as_text == 1 ? "Export as text" : as_text == 2 ? "Save as Word document" : "Save document", p, sizeof(p), 1))
        return -1;
    if (as_text == 2 && !has_ext(p, ".docx")) strncat(p, ".docx", sizeof(p) - strlen(p) - 1);
    if (!as_text && !has_ext(p, ".rwd") && !has_ext(p, ".txt") && !has_ext(p, ".docx")) strncat(p, ".rwd", sizeof(p) - strlen(p) - 1);
    return save_doc(p);
}

static int do_save(void)
{
    if (!untitled && has_ext(path, ".docx")) return save_doc(path);   /* ধাপ ২৬খ-৩: Word ফাইল Word হিসেবেই */
    if (untitled || !has_ext(path, ".rwd")) return save_as(0);
    return save_doc(path);
}

/* সেভ না করা বদল — 1 = এগোনো যায় */
static int confirm_discard(void)
{
    int r;
    if (!modified) return 1;
    r = ui_msgbox("rozword", "Save the changes to this document?", UI_MB_YESNOCANCEL | UI_MB_QUESTION);
    if (r == UI_YES) return do_save() == 0;
    return r == UI_NO;
}

static void new_doc(void)
{
    char cwd[256];
    doc_clear();
    reset_undo();
    if (!getcwd(cwd, sizeof(cwd)) || !strcmp(cwd, "/")) strcpy(cwd, "/home");
    snprintf(path, sizeof(path), "%s/untitled.rwd", cwd);
    untitled = 1;
    modified = 0;
}

/* ================= ক্লিপবোর্ড ================= */

static char *rich_doc, *rich_plain;          /* rozword-এর ভেতরের কপি (ফরম্যাট সহ) */

static void do_copy(int cut)
{
    int p1, i1, p2, i2;
    SB r = { 0, 0, 0 }, t = { 0, 0, 0 };
    if (!sel_range(&p1, &i1, &p2, &i2)) return;
    serialize_range(&r, p1, i1, p2, i2, 0);
    plain_range(&t, p1, i1, p2, i2, 0);
    free(rich_doc);
    free(rich_plain);
    rich_doc = r.s;
    rich_plain = t.s ? t.s : strdup("");
    ui_clip_set(rich_plain, (int)strlen(rich_plain));
    if (cut) {
        undo_point(2);
        delete_selection();
    }
}

static void do_paste(void)
{
    int len = 0, n;
    char *clip = ui_clip_get(&len);
    Para *F = NULL;
    if (!clip && !rich_doc) return;
    undo_point(2);
    delete_selection();
    if (rich_doc && (!clip || (rich_plain && (int)strlen(rich_plain) == len && !memcmp(clip, rich_plain, (size_t)len))))
        n = parse_rwd(rich_doc, &F, 0);
    else
        n = parse_plain(clip, (size_t)len, cur_attr(), &P[cp], &F);
    free(clip);
    if (n > 0) insert_paras(F, n);
    follow_cursor = 1;
}

/* ================= ধাপ ২৬খ-২: টেবিল, ছবি, header/footer ================= */

static void table_span(int i, int *a, int *b)      /* P[i]-এর টেবিলের অনুচ্ছেদ [a, b) */
{
    int t = P[i].tbl;
    *a = i;
    *b = i + 1;
    while (*a > 0 && P[*a - 1].tbl == t) (*a)--;
    while (*b < np && P[*b].tbl == t) (*b)++;
}

static int cell_first(int a, int b, int row, int col)
{
    int k;
    for (k = a; k < b; k++) if (P[k].row == row && P[k].col == col) return k;
    return -1;
}

static int cell_last(int a, int b, int row, int col)
{
    int k, r = -1;
    for (k = a; k < b; k++) if (P[k].row == row && P[k].col == col) r = k;
    return r;
}

static int table_rows(int a, int b)
{
    int k, r = 0;
    for (k = a; k < b; k++) if (P[k].row + 1 > r) r = P[k].row + 1;
    return r;
}

static void new_cell_para(int at, int tid, int row, int col)
{
    para_init(&P[at], default_attr());
    P[at].tbl = tid;
    P[at].row = row;
    P[at].col = col;
}

/* শেষে টেবিল/ছবি থাকলে তার পরে লেখার জন্য খালি অনুচ্ছেদ */
static void ensure_text_after(int i)
{
    if (i + 1 >= np || P[i + 1].tbl) {
        if (i + 1 < np && !P[i].tbl) return;
        paras_insert_at(i + 1, 1);
        para_init(&P[i + 1], default_attr());
    }
}

/* নতুন অনুচ্ছেদের জায়গা (টেবিল/ছবি): কার্সর যেখানে — টেবিলের ভেতরে থাকলে টেবিলের পরে */
static int block_insert_pos(int allow_in_cell)
{
    int a, b;
    if (P[cp].tbl && !allow_in_cell) { table_span(cp, &a, &b); return b; }
    if (P[cp].kind == 1) return cp + 1;
    if (ci == 0) return cp;
    if (ci >= P[cp].n) return cp + 1;
    split_para();
    return cp;
}

static void insert_table(void)
{
    char buf[32] = "3 x 3";
    int cols = 3, rows = 3, at, tid, r, c;
    char *x;
    if (!ui_inputbox("Insert table", "Columns x rows (for example 3 x 2):", buf, sizeof(buf))) return;
    cols = atoi(buf);
    x = strchr(buf, 'x');
    if (!x) x = strchr(buf, 'X');
    if (!x) x = strchr(buf, '*');
    rows = x ? atoi(x + 1) : 2;
    if (cols < 1 || cols > TBL_MAXCOL || rows < 1 || rows > 200) {
        ui_msgbox("rozword", "Columns: 1 to 16, rows: 1 to 200", UI_MB_OK | UI_MB_WARN);
        return;
    }
    undo_point(2);
    tid = tbl_new(cols);
    if (!tid) return;
    at = block_insert_pos(0);
    paras_insert_at(at, rows * cols);
    for (r = 0; r < rows; r++)
        for (c = 0; c < cols; c++) new_cell_para(at + r * cols + c, tid, r, c);
    ensure_text_after(at + rows * cols - 1);
    cp = sp = at;
    ci = si = 0;
    typing_attr = -1;
    layout_all = 1;
    snprintf(status_msg, sizeof(status_msg), "Table %d x %d - Tab goes to the next cell; Table menu adds rows/columns", cols, rows);
}

static void table_add_row(void)
{
    int a, b, r, k, at, c, cols;
    if (!P[cp].tbl) return;
    undo_point(2);
    table_span(cp, &a, &b);
    r = P[cp].row;
    cols = tbls[P[cp].tbl].cols;
    for (k = a; k < b; k++) if (P[k].row > r) P[k].row++;
    at = a;
    for (k = a; k < b; k++) if (P[k].row == r) at = k + 1;
    paras_insert_at(at, cols);
    for (c = 0; c < cols; c++) new_cell_para(at + c, P[a].tbl, r + 1, c);
    cp = sp = at;
    ci = si = 0;
    layout_all = 1;
}

static void table_add_col(void)
{
    int a, b, c, r, rows, tid, k;
    RTable *T;
    if (!P[cp].tbl) return;
    tid = P[cp].tbl;
    T = &tbls[tid];
    if (T->cols >= TBL_MAXCOL) { snprintf(status_msg, sizeof(status_msg), "At most %d columns", TBL_MAXCOL); return; }
    undo_point(2);
    c = P[cp].col;
    table_span(cp, &a, &b);
    rows = table_rows(a, b);
    for (r = rows - 1; r >= 0; r--) {
        int last;
        table_span(a, &a, &b);
        for (k = a; k < b; k++) if (P[k].row == r && P[k].col > c) P[k].col++;
        last = cell_last(a, b, r, c);
        if (last < 0) {                              /* ঘর নেই: সারির শেষে */
            last = a - 1;
            for (k = a; k < b; k++) if (P[k].row <= r) last = k;
        }
        paras_insert_at(last + 1, 1);
        new_cell_para(last + 1, tid, r, c + 1);
    }
    for (k = TBL_MAXCOL - 1; k > c + 1; k--) T->w[k] = T->w[k - 1];
    T->cols++;
    for (k = 0; k < T->cols; k++) T->w[k] = 1000 / T->cols;
    layout_all = 1;
}

static void table_delete(void)
{
    int a, b;
    if (!P[cp].tbl) return;
    undo_point(2);
    table_span(cp, &a, &b);
    paras_remove(a, b);
    if (np == 0) { paras_insert_at(0, 1); para_init(&P[0], default_attr()); }
    cp = sp = a < np ? a : np - 1;
    ci = si = 0;
    layout_all = 1;
}

static void table_del_row(void)
{
    int a, b, r, k;
    if (!P[cp].tbl) return;
    table_span(cp, &a, &b);
    if (table_rows(a, b) <= 1) { table_delete(); return; }
    undo_point(2);
    r = P[cp].row;
    for (k = b - 1; k >= a; k--) if (P[k].row == r) paras_remove(k, k + 1);
    table_span(a, &a, &b);
    for (k = a; k < b; k++) if (P[k].row > r) P[k].row--;
    cp = sp = a;
    for (k = a; k < b; k++) if (P[k].row == (r > 0 ? r - 1 : 0)) { cp = sp = k; break; }
    ci = si = 0;
    layout_all = 1;
}

static void table_del_col(void)
{
    int a, b, c, k, tid;
    RTable *T;
    if (!P[cp].tbl) return;
    tid = P[cp].tbl;
    T = &tbls[tid];
    if (T->cols <= 1) { table_delete(); return; }
    undo_point(2);
    c = P[cp].col;
    table_span(cp, &a, &b);
    for (k = b - 1; k >= a; k--) if (P[k].col == c) paras_remove(k, k + 1);
    table_span(a, &a, &b);
    for (k = a; k < b; k++) if (P[k].col > c) P[k].col--;
    for (k = c; k < TBL_MAXCOL - 1; k++) T->w[k] = T->w[k + 1];
    T->cols--;
    cp = sp = a;
    ci = si = 0;
    layout_all = 1;
}

static void table_col_width(int delta)
{
    RTable *T;
    int c;
    if (!P[cp].tbl) return;
    T = &tbls[P[cp].tbl];
    c = P[cp].col;
    if (c >= T->cols) return;
    undo_point(2);
    T->w[c] += delta;
    if (T->w[c] < 50) T->w[c] = 50;
    if (T->w[c] > 4000) T->w[c] = 4000;
    layout_all = 1;
}

/* Tab / Shift+Tab: পরের/আগের ঘর; শেষ ঘরে Tab = নতুন সারি */
static void table_tab(int dir)
{
    int a, b, r = P[cp].row, c = P[cp].col, cols = tbls[P[cp].tbl].cols, rows, k;
    table_span(cp, &a, &b);
    rows = table_rows(a, b);
    for (;;) {
        c += dir;
        if (c >= cols) { c = 0; r++; }
        if (c < 0) { c = cols - 1; r--; }
        if (r < 0) return;
        if (r >= rows) { table_add_row(); return; }
        k = cell_first(a, b, r, c);
        if (k >= 0) { cp = sp = k; ci = si = 0; follow_cursor = 1; return; }
    }
}

static void insert_picture(void)
{
    char p[512];
    size_t len = 0;
    char *data;
    const char *dot;
    int id, at, w, h, cw100 = PAGE_W - 2 * MARGIN;
    snprintf(p, sizeof(p), "%s", path);
    {
        char *sl = strrchr(p, '/');
        if (sl) sl[1] = 0;
    }
    if (!ui_filedlg("Insert picture", p, sizeof(p), 0)) return;
    data = read_file(p, &len);
    if (!data) { ui_msgbox("rozword", "Cannot read the picture", UI_MB_OK | UI_MB_ERROR); return; }
    dot = strrchr(p, '.');
    id = img_add((unsigned char *)data, (long)len, dot ? dot + 1 : "png");
    if (id >= 0 && !imgs[id].s) {                     /* stb চেনে না (যেমন .ppm): PNG বানিয়ে রাখা */
        gfx_surface *sf = gfx_load_image(p);
        if (sf && gfx_save_png(sf, "/tmp/.rozword-img.png") == 0) {
            size_t l2 = 0;
            char *d2 = read_file("/tmp/.rozword-img.png", &l2);
            if (d2) {
                free(imgs[id].data);
                imgs[id].data = (unsigned char *)d2;
                imgs[id].len = (long)l2;
                snprintf(imgs[id].ext, sizeof(imgs[id].ext), "png");
                imgs[id].s = sf;
                sf = NULL;
            }
            unlink("/tmp/.rozword-img.png");
        }
        if (sf) gfx_surface_free(sf);
    }
    if (id < 0 || !imgs[id].s) { ui_msgbox("rozword", "This picture format is not supported", UI_MB_OK | UI_MB_ERROR); return; }
    undo_point(2);
    w = imgs[id].s->w;
    h = imgs[id].s->h;
    if (w > cw100) { h = (int)((long)h * cw100 / w); w = cw100; }
    at = block_insert_pos(1);
    {
        int tb = P[cp].tbl, rw = P[cp].row, cl = P[cp].col;   /* ঘরের ভেতরে হলে একই ঘরে */
        paras_insert_at(at, 1);
        para_init(&P[at], default_attr());
        P[at].kind = 1;
        P[at].img = id;
        P[at].img_w = w;
        P[at].img_h = h;
        P[at].align = 1;
        P[at].tbl = tb;
        P[at].row = rw;
        P[at].col = cl;
    }
    if (!P[at].tbl) ensure_text_after(at);
    cp = sp = at;
    ci = si = 0;
    layout_all = 1;
    snprintf(status_msg, sizeof(status_msg), "Picture %dx%d - Format > Picture size changes it", imgs[id].s->w, imgs[id].s->h);
}

static void picture_size(void)
{
    char buf[16];
    int pct, cw100 = PAGE_W - 2 * MARGIN;
    Para *q = &P[cp];
    if (q->kind != 1) { ui_msgbox("rozword", "Click on a picture first", UI_MB_OK | UI_MB_INFO); return; }
    snprintf(buf, sizeof(buf), "%d", q->img_w * 100 / cw100);
    if (!ui_inputbox("Picture size", "Width in % of the text width (5 to 100):", buf, sizeof(buf))) return;
    pct = atoi(buf);
    if (pct < 5) pct = 5;
    if (pct > 100) pct = 100;
    undo_point(2);
    {
        int ow = q->img_w > 0 ? q->img_w : 1, oh = q->img_h;
        q->img_w = cw100 * pct / 100;
        q->img_h = (int)((long)oh * q->img_w / ow);
        if (q->img_h < 1) q->img_h = 1;
    }
    q->dirty = 1;
    layout_all = 1;
}

static void edit_header_footer(int footer)
{
    static const struct ui_menuitem al[] = { { "Left", NULL, 1 }, { "Center", NULL, 2 }, { "Right", NULL, 3 } };
    char buf[256];
    int r;
    snprintf(buf, sizeof(buf), "%s", footer ? ftr_text : hdr_text);
    if (!ui_inputbox(footer ? "Footer" : "Header", footer ? "Footer text ({page} = page number, {pages} = number of pages; empty = none):"
                                                          : "Header text ({page} = page number, {pages} = number of pages; empty = none):",
                     buf, sizeof(buf))) return;
    undo_point(2);
    if (footer) snprintf(ftr_text, sizeof(ftr_text), "%s", buf);
    else snprintf(hdr_text, sizeof(hdr_text), "%s", buf);
    if (buf[0]) {
        r = ui_popup_menu(al, 3, U.mx, U.my);
        if (r) { if (footer) ftr_align = r - 1; else hdr_align = r - 1; }
    }
}

/* ================= মেনু ================= */

enum {
    M_NEW = 1, M_OPEN, M_SAVE, M_SAVEAS, M_EXPORT, M_QUIT,
    M_UNDO, M_REDO, M_CUT, M_COPY, M_PASTE, M_ALL, M_FIND, M_FINDNEXT, M_REPLACE,
    M_BOLD, M_ITALIC, M_UNDER, M_STRIKE, M_BIGGER, M_SMALLER, M_FONT, M_SIZE, M_COLOR,
    M_LEFT, M_CENTER, M_RIGHT, M_JUSTIFY, M_BULLETS, M_NUMBERS,
    M_SP100, M_SP115, M_SP150, M_SP200,
    M_NORMAL, M_TITLE, M_H1, M_H2, M_H3,
    M_PBREAK, M_DATE,
    M_ZOOMIN, M_ZOOMOUT, M_ZOOM100,
    M_KEYS, M_ABOUT,
    /* ধাপ ২৬খ-২ */
    M_TABLE, M_PICTURE, M_PICSIZE, M_HEADER, M_FOOTER, M_PAGENUM,
    M_ROWADD, M_COLADD, M_ROWDEL, M_COLDEL, M_TBLDEL, M_COLWIDER, M_COLNARROW,
    M_SAVEDOCX                     /* ধাপ ২৬খ-৩ */
};

/* "Ctrl+I " (শেষে ফাঁকা) — শুধু দেখানোর জন্য: Ctrl+I/J/H-এর কোড Tab/Enter/Backspace-এর সমান,
 * তাই এগুলো handle_key নিজে Ctrl দেখে ধরে (মেনু-ফিতা ধরলে সাধারণ Tab-ও Italic হয়ে যেত) */
static const struct ui_menuitem file_items[] = {
    { "New", "Ctrl+N", M_NEW }, { "Open...", "Ctrl+O", M_OPEN }, { "Save", "Ctrl+S", M_SAVE },
    { "Save as...", "F12", M_SAVEAS }, { "Save as Word (.docx)...", NULL, M_SAVEDOCX },
    { "Export as text...", NULL, M_EXPORT }, { "-", NULL, 0 },
    { "Quit", "Ctrl+Q", M_QUIT },
};
static const struct ui_menuitem edit_items[] = {
    { "Undo", "Ctrl+Z", M_UNDO }, { "Redo", "Ctrl+Y", M_REDO }, { "-", NULL, 0 },
    { "Cut", "Ctrl+X", M_CUT }, { "Copy", "Ctrl+C", M_COPY }, { "Paste", "Ctrl+V", M_PASTE },
    { "Select all", "Ctrl+A", M_ALL }, { "-", NULL, 0 },
    { "Find...", "Ctrl+F", M_FIND }, { "Find next", "F3", M_FINDNEXT }, { "Replace...", "Ctrl+H ", M_REPLACE },
};
static const struct ui_menuitem format_items[] = {
    { "Bold", "Ctrl+B", M_BOLD }, { "Italic", "Ctrl+I ", M_ITALIC }, { "Underline", "Ctrl+U", M_UNDER },
    { "Strikethrough", NULL, M_STRIKE }, { "-", NULL, 0 },
    { "Font...", NULL, M_FONT }, { "Size...", NULL, M_SIZE }, { "Bigger", "Ctrl+> ", M_BIGGER },
    { "Smaller", "Ctrl+< ", M_SMALLER }, { "Color...", NULL, M_COLOR }, { "-", NULL, 0 },
    { "Align left", "Ctrl+L", M_LEFT }, { "Center", "Ctrl+E", M_CENTER }, { "Align right", "Ctrl+R", M_RIGHT },
    { "Justify", "Ctrl+J ", M_JUSTIFY }, { "-", NULL, 0 },
    { "Bullets", NULL, M_BULLETS }, { "Numbering", NULL, M_NUMBERS }, { "-", NULL, 0 },
    { "Line spacing 1.0", NULL, M_SP100 }, { "Line spacing 1.15", NULL, M_SP115 },
    { "Line spacing 1.5", NULL, M_SP150 }, { "Line spacing 2.0", NULL, M_SP200 }, { "-", NULL, 0 },
    { "Picture size...", NULL, M_PICSIZE },
};
static const struct ui_menuitem style_items[] = {
    { "Normal", "Ctrl+0 ", M_NORMAL }, { "Title", NULL, M_TITLE }, { "Heading 1", "Ctrl+1 ", M_H1 },
    { "Heading 2", "Ctrl+2 ", M_H2 }, { "Heading 3", "Ctrl+3 ", M_H3 },
};
static const struct ui_menuitem insert_items[] = {
    { "Page break (before this paragraph)", NULL, M_PBREAK }, { "Date and time", NULL, M_DATE }, { "-", NULL, 0 },
    { "Table...", NULL, M_TABLE }, { "Picture...", NULL, M_PICTURE }, { "-", NULL, 0 },
    { "Header...", NULL, M_HEADER }, { "Footer...", NULL, M_FOOTER }, { "Page numbers", NULL, M_PAGENUM },
};
static const struct ui_menuitem table_items[] = {
    { "Insert table...", NULL, M_TABLE }, { "-", NULL, 0 },
    { "Add row below", NULL, M_ROWADD }, { "Add column to the right", NULL, M_COLADD },
    { "Delete row", NULL, M_ROWDEL }, { "Delete column", NULL, M_COLDEL }, { "Delete table", NULL, M_TBLDEL },
    { "-", NULL, 0 }, { "Wider column", NULL, M_COLWIDER }, { "Narrower column", NULL, M_COLNARROW },
};
static const struct ui_menuitem view_items[] = {
    { "Zoom in", "Ctrl+= ", M_ZOOMIN }, { "Zoom out", "Ctrl+- ", M_ZOOMOUT }, { "Zoom 100%", NULL, M_ZOOM100 },
};
static const struct ui_menuitem help_items[] = {
    { "Keys", "F1", M_KEYS }, { "About rozword", NULL, M_ABOUT },
};
static const struct ui_menu menus[] = {
    { "File", file_items, (int)(sizeof(file_items) / sizeof(file_items[0])) },
    { "Edit", edit_items, (int)(sizeof(edit_items) / sizeof(edit_items[0])) },
    { "Format", format_items, (int)(sizeof(format_items) / sizeof(format_items[0])) },
    { "Styles", style_items, (int)(sizeof(style_items) / sizeof(style_items[0])) },
    { "Insert", insert_items, (int)(sizeof(insert_items) / sizeof(insert_items[0])) },
    { "Table", table_items, (int)(sizeof(table_items) / sizeof(table_items[0])) },
    { "View", view_items, (int)(sizeof(view_items) / sizeof(view_items[0])) },
    { "Help", help_items, (int)(sizeof(help_items) / sizeof(help_items[0])) },
};
#define NMENUS ((int)(sizeof(menus) / sizeof(menus[0])))

static const int sizes[] = { 8, 9, 10, 11, 12, 14, 16, 18, 20, 24, 28, 32, 36, 48, 60, 72 };
#define NSIZES ((int)(sizeof(sizes) / sizeof(sizes[0])))

struct named_color { const char *name; uint32_t rgb; };
static const struct named_color colors[] = {
    { "Black", 0x000000 }, { "Dark gray", 0x404040 }, { "Gray", 0x808080 }, { "Red", 0xC00000 },
    { "Orange", 0xE36C09 }, { "Gold", 0xBF8F00 }, { "Green", 0x00B050 }, { "Dark green", 0x375623 },
    { "Blue", 0x0070C0 }, { "Dark blue", 0x1F3864 }, { "Heading blue", 0x2F5496 }, { "Purple", 0x7030A0 },
    { "Teal", 0x008080 }, { "Brown", 0x833C0B },
};
#define NCOLORS ((int)(sizeof(colors) / sizeof(colors[0])))

static void pick_font(int x, int y)
{
    struct ui_menuitem it[NFAM];
    int i, n = 0, r;
    for (i = 0; i < NFAM; i++) {
        if (!fams[i].present) continue;
        it[n].label = fams[i].name;
        it[n].shortcut = NULL;
        it[n].id = i + 1;
        n++;
    }
    r = ui_popup_menu(it, n, x, y);
    if (r > 0) apply_attr(F_FONT, r - 1);
}

static void pick_size(int x, int y)
{
    static char lab[NSIZES][8];
    struct ui_menuitem it[NSIZES];
    int i, r;
    for (i = 0; i < NSIZES; i++) {
        snprintf(lab[i], sizeof(lab[i]), "%d", sizes[i]);
        it[i].label = lab[i];
        it[i].shortcut = NULL;
        it[i].id = sizes[i];
    }
    r = ui_popup_menu(it, NSIZES, x, y);
    if (r > 0) apply_attr(F_SIZE, r * 2);
}

static void pick_color(int x, int y)
{
    struct ui_menuitem it[NCOLORS];
    int i, r;
    for (i = 0; i < NCOLORS; i++) {
        it[i].label = colors[i].name;
        it[i].shortcut = NULL;
        it[i].id = i + 1;
    }
    r = ui_popup_menu(it, NCOLORS, x, y);
    if (r > 0) apply_attr(F_COLOR, (int)colors[r - 1].rgb);
}

static void pick_style(int x, int y)
{
    static const struct ui_menuitem it[] = {
        { "Normal", NULL, M_NORMAL }, { "Title", NULL, M_TITLE }, { "Heading 1", NULL, M_H1 },
        { "Heading 2", NULL, M_H2 }, { "Heading 3", NULL, M_H3 },
    };
    int r = ui_popup_menu(it, 5, x, y);
    if (r) apply_para(PA_STYLE, r - M_NORMAL);
}

static void set_zoom(int z)
{
    long old_h = doc_height();
    if (z < 50) z = 50;
    if (z > 250) z = 250;
    if (z == zoom) return;
    zoom = z;
    nfcache = 0;                                  /* (পুরোনো ফন্ট ছাড়া হয় না — ছোট লিক, zoom কম হয়) */
    layout_all = 1;
    paginate();
    if (old_h > 0) scroll_y = scroll_y * doc_height() / old_h;
    follow_cursor = 1;
    snprintf(status_msg, sizeof(status_msg), "Zoom %d%%", zoom);
}

static void show_keys(void)
{
    ui_msgbox("rozword - keys",
              "Ctrl+B/I/U  bold, italic, underline\n"
              "Ctrl+L/E/R/J  left, center, right, justify\n"
              "Ctrl+0..3  normal, heading 1-3\n"
              "Ctrl+> / Ctrl+<  bigger, smaller text\n"
              "Ctrl+Z/Y  undo, redo   Ctrl+X/C/V cut, copy, paste\n"
              "Ctrl+F find, F3 next, Ctrl+H replace\n"
              "Ctrl+S save, F12 save as, Ctrl+O open, Ctrl+N new\n"
              "Ctrl+Space  Bangla typing (Avro phonetic) on/off\n"
              "Ctrl+= / Ctrl+-  zoom; Ctrl+arrows: word by word\n"
              "Enter on an empty bullet ends the list\n"
              "Tables: Tab / Shift+Tab next / previous cell, Tab in the\n"
              "  last cell adds a row; Table menu: rows, columns, widths\n"
              "Insert > Picture, Header, Footer, Page numbers",
              UI_MB_OK | UI_MB_INFO);
}

static void do_action(int a)
{
    switch (a) {
    case M_NEW: if (confirm_discard()) new_doc(); break;
    case M_OPEN: {
        char p[512];
        if (!confirm_discard()) break;
        snprintf(p, sizeof(p), "%s", path);
        if (ui_filedlg("Open document", p, sizeof(p), 0) && load_doc(p) < 0) {
            char msg[700];
            snprintf(msg, sizeof(msg), "Cannot open %s", p);
            ui_msgbox("rozword", msg, UI_MB_OK | UI_MB_ERROR);
        }
        break;
    }
    case M_SAVE: do_save(); break;
    case M_SAVEAS: save_as(0); break;
    case M_EXPORT: save_as(1); break;
    case M_SAVEDOCX: save_as(2); break;
    case M_QUIT: if (confirm_discard()) ui_quit(&U); break;
    case M_UNDO: do_undo(0); break;
    case M_REDO: do_undo(1); break;
    case M_CUT: do_copy(1); break;
    case M_COPY: do_copy(0); break;
    case M_PASTE: do_paste(); break;
    case M_ALL: sp = 0; si = 0; cp = np - 1; ci = P[cp].n; break;
    case M_FIND:
        if (ui_inputbox("Find", "Find what:", find_text, sizeof(find_text))) find_next();
        break;
    case M_FINDNEXT:
        if (!find_text[0]) do_action(M_FIND);
        else find_next();
        break;
    case M_REPLACE:
        if (ui_inputbox("Replace", "Find what:", find_text, sizeof(find_text)) && find_text[0] &&
            ui_inputbox("Replace", "Replace with:", repl_text, sizeof(repl_text))) replace_all();
        break;
    case M_BOLD: toggle_flag(A_BOLD); break;
    case M_ITALIC: toggle_flag(A_ITALIC); break;
    case M_UNDER: toggle_flag(A_UNDER); break;
    case M_STRIKE: toggle_flag(A_STRIKE); break;
    case M_BIGGER: apply_attr(F_SIZE_STEP, 1); break;
    case M_SMALLER: apply_attr(F_SIZE_STEP, -1); break;
    case M_FONT: pick_font(U.mx, U.my); break;
    case M_SIZE: pick_size(U.mx, U.my); break;
    case M_COLOR: pick_color(U.mx, U.my); break;
    case M_LEFT: apply_para(PA_ALIGN, 0); break;
    case M_CENTER: apply_para(PA_ALIGN, 1); break;
    case M_RIGHT: apply_para(PA_ALIGN, 2); break;
    case M_JUSTIFY: apply_para(PA_ALIGN, 3); break;
    case M_BULLETS: apply_para(PA_LIST, 1); break;
    case M_NUMBERS: apply_para(PA_LIST, 2); break;
    case M_SP100: apply_para(PA_SPACING, 100); break;
    case M_SP115: apply_para(PA_SPACING, 115); break;
    case M_SP150: apply_para(PA_SPACING, 150); break;
    case M_SP200: apply_para(PA_SPACING, 200); break;
    case M_NORMAL: case M_TITLE: case M_H1: case M_H2: case M_H3: apply_para(PA_STYLE, a - M_NORMAL); break;
    case M_PBREAK: apply_para(PA_PBREAK, 0); break;
    case M_DATE: {
        char d[64];
        time_t t = time(NULL);
        struct tm *tm = localtime(&t);
        if (!tm || !strftime(d, sizeof(d), "%d %B %Y, %H:%M", tm)) strcpy(d, "?");
        undo_point(2);
        delete_selection();
        insert_plain(d, (int)strlen(d));
        break;
    }
    case M_ZOOMIN: set_zoom(zoom + 10); break;
    case M_ZOOMOUT: set_zoom(zoom - 10); break;
    case M_ZOOM100: set_zoom(100); break;
    case M_KEYS: show_keys(); break;
    case M_TABLE: insert_table(); break;
    case M_PICTURE: insert_picture(); break;
    case M_PICSIZE: picture_size(); break;
    case M_HEADER: edit_header_footer(0); break;
    case M_FOOTER: edit_header_footer(1); break;
    case M_PAGENUM:
        undo_point(2);
        snprintf(ftr_text, sizeof(ftr_text), "Page {page} of {pages}");
        ftr_align = 1;
        snprintf(status_msg, sizeof(status_msg), "Page numbers in the footer - Insert > Footer changes them");
        break;
    case M_ROWADD: case M_COLADD: case M_ROWDEL: case M_COLDEL: case M_TBLDEL: case M_COLWIDER: case M_COLNARROW:
        if (!P[cp].tbl) { ui_msgbox("rozword", "Click in a table first (Insert > Table makes one)", UI_MB_OK | UI_MB_INFO); break; }
        if (a == M_ROWADD) table_add_row();
        else if (a == M_COLADD) table_add_col();
        else if (a == M_ROWDEL) table_del_row();
        else if (a == M_COLDEL) table_del_col();
        else if (a == M_TBLDEL) table_delete();
        else table_col_width(a == M_COLWIDER ? 100 : -100);
        break;
    case M_ABOUT:
        ui_msgbox("About rozword", "rozword " RW_VERSION "\nA word processor for rozOS.\n"
                  "Documents: .rwd (rozword), Word .docx and .txt.\nTables, pictures, header and footer.\nBangla: Ctrl+Space (Avro phonetic).\nhelp rozword",
                  UI_MB_OK | UI_MB_INFO);
        break;
    }
    follow_cursor = 1;
}

/* ================= কী ================= */

static int is_wordc(int c) { return c != ' ' && c != '.' && c != ',' && c != ';' && c != ':' && c != '!' && c != '?'; }

static void move_word(int dir)
{
    Para *p = &P[cp];
    if (dir < 0) {
        if (ci == 0) { if (cp > 0) { cp--; ci = P[cp].n; } return; }
        while (ci > 0 && !is_wordc((unsigned char)p->t[ci - 1])) ci--;
        while (ci > 0 && is_wordc((unsigned char)p->t[ci - 1])) ci--;
        while (ci > 0 && !is_stop(p, ci)) ci--;
    } else {
        if (ci == p->n) { if (cp + 1 < np) { cp++; ci = 0; } return; }
        while (ci < p->n && is_wordc((unsigned char)p->t[ci])) ci++;
        while (ci < p->n && !is_wordc((unsigned char)p->t[ci])) ci++;
        while (ci < p->n && !is_stop(p, ci)) ci++;
    }
}

/* লাইন ধরে ওপর/নিচে (dy লাইন) */
static void move_lines(int dy)
{
    Para *p = &P[cp];
    Line *L = &p->ln[line_of(p, ci)];
    int g = gl_index(cp, line_of(p, ci)), x, best, bestd = 1 << 30, s, e, k;
    if (want_x < 0) want_x = x_in_line(p, L, ci);
    x = want_x;
    g += dy;
    if (g < 0) { cp = 0; ci = 0; return; }
    if (g >= ngl) { cp = np - 1; ci = P[cp].n; return; }
    cp = gl_p[g];
    p = &P[cp];
    k = gl_l[g];
    L = &p->ln[k];
    s = L->s;
    e = (k < p->nl - 1 && L->e > L->s) ? prev_stop(p, L->e) : L->e;
    best = s;
    for (;;) {
        int d = x_in_line(p, L, s) - x;
        if (d < 0) d = -d;
        if (d < bestd) { bestd = d; best = s; }
        if (s >= e) break;
        s = next_stop(p, s);
        if (s > e) s = e;
    }
    ci = best;
}

static void handle_key(int k, int mods)
{
    int shift = mods & GFX_MOD_SHIFT, ctrl = mods & GFX_MOD_CTRL;
    int nav = k >= GFX_KEY_UP && k <= GFX_KEY_PAGE_DOWN;
    if (rozbn_is_toggle_key(k, mods)) {
        rozbn_reset(&bcomp);
        snprintf(status_msg, sizeof(status_msg), rozbn_toggle() ? "Bangla typing (Avro phonetic) - Ctrl+Space for English"
                                                                : "English typing - Ctrl+Space for Bangla");
        return;
    }
    if (nav) {
        int lines = view_h * 9 / 10 / (zpx(22) > 8 ? zpx(22) : 8);
        rozbn_reset(&bcomp);
        if (k != GFX_KEY_UP && k != GFX_KEY_DOWN && k != GFX_KEY_PAGE_UP && k != GFX_KEY_PAGE_DOWN) want_x = -1;
        switch (k) {
        case GFX_KEY_LEFT:
            if (!shift && (cp != sp || ci != si)) {        /* নির্বাচন থাকলে তার শুরুতে */
                int p1, i1, p2, i2;
                sel_range(&p1, &i1, &p2, &i2);
                cp = p1; ci = i1;
            } else if (ctrl) move_word(-1);
            else if (ci > 0) ci = prev_stop(&P[cp], ci);
            else if (cp > 0) { cp--; ci = P[cp].n; }
            break;
        case GFX_KEY_RIGHT:
            if (!shift && (cp != sp || ci != si)) {
                int p1, i1, p2, i2;
                sel_range(&p1, &i1, &p2, &i2);
                cp = p2; ci = i2;
            } else if (ctrl) move_word(1);
            else if (ci < P[cp].n) ci = next_stop(&P[cp], ci);
            else if (cp + 1 < np) { cp++; ci = 0; }
            break;
        case GFX_KEY_UP: move_lines(-1); break;
        case GFX_KEY_DOWN: move_lines(1); break;
        case GFX_KEY_PAGE_UP: move_lines(-lines); scroll_y -= view_h * 9 / 10; break;
        case GFX_KEY_PAGE_DOWN: move_lines(lines); scroll_y += view_h * 9 / 10; break;
        case GFX_KEY_HOME:
            if (ctrl) { cp = 0; ci = 0; }
            else ci = P[cp].ln[line_of(&P[cp], ci)].s;
            break;
        case GFX_KEY_END:
            if (ctrl) { cp = np - 1; ci = P[cp].n; }
            else {
                Para *p = &P[cp];
                int ln = line_of(p, ci);
                ci = ln < p->nl - 1 ? prev_stop(p, p->ln[ln].e) : p->n;
            }
            break;
        }
        if (!shift) { sp = cp; si = ci; }
        follow_cursor = 1;
        return;
    }
    want_x = -1;
    /* Ctrl দিয়ে, মেনু-ফিতা যেগুলো ধরে না */
    if (ctrl) {
        switch (k) {
        case 9: do_action(M_ITALIC); return;                 /* Ctrl+I (Tab-এর কোড) */
        case 10: case 13: do_action(M_JUSTIFY); return;      /* Ctrl+J (Enter-এর কোড) */
        case 8: do_action(M_REPLACE); return;                /* Ctrl+H (Backspace-এর কোড) */
        case '0': case '1': case '2': case '3': do_action(M_NORMAL + (k - '0' == 0 ? 0 : k - '0' + 1)); return;
        case '=': case '+': do_action(M_ZOOMIN); return;
        case '-': do_action(M_ZOOMOUT); return;
        case '>': case '.': do_action(M_BIGGER); return;
        case '<': case ',': do_action(M_SMALLER); return;
        case GFX_KEY_DELETE: case 127: {                     /* Ctrl+Delete: পরের শব্দ মোছা */
            int p1, i1, p2, i2;
            undo_point(2);
            if (!sel_range(&p1, &i1, &p2, &i2)) { sp = cp; si = ci; move_word(1); }
            if (sel_range(&p1, &i1, &p2, &i2)) delete_range(p1, i1, p2, i2);
            follow_cursor = 1;
            return;
        }
        default: break;
        }
    }
    if (rozbn_mode() && !ctrl) {                         /* অভ্র phonetic */
        int del = 0, had = bcomp.rlen > 0;
        char ins[256];
        if (rozbn_key(&bcomp, k, &del, ins, sizeof(ins))) {
            undo_point(1);
            if (!had) delete_selection();
            if (del > ci) del = ci;
            if (del) { para_delete_bytes(&P[cp], ci - del, ci); ci -= del; sp = cp; si = ci; }
            if (ins[0]) insert_plain(ins, (int)strlen(ins));
            follow_cursor = 1;
            return;
        }
    } else {
        rozbn_reset(&bcomp);
    }
    switch (k) {
    case GFX_KEY_ESC: sp = cp; si = ci; return;
    case '\n': case '\r':
        undo_point(2);
        delete_selection();
        split_para();
        break;
    case '\b':
        if (cp != sp || ci != si) { undo_point(2); delete_selection(); break; }
        if (ci > 0) {
            int pi = prev_stop(&P[cp], ci);
            /* বাংলা ক্লাস্টারের শেষ চিহ্নটা (কার, হসন্ত) আলাদা করে মোছে — টাইপের ভুল শোধরাতে সুবিধা */
            int j = ci - 1;
            while (j > 0 && ((unsigned char)P[cp].t[j] & 0xC0) == 0x80) j--;
            undo_point(1);
            if (j > pi) pi = j;
            para_delete_bytes(&P[cp], pi, ci);
            ci = pi;
        } else if (P[cp].kind == 1) {               /* ধাপ ২৬খ-২: ছবি মোছা */
            int had_prev = cp > 0 && same_cell(cp - 1, cp), before = np;
            undo_point(2);
            remove_para_keep_cell(cp);
            if (np < before && had_prev) { cp--; ci = P[cp].n; }
            else { if (cp >= np) cp = np - 1; ci = 0; }
        } else if (P[cp].list) {
            undo_point(2);
            P[cp].list = 0;
            P[cp].dirty = 1;
            layout_all = 1;
        } else if (cp > 0 && P[cp - 1].kind == 1 && same_cell(cp - 1, cp)) {   /* আগের ছবিটা মোছা */
            undo_point(2);
            paras_remove(cp - 1, cp);                 /* একই ঘরে cp আছে, তাই ঘর খালি হয় না */
            cp--;
            ci = 0;
        } else if (cp > 0 && !same_cell(cp - 1, cp)) {   /* টেবিলের ঘরের শুরু / টেবিলের ঠিক পরে: জোড়া নয় */
            if (!P[cp].tbl && P[cp].n == 0 && P[cp - 1].tbl && cp + 1 < np) {
                undo_point(2);                          /* টেবিলের পরের খালি অনুচ্ছেদ মোছা */
                paras_remove(cp, cp + 1);
                cp--;
                ci = P[cp].n;
            } else if (!P[cp].tbl) {
                cp--;
                ci = P[cp].n;
            }
        } else if (cp > 0) {
            int at = P[cp - 1].n;
            undo_point(2);
            join_with_next(cp - 1);
            cp--;
            ci = at;
        }
        sp = cp;
        si = ci;
        break;
    case GFX_KEY_DELETE:
        if (cp != sp || ci != si) { undo_point(2); delete_selection(); break; }
        undo_point(1);
        if (P[cp].kind == 1) {                        /* ধাপ ২৬খ-২ */
            remove_para_keep_cell(cp);
            if (cp >= np) cp = np - 1;
            ci = 0;
        } else if (ci < P[cp].n) para_delete_bytes(&P[cp], ci, next_stop(&P[cp], ci));
        else if (cp + 1 < np && same_cell(cp, cp + 1) && P[cp + 1].kind == 1) remove_para_keep_cell(cp + 1);
        else if (cp + 1 < np && same_cell(cp, cp + 1)) join_with_next(cp);
        sp = cp;
        si = ci;
        break;
    case '\t':
        if (P[cp].tbl) { table_tab(mods & GFX_MOD_SHIFT ? -1 : 1); break; }   /* ধাপ ২৬খ-২: পরের ঘর */
        undo_point(1);
        delete_selection();
        insert_plain("    ", 4);
        break;
    default:
        if (k >= 32 && k < 127 && !ctrl) {
            char c = (char)k;
            undo_point(k == ' ' ? 2 : 1);
            delete_selection();
            insert_plain(&c, 1);
        }
        break;
    }
    follow_cursor = 1;
}

/* ================= টুলবার আর ফ্রেম ================= */

#define TOOL_H   32
#define STATUS_H 22

/* চেপে রাখা যায় এমন বোতাম (B, I, U ...) — on হলে বসানো দেখায়; ক্লিকে 1 */
static int toggle_button(int x, int y, int w, int h, const char *label, int on)
{
    int hov = ui_hover(&U, x, y, w, h);
    ui_button(U.s, x, y, w, h, label, on ? UI_DOWN : hov ? UI_HOVER : UI_NORMAL);
    return ui_clicked(&U, x, y, w, h);
}

/* alignment-এর ছবি: চারটা রেখা */
static void align_icon(int x, int y, int kind, uint32_t c)
{
    static const int lens[4] = { 14, 9, 14, 7 };
    int i;
    for (i = 0; i < 4; i++) {
        int l = kind == 3 ? 14 : lens[i], lx = x;
        if (kind == 1) lx = x + (14 - l) / 2;
        else if (kind == 2) lx = x + 14 - l;
        gfx_fill_rect(U.s, lx, y + i * 4, l, 2, c);
    }
}

/* ড্রপ-ডাউন বোতাম: লেখা + ছোট ত্রিভুজ; ক্লিকে 1 */
static int drop_button(int x, int y, int w, int h, const char *label)
{
    int r = ui_do_button(&U, x, y, w, h, label), cx = x + w - 12, cy = y + h / 2 - 2;
    gfx_fill_triangle(U.s, cx - 4, cy, cx + 4, cy, cx, cy + 4, ui_theme_get()->text);
    return r;
}

static void tool_sep(int *x, int y)
{
    const struct ui_theme *t = ui_theme_get();
    gfx_vline(U.s, *x + 2, y + 2, TOOL_H - 10, t->dark);
    gfx_vline(U.s, *x + 3, y + 2, TOOL_H - 10, t->light2);
    *x += 8;
}

static void toolbar(void)
{
    const struct ui_theme *t = ui_theme_get();
    int x = 4, y = UI_MENUBAR_H + 3, h = TOOL_H - 6, k;
    const Attr *a = &attrs[cur_attr()];
    char lab[64];
    static const char *style_names[5] = { "Normal", "Title", "Heading 1", "Heading 2", "Heading 3" };
    ui_panel(U.s, 0, UI_MENUBAR_H, U.w, TOOL_H, t->face, 1);
    if (ui_do_tool(&U, x, y, 30, h, "new", 'd', NULL)) do_action(M_NEW);
    x += 32;
    if (ui_do_tool(&U, x, y, 30, h, "open", 'f', NULL)) do_action(M_OPEN);
    x += 32;
    if (ui_do_tool(&U, x, y, 30, h, "save", 'v', NULL)) do_action(M_SAVE);
    x += 32;
    tool_sep(&x, y);
    snprintf(lab, sizeof(lab), "%s   ", fams[a->font < NFAM ? a->font : 0].name);
    if (drop_button(x, y, 150, h, lab)) pick_font(x, y + h);
    x += 152;
    snprintf(lab, sizeof(lab), "%d%s   ", a->size2 / 2, a->size2 & 1 ? ".5" : "");
    if (drop_button(x, y, 52, h, lab)) pick_size(x, y + h);
    x += 54;
    snprintf(lab, sizeof(lab), "%s   ", style_names[P[cp].style < 5 ? P[cp].style : 0]);
    if (drop_button(x, y, 100, h, lab)) pick_style(x, y + h);
    x += 102;
    tool_sep(&x, y);
    if (toggle_button(x, y, 26, h, "B", a->flags & A_BOLD)) do_action(M_BOLD);
    x += 28;
    if (toggle_button(x, y, 26, h, "I", a->flags & A_ITALIC)) do_action(M_ITALIC);
    x += 28;
    if (toggle_button(x, y, 26, h, "U", a->flags & A_UNDER)) do_action(M_UNDER);
    x += 28;
    if (toggle_button(x, y, 26, h, "S", a->flags & A_STRIKE)) do_action(M_STRIKE);
    x += 28;
    if (toggle_button(x, y, 26, h, "", 0)) pick_color(x, y + h);
    gfx_fill_rect(U.s, x + 6, y + 6, 14, h - 12, 0xFF000000u | a->color);
    gfx_rect(U.s, x + 6, y + 6, 14, h - 12, t->dark);
    x += 28;
    tool_sep(&x, y);
    for (k = 0; k < 4; k++) {
        if (toggle_button(x, y, 26, h, "", P[cp].align == k)) do_action(M_LEFT + k);
        align_icon(x + 6, y + 5, k, t->text);
        x += 28;
    }
    tool_sep(&x, y);
    if (toggle_button(x, y, 30, h, "\xE2\x80\xA2 -", P[cp].list == 1)) do_action(M_BULLETS);
    x += 32;
    if (toggle_button(x, y, 30, h, "1.", P[cp].list == 2)) do_action(M_NUMBERS);
    x += 32;
}

static void select_word_at(void)
{
    Para *p = &P[cp];
    int a = ci, b = ci;
    while (a > 0 && is_wordc((unsigned char)p->t[a - 1])) a--;
    while (b < p->n && is_wordc((unsigned char)p->t[b])) b++;
    while (a > 0 && !is_stop(p, a)) a--;
    while (b < p->n && !is_stop(p, b)) b++;
    sp = cp; si = a; ci = b;
}

static void frame(void)
{
    const struct ui_theme *t = ui_theme_get();
    int W = U.w, H = U.h, act;
    /* Ctrl+I/J/H মেনু-ফিতার আগে (ওদের কোড Tab/Enter/Backspace) */
    if (U.key && (U.mods & GFX_MOD_CTRL) && (U.key == 9 || U.key == 10 || U.key == 13 || U.key == 8)) {
        paginate();
        handle_key(U.key, U.mods);
        U.key = 0;
    }
    act = ui_menubar(&U, 0, 0, W, menus, NMENUS);
    if (act) do_action(act);
    paginate();
    toolbar();
    view_x0 = 0;
    view_y0 = UI_MENUBAR_H + TOOL_H;
    view_w = W - 16;
    view_h = H - view_y0 - STATUS_H;
    if (view_h < 40) view_h = 40;
    paginate();
    if (U.key == GFX_KEY_F1) { show_keys(); U.key = 0; }
    if (U.key) {
        handle_key(U.key, U.mods);
        U.key = 0;
        paginate();
    }
    /* মাউস */
    {
        int over = ui_hover(&U, view_x0, view_y0, view_w, view_h);
        if (over && U.wheel) {
            if (U.mods & GFX_MOD_CTRL) set_zoom(zoom - U.wheel * 10);
            else scroll_y += U.wheel * 60;
        }
        if (over && U.pressed) {
            int p, i;
            rozbn_reset(&bcomp);
            hit_test(U.mx, U.my, &p, &i);
            cp = p;
            ci = i;
            if (!(U.mods & GFX_MOD_SHIFT)) { sp = cp; si = ci; }
            want_x = -1;
            dragging = 1;
            if (U.dclick) { select_word_at(); dragging = 0; }
        } else if (dragging && (U.buttons & GFX_BUTTON_LEFT)) {
            int p, i;
            if (U.my < view_y0) scroll_y -= 30;
            if (U.my > view_y0 + view_h) scroll_y += 30;
            hit_test(U.mx, U.my, &p, &i);
            cp = p;
            ci = i;
        }
        if (!(U.buttons & GFX_BUTTON_LEFT)) dragging = 0;
        if (over && U.rpressed) {
            static const struct ui_menuitem pm[] = {
                { "Cut", "Ctrl+X", M_CUT }, { "Copy", "Ctrl+C", M_COPY }, { "Paste", "Ctrl+V", M_PASTE },
                { "-", NULL, 0 }, { "Bold", "Ctrl+B", M_BOLD }, { "Italic", "Ctrl+I", M_ITALIC },
                { "Underline", "Ctrl+U", M_UNDER }, { "-", NULL, 0 }, { "Select all", "Ctrl+A", M_ALL },
            };
            int r = ui_popup_menu(pm, (int)(sizeof(pm) / sizeof(pm[0])), U.mx, U.my);
            if (r) do_action(r);
        }
    }
    paginate();
    /* কার্সর দেখা যায় এমন স্ক্রল */
    if (follow_cursor) {
        Para *p = &P[cp];
        Line *L = &p->ln[line_of(p, ci)];
        long top = line_top_doc(L);
        if (top < scroll_y + 10) scroll_y = top - 10;
        if (top + L->h > scroll_y + view_h - 10) scroll_y = top + L->h - view_h + 10;
        follow_cursor = 0;
    }
    if (scroll_y > doc_height() - view_h) scroll_y = doc_height() - view_h;
    if (scroll_y < 0) scroll_y = 0;
    gfx_set_clip(U.s, view_x0, view_y0, view_w, view_h);
    draw_pages(U.s);
    gfx_reset_clip(U.s);
    {
        int pos = (int)scroll_y, total = (int)doc_height();
        if (ui_do_vscroll(&U, W - 16, view_y0, 16, view_h, &pos, total, view_h)) scroll_y = pos;
    }
    /* অবস্থা-লাইন */
    {
        char st[320];
        Para *p = &P[cp];
        int page = p->ln[line_of(p, ci)].page + 1;
        snprintf(st, sizeof(st), "Page %d of %d  |  %d words  |  Zoom %d%%  |  %s  |  %s%s", page, npages, word_count(),
                 zoom, rozbn_mode() ? "Bangla" : "English", modified ? "modified  |  " : "", status_msg);
        ui_statusbar(U.s, 0, H - STATUS_H, W, STATUS_H, st);
    }
    (void)t;
}

static void update_title(void)
{
    char t[600];
    snprintf(t, sizeof(t), "rozword - %s%s", base_name(path), modified ? " *" : "");
    gfx_set_title(t);
}

int main(int argc, char **argv)
{
    int was_mod = -1;
    char was_path[512] = "";
    if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        printf("usage: rozword [FILE.rwd | FILE.docx | FILE.txt]   - word processor (help rozword)\n"
               "       rozword -c IN OUT    convert between .rwd, .docx and .txt (no window)\n");
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "-c")) {           /* ধাপ ২৬খ-৩: উইন্ডো ছাড়া রূপান্তর */
        if (argc < 4) { fprintf(stderr, "usage: rozword -c IN OUT\n"); return 2; }
        cli_mode = 1;
        fonts_init();
        doc_clear();
        if (load_doc(argv[2]) < 0) { fprintf(stderr, "rozword: cannot open %s\n", argv[2]); return 1; }
        if (save_doc(argv[3]) < 0) return 1;
        printf("%s -> %s (%d paragraphs%s)\n", argv[2], argv[3], np, docx_skipped ? ", some objects left out" : "");
        return 0;
    }
    if (gfx_init_window("rozword", 960, 660) < 0) {
        perror("rozword: no screen");
        return 1;
    }
    fonts_init();
    new_doc();
    if (argc > 1) {
        if (load_doc(argv[1]) < 0) {
            struct stat st;
            if (stat(argv[1], &st) == 0) {
                char msg[700];
                snprintf(msg, sizeof(msg), "Cannot open %s", argv[1]);
                ui_msgbox("rozword", msg, UI_MB_OK | UI_MB_ERROR);
            } else {                                     /* নেই = এই নামে নতুন */
                snprintf(path, sizeof(path), "%s", argv[1]);
                untitled = 0;
                snprintf(status_msg, sizeof(status_msg), "New document %s", base_name(path));
            }
        }
    } else {
        snprintf(status_msg, sizeof(status_msg), "F1 keys  |  Ctrl+Space Bangla  |  F10 menu");
    }
    ui_init(&U);
    ui_ask_before_close(&U, 1);
    U.tick_ms = 500;                                     /* কার্সর ঝলকানো */
    while (ui_run(&U)) {
        if (U.closing) {
            if (confirm_discard()) { ui_quit(&U); continue; }
            U.closing = 0;
        }
        frame();
        if (modified != was_mod || strcmp(path, was_path)) {
            was_mod = modified;
            snprintf(was_path, sizeof(was_path), "%s", path);
            update_title();
        }
    }
    gfx_quit();
    return 0;
}
