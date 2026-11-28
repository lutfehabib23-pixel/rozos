/* rozpdf — পাতা আঁকা: content stream, path, লেখা, ছবি (pdf.h দেখুন)
 *
 * স্থানাঙ্ক: PDF-এর নিয়মে matrix [a b c d e f] মানে x' = a·x + c·y + e, y' = b·x + d·y + f;
 * device = পিক্সেল, y নিচের দিকে। প্রতিটা পাতা আলাদা surface-এ সাদা পটে আঁকা হয়।
 * path: Bezier ভেঙে রেখা, তারপর ৪টা উপ-রেখায় (sub-scanline) anti-aliased ভরাট (nonzero /
 * even-odd)। clip: path-এর বাক্সের সাথে মেলানো আয়তক্ষেত্র (সরলীকরণ)।
 * লেখা: PDF-এর ভেতরের ফন্ট (TrueType, CFF, OpenType; Type1 নিজের module দিয়ে) FreeType-এ
 * মেমোরি থেকে খোলা; না থাকলে নামের মিল দেখে rozOS-এর ফন্ট (bold/italic কৃত্রিম)।
 * Type3 ফন্ট: প্রতিটা অক্ষরের নিজের content stream। */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "pdf.h"
#include "rozgfx.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYNTHESIS_H
#include FT_MODULE_H
#include FT_ADVANCES_H

#ifdef ROZPDF_TYPE1
/* type1.c-র driver (আসল ধরন FT_Driver_ClassRec — তার শুরুতেই FT_Module_Class) */
extern const FT_Module_Class t1_driver_class;
#endif

/* ---------- matrix ---------- */

typedef struct { double a, b, c, d, e, f; } mat;

static mat mat_mul(mat m, mat n)                       /* আগে m, তারপর n */
{
    mat r;
    r.a = m.a * n.a + m.b * n.c;
    r.b = m.a * n.b + m.b * n.d;
    r.c = m.c * n.a + m.d * n.c;
    r.d = m.c * n.b + m.d * n.d;
    r.e = m.e * n.a + m.f * n.c + n.e;
    r.f = m.e * n.b + m.f * n.d + n.f;
    return r;
}

static void mat_apply(const mat *m, double x, double y, double *ox, double *oy)
{
    *ox = m->a * x + m->c * y + m->e;
    *oy = m->b * x + m->d * y + m->f;
}

static int mat_inv(const mat *m, mat *r)
{
    double det = m->a * m->d - m->b * m->c;
    if (fabs(det) < 1e-12) return -1;
    r->a = m->d / det;
    r->b = -m->b / det;
    r->c = -m->c / det;
    r->d = m->a / det;
    r->e = -(m->e * r->a + m->f * r->c);
    r->f = -(m->e * r->b + m->f * r->d);
    return 0;
}

static mat mat_from_array(pdf_doc *d, pobj *a, mat def)
{
    mat m = def;
    a = pdf_resolve(d, a);
    if (a && a->type == P_ARRAY && a->u.arr.n >= 6) {
        m.a = pdf_num(pdf_resolve(d, a->u.arr.v[0]), 1);
        m.b = pdf_num(pdf_resolve(d, a->u.arr.v[1]), 0);
        m.c = pdf_num(pdf_resolve(d, a->u.arr.v[2]), 0);
        m.d = pdf_num(pdf_resolve(d, a->u.arr.v[3]), 1);
        m.e = pdf_num(pdf_resolve(d, a->u.arr.v[4]), 0);
        m.f = pdf_num(pdf_resolve(d, a->u.arr.v[5]), 0);
    }
    return m;
}

/* ---------- ফন্ট ---------- */

typedef struct tu_ent { uint32_t lo, hi, uni; int pool, npool; } tu_ent;   /* ToUnicode */
typedef struct cs_rng { uint32_t lo, hi; int nbytes; } cs_rng;             /* codespace */
typedef struct cid_rng { uint32_t lo, hi, cid; } cid_rng;                  /* encoding CMap */
typedef struct w_rng { uint32_t lo, hi; double w; } w_rng;                 /* CID widths */

typedef struct pfont {
    struct pfont *next;
    pdf_doc *doc;
    pobj *dict;
    int kind;                         /* 1 = সাধারণ, 0 = Type0, 3 = Type3 */
    FT_Face face;
    unsigned char *fontdata;
    int substituted, bold, italic, symbolic, is_sfnt;
    /* সাধারণ */
    double widths[256];
    int has_widths;
    double missing;
    char *names[256];
    uint32_t uni[256];
    int gid[256];
    /* Type0 */
    int two_byte;
    cs_rng *cs; int ncs;
    cid_rng *cmap; int ncmap; int identity;
    w_rng *wr; int nwr; double dw;
    uint16_t *cid2gid; long ncid2gid;
    int vertical;
    /* ToUnicode */
    tu_ent *tu; int ntu; uint32_t *pool; int npool;
    /* Type3 */
    mat fmatrix;
    pobj *charprocs, *resources;
} pfont;

static FT_Library ftlib;
static int ft_ok;
static pfont *fonts;
static char warn[200];

const char *pdf_render_warning(void) { return warn; }

void pdf_render_init(void)
{
    if (ft_ok) return;
    if (FT_Init_FreeType(&ftlib) == 0) {
        ft_ok = 1;
#ifdef ROZPDF_TYPE1
        FT_Add_Module(ftlib, &t1_driver_class);
#endif
    } else {
        ft_ok = -1;
    }
}

void pdf_render_forget(pdf_doc *d)
{
    pfont **pp = &fonts;
    while (*pp) {
        pfont *f = *pp;
        if (f->doc == d) {
            int i;
            *pp = f->next;
            if (f->face) FT_Done_Face(f->face);
            free(f->fontdata);
            for (i = 0; i < 256; i++) free(f->names[i]);
            free(f->cs); free(f->cmap); free(f->wr); free(f->cid2gid); free(f->tu); free(f->pool);
            free(f);
        } else {
            pp = &f->next;
        }
    }
}

/* --- glyph নাম ↔ Unicode --- */

static const char *const ascii_names[95] = {
    "space", "exclam", "quotedbl", "numbersign", "dollar", "percent", "ampersand", "quotesingle", "parenleft",
    "parenright", "asterisk", "plus", "comma", "hyphen", "period", "slash", "zero", "one", "two", "three", "four",
    "five", "six", "seven", "eight", "nine", "colon", "semicolon", "less", "equal", "greater", "question", "at",
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V",
    "W", "X", "Y", "Z", "bracketleft", "backslash", "bracketright", "asciicircum", "underscore", "grave",
    "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o", "p", "q", "r", "s", "t", "u", "v",
    "w", "x", "y", "z", "braceleft", "bar", "braceright", "asciitilde"
};
static const char *const latin1_names[96] = {
    "space", "exclamdown", "cent", "sterling", "currency", "yen", "brokenbar", "section", "dieresis", "copyright",
    "ordfeminine", "guillemotleft", "logicalnot", "hyphen", "registered", "macron", "degree", "plusminus",
    "twosuperior", "threesuperior", "acute", "mu", "paragraph", "periodcentered", "cedilla", "onesuperior",
    "ordmasculine", "guillemotright", "onequarter", "onehalf", "threequarters", "questiondown", "Agrave", "Aacute",
    "Acircumflex", "Atilde", "Adieresis", "Aring", "AE", "Ccedilla", "Egrave", "Eacute", "Ecircumflex", "Edieresis",
    "Igrave", "Iacute", "Icircumflex", "Idieresis", "Eth", "Ntilde", "Ograve", "Oacute", "Ocircumflex", "Otilde",
    "Odieresis", "multiply", "Oslash", "Ugrave", "Uacute", "Ucircumflex", "Udieresis", "Yacute", "Thorn",
    "germandbls", "agrave", "aacute", "acircumflex", "atilde", "adieresis", "aring", "ae", "ccedilla", "egrave",
    "eacute", "ecircumflex", "edieresis", "igrave", "iacute", "icircumflex", "idieresis", "eth", "ntilde", "ograve",
    "oacute", "ocircumflex", "otilde", "odieresis", "divide", "oslash", "ugrave", "uacute", "ucircumflex",
    "udieresis", "yacute", "thorn", "ydieresis"
};
static const struct { const char *n; uint32_t u; } extra_names[] = {
    { "Euro", 0x20AC }, { "quotesinglbase", 0x201A }, { "florin", 0x0192 }, { "quotedblbase", 0x201E },
    { "ellipsis", 0x2026 }, { "dagger", 0x2020 }, { "daggerdbl", 0x2021 }, { "circumflex", 0x02C6 },
    { "perthousand", 0x2030 }, { "Scaron", 0x0160 }, { "guilsinglleft", 0x2039 }, { "OE", 0x0152 },
    { "Zcaron", 0x017D }, { "quoteleft", 0x2018 }, { "quoteright", 0x2019 }, { "quotedblleft", 0x201C },
    { "quotedblright", 0x201D }, { "bullet", 0x2022 }, { "endash", 0x2013 }, { "emdash", 0x2014 },
    { "tilde", 0x02DC }, { "trademark", 0x2122 }, { "scaron", 0x0161 }, { "guilsinglright", 0x203A },
    { "oe", 0x0153 }, { "zcaron", 0x017E }, { "Ydieresis", 0x0178 }, { "fi", 0xFB01 }, { "fl", 0xFB02 },
    { "ff", 0xFB00 }, { "ffi", 0xFB03 }, { "ffl", 0xFB04 }, { "dotlessi", 0x0131 }, { "Lslash", 0x0141 },
    { "lslash", 0x0142 }, { "minus", 0x2212 }, { "fraction", 0x2044 }, { "breve", 0x02D8 }, { "dotaccent", 0x02D9 },
    { "ring", 0x02DA }, { "hungarumlaut", 0x02DD }, { "ogonek", 0x02DB }, { "caron", 0x02C7 },
    { "nbspace", 0x00A0 }, { "sfthyphen", 0x00AD }, { "nonbreakingspace", 0x00A0 }, { "periodcentered", 0x00B7 },
    { "alpha", 0x03B1 }, { "beta", 0x03B2 }, { "gamma", 0x03B3 }, { "delta", 0x03B4 }, { "epsilon", 0x03B5 },
    { "pi", 0x03C0 }, { "sigma", 0x03C3 }, { "mu1", 0x00B5 }, { "Delta", 0x2206 }, { "Omega", 0x2126 },
    { "summation", 0x2211 }, { "integral", 0x222B }, { "infinity", 0x221E }, { "lessequal", 0x2264 },
    { "greaterequal", 0x2265 }, { "notequal", 0x2260 }, { "approxequal", 0x2248 }, { "radical", 0x221A },
    { "arrowright", 0x2192 }, { "arrowleft", 0x2190 }, { "periodcentered", 0x00B7 }, { "copyrightserif", 0x00A9 },
    { "registerserif", 0x00AE }, { "trademarkserif", 0x2122 },
};

static uint32_t name_to_uni(const char *n)
{
    int i;
    if (!n || !*n) return 0;
    if (n[0] == 'u' && n[1] == 'n' && n[2] == 'i' && strlen(n) >= 7) {
        uint32_t v = (uint32_t)strtoul(n + 3, NULL, 16);
        char h[5];
        memcpy(h, n + 3, 4);
        h[4] = 0;
        v = (uint32_t)strtoul(h, NULL, 16);
        return v;
    }
    if (n[0] == 'u' && strlen(n) >= 5 && strlen(n) <= 7) {
        char *e;
        uint32_t v = (uint32_t)strtoul(n + 1, &e, 16);
        if (!*e) return v;
    }
    for (i = 0; i < 95; i++) if (!strcmp(ascii_names[i], n)) return (uint32_t)(0x20 + i);
    for (i = 1; i < 96; i++) if (!strcmp(latin1_names[i], n)) return (uint32_t)(0xA0 + i);
    for (i = 0; i < (int)(sizeof(extra_names) / sizeof(extra_names[0])); i++)
        if (!strcmp(extra_names[i].n, n)) return extra_names[i].u;
    {                                                  /* "a65" / "g65" / "cid65" ধাঁচ: অজানা */
        const char *dot = strchr(n, '.');              /* "A.sc", "f_i" */
        if (dot && dot > n) {
            char base[64];
            size_t l = (size_t)(dot - n) < sizeof(base) - 1 ? (size_t)(dot - n) : sizeof(base) - 1;
            memcpy(base, n, l);
            base[l] = 0;
            return name_to_uni(base);
        }
    }
    return 0;
}

/* WinAnsi (cp1252): কোড → Unicode আর নাম */
static const uint16_t win80[32] = {
    0x20AC, 0x2022, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x2022,
    0x017D, 0x2022, 0x2022, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A,
    0x0153, 0x2022, 0x017E, 0x0178
};

static uint32_t winansi_uni(int c)
{
    if (c >= 0x20 && c < 0x7F) return (uint32_t)c;
    if (c >= 0x80 && c < 0xA0) return win80[c - 0x80];
    if (c >= 0xA0) return (uint32_t)c;
    return 0;
}

static const char *winansi_name(int c)
{
    int i;
    uint32_t u;
    if (c >= 0x20 && c < 0x7F) return ascii_names[c - 0x20];
    if (c >= 0xA0) return latin1_names[c - 0xA0];
    if (c < 0x80) return NULL;
    u = win80[c - 0x80];
    for (i = 0; i < (int)(sizeof(extra_names) / sizeof(extra_names[0])); i++) if (extra_names[i].u == u) return extra_names[i].n;
    return NULL;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* --- CMap (ToUnicode / encoding) পড়া --- */

static uint32_t bytes_be(const unsigned char *s, int n)
{
    uint32_t v = 0;
    int i;
    for (i = 0; i < n && i < 4; i++) v = (v << 8) | s[i];
    return v;
}

static void tu_add(pfont *f, uint32_t lo, uint32_t hi, uint32_t uni, const uint32_t *multi, int nmulti)
{
    tu_ent *n;
    if ((f->ntu & 63) == 0) {
        n = (tu_ent *)realloc(f->tu, sizeof(tu_ent) * (size_t)(f->ntu + 64));
        if (!n) return;
        f->tu = n;
    }
    n = &f->tu[f->ntu++];
    n->lo = lo; n->hi = hi; n->uni = uni; n->pool = -1; n->npool = 0;
    if (nmulti > 1) {
        uint32_t *np = (uint32_t *)realloc(f->pool, sizeof(uint32_t) * (size_t)(f->npool + nmulti));
        if (!np) return;
        f->pool = np;
        memcpy(f->pool + f->npool, multi, sizeof(uint32_t) * (size_t)nmulti);
        n->pool = f->npool;
        n->npool = nmulti;
        f->npool += nmulti;
    }
}

/* UTF-16BE → codepoint তালিকা */
static int utf16_decode(const unsigned char *s, int n, uint32_t *out, int max)
{
    int i = 0, k = 0;
    while (i + 1 < n && k < max) {
        uint32_t c = (uint32_t)(s[i] << 8 | s[i + 1]);
        i += 2;
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n) {
            uint32_t c2 = (uint32_t)(s[i] << 8 | s[i + 1]);
            if (c2 >= 0xDC00 && c2 < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00); i += 2; }
        }
        out[k++] = c;
    }
    if (n == 1 && max > 0) out[k++] = s[0];
    return k;
}

static void parse_cmap(pdf_doc *d, pfont *f, pobj *st, int tounicode)
{
    long len;
    unsigned char *data;
    pdf_lexer lx;
    pobj *ops[8];
    int nops = 0, mode = 0;                            /* 1 cs, 2 bfchar, 3 bfrange, 4 cidchar, 5 cidrange */
    pobj *o;
    if (!st) return;
    if (st->type == P_NAME) return;                    /* পূর্বনির্ধারিত নাম (Identity-H ইত্যাদি) আলাদা দেখা হয় */
    data = pdf_stream_data(d, st, &len, 0, NULL);
    if (!data) return;
    lx.d = d;
    lx.p = data;
    lx.end = data + len;
    while ((o = pdf_lex_object(&lx)) != NULL) {
        if (o->type == P_KEYWORD) {
            const char *k = (const char *)o->u.str.s;
            if (!strcmp(k, "begincodespacerange")) mode = 1;
            else if (!strcmp(k, "beginbfchar")) mode = 2;
            else if (!strcmp(k, "beginbfrange")) mode = 3;
            else if (!strcmp(k, "begincidchar")) mode = 4;
            else if (!strcmp(k, "begincidrange")) mode = 5;
            else if (!strncmp(k, "end", 3)) mode = 0;
            else if (!strcmp(k, "usecmap")) { /* বাদ */ }
            nops = 0;
            continue;
        }
        if (nops < 8) ops[nops++] = o;
        if (mode == 1 && nops == 2) {
            if (ops[0]->type == P_STR && ops[1]->type == P_STR && !tounicode) {
                cs_rng *n = (cs_rng *)realloc(f->cs, sizeof(cs_rng) * (size_t)(f->ncs + 1));
                if (n) {
                    f->cs = n;
                    n[f->ncs].lo = bytes_be(ops[0]->u.str.s, ops[0]->u.str.len);
                    n[f->ncs].hi = bytes_be(ops[1]->u.str.s, ops[1]->u.str.len);
                    n[f->ncs].nbytes = ops[0]->u.str.len;
                    f->ncs++;
                }
            }
            nops = 0;
        } else if (mode == 2 && nops == 2) {
            if (ops[0]->type == P_STR && tounicode) {
                uint32_t u[8];
                int k = 0;
                if (ops[1]->type == P_STR) k = utf16_decode(ops[1]->u.str.s, ops[1]->u.str.len, u, 8);
                else if (ops[1]->type == P_NAME) { u[0] = name_to_uni((char *)ops[1]->u.str.s); k = u[0] ? 1 : 0; }
                if (k > 0) {
                    uint32_t c = bytes_be(ops[0]->u.str.s, ops[0]->u.str.len);
                    tu_add(f, c, c, u[0], u, k);
                }
            }
            nops = 0;
        } else if (mode == 3 && nops == 3) {
            if (ops[0]->type == P_STR && ops[1]->type == P_STR && tounicode) {
                uint32_t lo = bytes_be(ops[0]->u.str.s, ops[0]->u.str.len), hi = bytes_be(ops[1]->u.str.s, ops[1]->u.str.len);
                if (hi >= lo && hi - lo < 65536) {
                    if (ops[2]->type == P_STR) {
                        uint32_t u[8];
                        int k = utf16_decode(ops[2]->u.str.s, ops[2]->u.str.len, u, 8);
                        if (k == 1) tu_add(f, lo, hi, u[0], NULL, 0);
                        else if (k > 1) {               /* শেষ অক্ষর বাড়ে */
                            uint32_t c;
                            for (c = lo; c <= hi; c++) {
                                uint32_t v[8];
                                memcpy(v, u, sizeof(uint32_t) * (size_t)k);
                                v[k - 1] += c - lo;
                                tu_add(f, c, c, v[0], v, k);
                            }
                        }
                    } else if (ops[2]->type == P_ARRAY) {
                        int i;
                        for (i = 0; i < ops[2]->u.arr.n && lo + (uint32_t)i <= hi; i++) {
                            pobj *s = ops[2]->u.arr.v[i];
                            uint32_t u[8];
                            int k;
                            if (s->type != P_STR) continue;
                            k = utf16_decode(s->u.str.s, s->u.str.len, u, 8);
                            if (k) tu_add(f, lo + (uint32_t)i, lo + (uint32_t)i, u[0], u, k);
                        }
                    }
                }
            }
            nops = 0;
        } else if (mode == 4 && nops == 2) {
            if (ops[0]->type == P_STR && !tounicode) {
                cid_rng *n = (cid_rng *)realloc(f->cmap, sizeof(cid_rng) * (size_t)(f->ncmap + 1));
                if (n) {
                    f->cmap = n;
                    n[f->ncmap].lo = n[f->ncmap].hi = bytes_be(ops[0]->u.str.s, ops[0]->u.str.len);
                    n[f->ncmap].cid = (uint32_t)pdf_num(ops[1], 0);
                    f->ncmap++;
                }
            }
            nops = 0;
        } else if (mode == 5 && nops == 3) {
            if (ops[0]->type == P_STR && ops[1]->type == P_STR && !tounicode) {
                cid_rng *n = (cid_rng *)realloc(f->cmap, sizeof(cid_rng) * (size_t)(f->ncmap + 1));
                if (n) {
                    f->cmap = n;
                    n[f->ncmap].lo = bytes_be(ops[0]->u.str.s, ops[0]->u.str.len);
                    n[f->ncmap].hi = bytes_be(ops[1]->u.str.s, ops[1]->u.str.len);
                    n[f->ncmap].cid = (uint32_t)pdf_num(ops[2], 0);
                    f->ncmap++;
                }
            }
            nops = 0;
        } else if (mode == 0) {
            nops = 0;
        }
    }
}

/* code → Unicode (out-এ কয়টা) */
static int font_unicode(pfont *f, uint32_t code, uint32_t *out, int max)
{
    int i;
    for (i = f->ntu - 1; i >= 0; i--) {
        tu_ent *t = &f->tu[i];
        if (code >= t->lo && code <= t->hi) {
            if (t->npool > 1) {
                int k = t->npool < max ? t->npool : max;
                memcpy(out, f->pool + t->pool, sizeof(uint32_t) * (size_t)k);
                return k;
            }
            out[0] = t->uni + (code - t->lo);
            return 1;
        }
    }
    if (f->kind != 0 && code < 256 && f->uni[code]) { out[0] = f->uni[code]; return 1; }
    if (f->kind == 0 && f->face && f->substituted) { out[0] = code; return 1; }
    return 0;
}

/* --- বাইরের ফন্ট (embed নেই) --- */

static int has_word(const char *s, const char *w)
{
    size_t n = strlen(w);
    for (; *s; s++) {
        size_t i;
        for (i = 0; i < n && s[i]; i++) {
            char a = s[i], b = w[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (a != b) break;
        }
        if (i == n) return 1;
    }
    return 0;
}

static unsigned char *read_file(const char *path, long *len)
{
    FILE *fp = fopen(path, "rb");
    unsigned char *b;
    long n;
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    b = (unsigned char *)malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, fp) != (size_t)n) { free(b); b = NULL; }
    fclose(fp);
    *len = n;
    return b;
}

static void substitute(pfont *f, const char *name, int flags)
{
    static const char *const sans[] = { "/usr/share/fonts/NotoSans.ttf", "/usr/share/fonts/DejaVuSans.ttf", NULL };
    static const char *const serif[] = { "/usr/share/fonts/NotoSerif.ttf", "/usr/share/fonts/SourceSerif4.ttf",
                                         "/usr/share/fonts/DejaVuSans.ttf", NULL };
    static const char *const mono[] = { "/usr/share/fonts/JetBrainsMono-Regular.ttf", "/usr/share/fonts/DejaVuSansMono.ttf",
                                        "/usr/share/fonts/DejaVuSans.ttf", NULL };
    static const char *const monob[] = { "/usr/share/fonts/JetBrainsMono-Bold.ttf", "/usr/share/fonts/DejaVuSansMono.ttf", NULL };
    static const char *const bangla[] = { "/usr/share/fonts/NotoSansBengali.ttf", "/usr/share/fonts/DejaVuSans.ttf", NULL };
    const char *const *list = sans;
    int i, bold_file = 0;
    if (!name) name = "";
    f->bold = has_word(name, "bold") || has_word(name, "black") || has_word(name, "heavy") || has_word(name, "semibold");
    f->italic = has_word(name, "italic") || has_word(name, "oblique");
    if (has_word(name, "courier") || has_word(name, "mono") || has_word(name, "consol") || (flags & 1)) {
        list = f->bold ? monob : mono;
        bold_file = f->bold;
    } else if (has_word(name, "times") || has_word(name, "serif") || has_word(name, "roman") || has_word(name, "georgia") ||
               has_word(name, "garamond") || has_word(name, "cambria") || has_word(name, "book") || (flags & 2)) {
        list = serif;
    }
    if (has_word(name, "bangla") || has_word(name, "beng") || has_word(name, "vrinda") || has_word(name, "nikosh") ||
        has_word(name, "solaiman") || has_word(name, "kalpurush") || has_word(name, "siyam")) list = bangla;
    if (has_word(name, "sans") && list == serif) list = sans;
    for (i = 0; list[i]; i++) {
        long len;
        unsigned char *b = read_file(list[i], &len);
        if (!b) continue;
        if (FT_New_Memory_Face(ftlib, b, len, 0, &f->face) == 0) {
            f->fontdata = b;
            break;
        }
        free(b);
        f->face = NULL;
    }
    if (bold_file) f->bold = 0;                        /* আলাদা bold ফাইল পেলে কৃত্রিম নয় */
    f->substituted = 1;
    if (f->face) FT_Select_Charmap(f->face, FT_ENCODING_UNICODE);
}

static int load_embedded(pdf_doc *d, pfont *f, pobj *desc)
{
    static const char *const keys[3] = { "FontFile2", "FontFile3", "FontFile" };
    int i;
    for (i = 0; i < 3; i++) {
        pobj *st = pdf_get(d, desc, keys[i]);
        long len;
        unsigned char *data, *copy;
        if (!st || st->type != P_STREAM) continue;
        data = pdf_stream_data(d, st, &len, 0, NULL);
        if (!data || len < 4) continue;
        copy = (unsigned char *)malloc((size_t)len);
        if (!copy) return -1;
        memcpy(copy, data, (size_t)len);
        if (FT_New_Memory_Face(ftlib, copy, len, 0, &f->face) == 0) {
            f->fontdata = copy;
            f->is_sfnt = FT_IS_SFNT(f->face) ? 1 : 0;
            return 0;
        }
        free(copy);
        f->face = NULL;
        if (i == 2) {
#ifndef ROZPDF_TYPE1
            snprintf(warn, sizeof(warn), "a Type 1 font is shown with a replacement font");
#endif
        }
    }
    return -1;
}

static void simple_encoding(pdf_doc *d, pfont *f, pobj *enc, int std_base)
{
    int c;
    pobj *diff = NULL;
    const char *base = NULL;
    enc = pdf_resolve(d, enc);
    if (enc && enc->type == P_NAME) base = (const char *)enc->u.str.s;
    else if (enc && enc->type == P_DICT) {
        base = pdf_name(d, enc, "BaseEncoding");
        diff = pdf_get(d, enc, "Differences");
    }
    /* ভিত্তি: WinAnsi (StandardEncoding-ও ASCII অংশে প্রায় এক, শুধু ' আর `) */
    if (base || std_base) {
        for (c = 0; c < 256; c++) {
            const char *n = winansi_name(c);
            if (!base || !strcmp(base, "StandardEncoding")) {
                if (c == 0x27) n = "quoteright";
                else if (c == 0x60) n = "quoteleft";
                else if (c >= 0x80) n = NULL;
            }
            if (n) {
                f->names[c] = xstrdup(n);
                f->uni[c] = name_to_uni(n);
            }
        }
    }
    if (diff && diff->type == P_ARRAY) {
        int i, code = 0;
        for (i = 0; i < diff->u.arr.n; i++) {
            pobj *o = pdf_resolve(d, diff->u.arr.v[i]);
            if (!o) continue;
            if (o->type == P_INT) code = (int)o->u.i;
            else if (o->type == P_NAME && code >= 0 && code < 256) {
                free(f->names[code]);
                f->names[code] = xstrdup((char *)o->u.str.s);
                f->uni[code] = name_to_uni((char *)o->u.str.s);
                code++;
            }
        }
    }
}

static int simple_gid(pfont *f, int code)
{
    FT_Face face = f->face;
    int g = 0;
    if (!face) return 0;
    if (f->substituted) {
        uint32_t u = f->uni[code] ? f->uni[code] : (uint32_t)code;
        if (u == 0xA0) u = ' ';
        return (int)FT_Get_Char_Index(face, u);
    }
    if (!f->is_sfnt) {                                 /* Type1 / CFF: নাম দিয়ে */
        if (f->names[code]) g = (int)FT_Get_Name_Index(face, f->names[code]);
        if (!g) {
            int i;
            for (i = 0; i < face->num_charmaps && !g; i++) {
                FT_CharMap cm = face->charmaps[i];
                if (cm->encoding == FT_ENCODING_ADOBE_CUSTOM || cm->encoding == FT_ENCODING_ADOBE_STANDARD ||
                    cm->encoding == FT_ENCODING_ADOBE_EXPERT) {
                    FT_Set_Charmap(face, cm);
                    g = (int)FT_Get_Char_Index(face, (FT_ULong)code);
                }
            }
        }
        if (!g && f->uni[code]) {
            FT_Select_Charmap(face, FT_ENCODING_UNICODE);
            g = (int)FT_Get_Char_Index(face, f->uni[code]);
        }
        return g;
    }
    /* TrueType */
    {
        int i;
        FT_CharMap ms_sym = NULL, ms_uni = NULL, mac = NULL;
        for (i = 0; i < face->num_charmaps; i++) {
            FT_CharMap cm = face->charmaps[i];
            if (cm->platform_id == 3 && cm->encoding_id == 0) ms_sym = cm;
            else if (cm->platform_id == 3 && cm->encoding_id == 1) ms_uni = cm;
            else if (cm->platform_id == 1 && cm->encoding_id == 0) mac = cm;
        }
        if (!f->symbolic && ms_uni && f->uni[code]) {
            FT_Set_Charmap(face, ms_uni);
            g = (int)FT_Get_Char_Index(face, f->uni[code]);
        }
        if (!g && ms_sym) {
            FT_Set_Charmap(face, ms_sym);
            g = (int)FT_Get_Char_Index(face, (FT_ULong)code);
            if (!g) g = (int)FT_Get_Char_Index(face, 0xF000 + (FT_ULong)code);
            if (!g) g = (int)FT_Get_Char_Index(face, 0xF100 + (FT_ULong)code);
            if (!g) g = (int)FT_Get_Char_Index(face, 0xF200 + (FT_ULong)code);
        }
        if (!g && mac) {
            FT_Set_Charmap(face, mac);
            g = (int)FT_Get_Char_Index(face, (FT_ULong)code);
        }
        if (!g && ms_uni && f->uni[code]) {
            FT_Set_Charmap(face, ms_uni);
            g = (int)FT_Get_Char_Index(face, f->uni[code]);
        }
        if (!g && f->names[code] && FT_HAS_GLYPH_NAMES(face)) g = (int)FT_Get_Name_Index(face, f->names[code]);
        if (!g && !ms_sym && !mac && !ms_uni && code < face->num_glyphs) g = code;
    }
    return g;
}

static pfont *load_font(pdf_doc *d, pobj *fd)
{
    pfont *f;
    const char *sub, *bname;
    pobj *desc, *o;
    int i, flags;
    fd = pdf_resolve(d, fd);
    if (!fd || fd->type != P_DICT) return NULL;
    for (f = fonts; f; f = f->next) if (f->doc == d && f->dict == fd) return f;
    f = (pfont *)calloc(1, sizeof(pfont));
    if (!f) return NULL;
    f->doc = d;
    f->dict = fd;
    f->next = fonts;
    fonts = f;
    f->dw = 1000;
    sub = pdf_name(d, fd, "Subtype");
    bname = pdf_name(d, fd, "BaseFont");
    if (sub && !strcmp(sub, "Type3")) {
        f->kind = 3;
        f->fmatrix = mat_from_array(d, pdf_get(d, fd, "FontMatrix"), (mat){ 0.001, 0, 0, 0.001, 0, 0 });
        f->charprocs = pdf_get(d, fd, "CharProcs");
        f->resources = pdf_get(d, fd, "Resources");
        simple_encoding(d, f, pdf_get(d, fd, "Encoding"), 0);
        {
            pobj *w = pdf_get(d, fd, "Widths");
            int first = (int)pdf_int(d, fd, "FirstChar", 0);
            for (i = 0; i < 256; i++) f->widths[i] = -1;
            if (w && w->type == P_ARRAY) {
                f->has_widths = 1;
                for (i = 0; i < w->u.arr.n; i++)
                    if (first + i >= 0 && first + i < 256) f->widths[first + i] = pdf_num(pdf_resolve(d, w->u.arr.v[i]), 0);
            }
        }
    } else if (sub && !strcmp(sub, "Type0")) {
        pobj *desc_arr = pdf_get(d, fd, "DescendantFonts"), *cid = NULL, *enc;
        f->kind = 0;
        f->two_byte = 1;
        if (desc_arr && desc_arr->type == P_ARRAY && desc_arr->u.arr.n > 0) cid = pdf_resolve(d, desc_arr->u.arr.v[0]);
        enc = pdf_get(d, fd, "Encoding");
        if (enc && enc->type == P_NAME) {
            const char *en = (const char *)enc->u.str.s;
            f->identity = !strcmp(en, "Identity-H") || !strcmp(en, "Identity-V");
            f->vertical = en[strlen(en) - 1] == 'V' && en[strlen(en) - 2] == '-';
            if (!f->identity) f->identity = 1;         /* অন্য পূর্বনির্ধারিত CMap: সরলীকরণ */
        } else if (enc && enc->type == P_STREAM) {
            parse_cmap(d, f, enc, 0);
            if (!f->ncmap) f->identity = 1;
        }
        if (cid) {
            pobj *w = pdf_get(d, cid, "W"), *c2g;
            f->dw = pdf_real(d, cid, "DW", 1000);
            if (w && w->type == P_ARRAY) {
                for (i = 0; i < w->u.arr.n;) {
                    pobj *a = pdf_resolve(d, w->u.arr.v[i]), *b = i + 1 < w->u.arr.n ? pdf_resolve(d, w->u.arr.v[i + 1]) : NULL;
                    if (!a || !b) break;
                    if (b->type == P_ARRAY) {
                        int k;
                        uint32_t c0 = (uint32_t)pdf_num(a, 0);
                        for (k = 0; k < b->u.arr.n; k++) {
                            w_rng *n = (w_rng *)realloc(f->wr, sizeof(w_rng) * (size_t)(f->nwr + 1));
                            if (!n) break;
                            f->wr = n;
                            n[f->nwr].lo = n[f->nwr].hi = c0 + (uint32_t)k;
                            n[f->nwr].w = pdf_num(pdf_resolve(d, b->u.arr.v[k]), f->dw);
                            f->nwr++;
                        }
                        i += 2;
                    } else if (i + 2 < w->u.arr.n) {
                        w_rng *n = (w_rng *)realloc(f->wr, sizeof(w_rng) * (size_t)(f->nwr + 1));
                        if (n) {
                            f->wr = n;
                            n[f->nwr].lo = (uint32_t)pdf_num(a, 0);
                            n[f->nwr].hi = (uint32_t)pdf_num(b, 0);
                            n[f->nwr].w = pdf_num(pdf_resolve(d, w->u.arr.v[i + 2]), f->dw);
                            f->nwr++;
                        }
                        i += 3;
                    } else {
                        break;
                    }
                }
            }
            c2g = pdf_get(d, cid, "CIDToGIDMap");
            if (c2g && c2g->type == P_STREAM) {
                long len;
                unsigned char *m = pdf_stream_data(d, c2g, &len, 0, NULL);
                if (m && len >= 2) {
                    f->cid2gid = (uint16_t *)malloc(sizeof(uint16_t) * (size_t)(len / 2));
                    if (f->cid2gid) {
                        long k;
                        for (k = 0; k < len / 2; k++) f->cid2gid[k] = (uint16_t)(m[2 * k] << 8 | m[2 * k + 1]);
                        f->ncid2gid = len / 2;
                    }
                }
            }
            desc = pdf_get(d, cid, "FontDescriptor");
            flags = (int)pdf_int(d, desc, "Flags", 0);
            if (ft_ok > 0 && load_embedded(d, f, desc) < 0) substitute(f, bname, flags);
        }
    } else {
        f->kind = 1;
        desc = pdf_get(d, fd, "FontDescriptor");
        flags = (int)pdf_int(d, desc, "Flags", 0);
        f->symbolic = (flags & 4) != 0;
        f->missing = pdf_real(d, desc, "MissingWidth", 0);
        if (ft_ok > 0 && load_embedded(d, f, desc) < 0) substitute(f, bname, flags);
        o = pdf_get(d, fd, "Encoding");
        simple_encoding(d, f, o, !o ? (f->substituted || (f->face && f->is_sfnt && !f->symbolic)) : 0);
        if (!o && f->substituted && bname && (has_word(bname, "symbol") || has_word(bname, "dingbat"))) {
            for (i = 0; i < 256; i++) f->uni[i] = 0;   /* প্রতীক ফন্ট: ভুল অক্ষর না দেখিয়ে */
        }
        {
            pobj *w = pdf_get(d, fd, "Widths");
            int first = (int)pdf_int(d, fd, "FirstChar", 0);
            for (i = 0; i < 256; i++) f->widths[i] = -1;
            if (w && w->type == P_ARRAY) {
                f->has_widths = 1;
                for (i = 0; i < w->u.arr.n; i++)
                    if (first + i >= 0 && first + i < 256) f->widths[first + i] = pdf_num(pdf_resolve(d, w->u.arr.v[i]), 0);
            }
        }
        for (i = 0; i < 256; i++) f->gid[i] = simple_gid(f, i);
    }
    {
        pobj *tu = pdf_get(d, fd, "ToUnicode");
        if (tu && tu->type == P_STREAM) parse_cmap(d, f, tu, 1);
    }
    if (f->face && FT_IS_SCALABLE(f->face)) FT_Set_Char_Size(f->face, 0, 64 * 64, 72, 72);   /* em = ৬৪ px */
    return f;
}

/* code → width (১০০০ ভাগে; Type3-এ glyph space) */
static double font_width(pfont *f, uint32_t code, uint32_t cid, int gid)
{
    int i;
    if (f->kind == 0) {
        for (i = 0; i < f->nwr; i++) if (cid >= f->wr[i].lo && cid <= f->wr[i].hi) return f->wr[i].w;
        return f->dw;
    }
    if (code < 256 && f->widths[code] >= 0 && f->has_widths) return f->widths[code];
    if (f->kind == 3) return 0;
    if (f->face && gid > 0 && f->face->units_per_EM) {
        FT_Fixed adv;
        if (FT_Get_Advance(f->face, (FT_UInt)gid, FT_LOAD_NO_SCALE, &adv) == 0) {
            double w = adv * 1000.0 / f->face->units_per_EM;
            if (code < 256 && f->kind == 1) { f->widths[code] = w; f->has_widths = 1; }   /* পরের বারের জন্য */
            return w;
        }
    }
    return f->missing > 0 ? f->missing : 500;
}

/* পরের code (কয় বাইট) */
static int next_code(pfont *f, const unsigned char *s, int n, uint32_t *code)
{
    int i;
    if (f->kind != 0) { *code = s[0]; return 1; }
    if (f->ncs) {
        int len;
        for (len = 1; len <= 4 && len <= n; len++) {
            uint32_t v = bytes_be(s, len);
            for (i = 0; i < f->ncs; i++) if (f->cs[i].nbytes == len && v >= f->cs[i].lo && v <= f->cs[i].hi) { *code = v; return len; }
        }
    }
    if (n >= 2) { *code = (uint32_t)(s[0] << 8 | s[1]); return 2; }
    *code = s[0];
    return 1;
}

static uint32_t code_to_cid(pfont *f, uint32_t code)
{
    int i;
    for (i = 0; i < f->ncmap; i++) if (code >= f->cmap[i].lo && code <= f->cmap[i].hi) return f->cmap[i].cid + (code - f->cmap[i].lo);
    return code;
}

static int cid_to_gid(pfont *f, uint32_t cid, uint32_t code)
{
    if (!f->face) return 0;
    if (f->substituted) {                              /* বাইরের ফন্ট: Unicode দিয়ে */
        uint32_t u[4];
        if (font_unicode(f, code, u, 4) > 0) return (int)FT_Get_Char_Index(f->face, u[0]);
        return 0;
    }
    if (f->cid2gid) return cid < (uint32_t)f->ncid2gid ? f->cid2gid[cid] : 0;
    return (int)cid;                                   /* Identity (CFF-এ FreeType নিজে CID ধরে) */
}

/* ---------- আঁকার অবস্থা ---------- */

typedef struct gstate {
    mat ctm;
    uint32_t fill, stroke;            /* RGB (alpha আলাদা) */
    double fa, sa;                    /* ca, CA */
    int fill_n, stroke_n;             /* color space-এর component (০ = pattern — আঁকা বাদ) */
    pobj *fill_idx, *stroke_idx;      /* Indexed হলে [/Indexed base hival lookup] */
    double lw;
    int cap, join;
    double dash[8]; int ndash; double dash_phase;
    double cx0, cy0, cx1, cy1;        /* clip (device) */
    /* লেখা */
    pfont *font;
    double fs, tc, tw, th, tl, ts;
    int tr;
} gstate;

typedef struct rctx {
    pdf_doc *d;
    gfx_surface *s;
    const pdf_render_opts *o;
    gstate gs[32];
    int gsp;
    /* path */
    double *pts;                      /* x,y জোড়া */
    int npts, cap;
    int *sub;                         /* subpath-এর শুরু (index), শেষ -1 থাকলে খোলা */
    int nsub, subcap;
    unsigned char *closed;
    double px, py, sx, sy;            /* বর্তমান বিন্দু, subpath শুরু (user space) */
    int pending_clip;                 /* 1 = W, 2 = W* */
    /* লেখা */
    mat tm, tlm;
    /* ফল */
    pdf_textitem *text;
    int ntext, textcap;
    int depth;
    long ops;
    pobj *res;                        /* এখনকার resource (Type3 ফন্টের নিজের না থাকলে) */
    /* ActualText (/Span ... BDC … EMC): বাংলার মতো লেখায় glyph নয়, আসল লেখা */
    int mc_depth, at_depth;
    uint32_t at_text[128];
    int at_n, at_has;
    double at_box[4];
} rctx;

#define GS (&r->gs[r->gsp])

/* ---------- rasterizer ---------- */

typedef struct edge { double x0, y0, x1, y1; int dir; } edge;

static float *covbuf;
static int covw;

static void blend_px(uint32_t *p, uint32_t c, double a)
{
    uint32_t d = *p;
    int ia = (int)(a * 256);
    int r, g, b;
    if (ia <= 0) return;
    if (ia >= 256) { *p = 0xFF000000u | (c & 0xFFFFFF); return; }
    r = (int)GFX_R(d) + ((((int)GFX_R(c) - (int)GFX_R(d)) * ia) >> 8);
    g = (int)GFX_G(d) + ((((int)GFX_G(c) - (int)GFX_G(d)) * ia) >> 8);
    b = (int)GFX_B(d) + ((((int)GFX_B(c) - (int)GFX_B(d)) * ia) >> 8);
    *p = GFX_RGB(r, g, b);
}

static int cmp_edge(const void *a, const void *b)
{
    double ya = ((const edge *)a)->y0, yb = ((const edge *)b)->y0;
    return ya < yb ? -1 : ya > yb;
}

static int cmp_xi(const void *a, const void *b)
{
    double xa = ((const double *)a)[0], xb = ((const double *)b)[0];
    return xa < xb ? -1 : xa > xb;
}

#define SUBS 4

static void fill_edges(rctx *r, edge *e, int ne, int evenodd, uint32_t color, double alpha)
{
    gstate *g = GS;
    int y0, y1, y, i, k, nact = 0, next = 0, ix0 = 1 << 30, ix1 = -1;
    edge **act;
    double *xs;
    double cx0 = g->cx0 > 0 ? g->cx0 : 0, cy0 = g->cy0 > 0 ? g->cy0 : 0;
    double cx1 = g->cx1 < r->s->w ? g->cx1 : r->s->w, cy1 = g->cy1 < r->s->h ? g->cy1 : r->s->h;
    double ymin = 1e30, ymax = -1e30;
    if (ne == 0 || alpha <= 0 || cx1 <= cx0 || cy1 <= cy0) return;
    for (i = 0; i < ne; i++) {
        if (e[i].y0 < ymin) ymin = e[i].y0;
        if (e[i].y1 > ymax) ymax = e[i].y1;
    }
    if (ymin < cy0) ymin = cy0;
    if (ymax > cy1) ymax = cy1;
    if (ymax <= ymin) return;
    y0 = (int)floor(ymin);
    y1 = (int)ceil(ymax);
    qsort(e, (size_t)ne, sizeof(edge), cmp_edge);
    act = (edge **)malloc(sizeof(edge *) * (size_t)ne);
    xs = (double *)malloc(sizeof(double) * 2 * (size_t)ne);
    if (!act || !xs) { free(act); free(xs); return; }
    if (covw < r->s->w + 2) {
        free(covbuf);
        covw = r->s->w + 2;
        covbuf = (float *)calloc((size_t)covw, sizeof(float));
        if (!covbuf) { covw = 0; free(act); free(xs); return; }
    }
    for (y = y0; y < y1; y++) {
        int any = 0;
        ix0 = 1 << 30;
        ix1 = -1;
        for (k = 0; k < SUBS; k++) {
            double sy = y + (k + 0.5) / SUBS;
            int n = 0, wind = 0, j;
            if (sy < cy0 || sy >= cy1) continue;
            while (next < ne && e[next].y0 <= sy) act[nact++] = &e[next++];
            for (j = 0; j < nact;) {                    /* শেষ হয়ে যাওয়া edge সরানো */
                if (act[j]->y1 <= sy) act[j] = act[--nact];
                else j++;
            }
            for (j = 0; j < nact; j++) {
                edge *ed = act[j];
                if (sy < ed->y0) continue;
                xs[2 * n] = ed->x0 + (sy - ed->y0) * (ed->x1 - ed->x0) / (ed->y1 - ed->y0);
                xs[2 * n + 1] = ed->dir;
                n++;
            }
            if (n < 2) continue;
            qsort(xs, (size_t)n, sizeof(double) * 2, cmp_xi);
            for (j = 0; j < n - 1; j++) {
                double xa, xb;
                int inside;
                wind += (int)xs[2 * j + 1];
                inside = evenodd ? (wind & 1) : (wind != 0);
                if (!inside) continue;
                xa = xs[2 * j];
                xb = xs[2 * j + 2];
                if (xa < cx0) xa = cx0;
                if (xb > cx1) xb = cx1;
                if (xb <= xa) continue;
                {
                    int ia = (int)xa, ib = (int)xb;
                    float w = 1.0f / SUBS;
                    if (ia == ib) {
                        covbuf[ia] += (float)(xb - xa) * w;
                    } else {
                        int x;
                        covbuf[ia] += (float)(ia + 1 - xa) * w;
                        for (x = ia + 1; x < ib; x++) covbuf[x] += w;
                        if (ib < r->s->w) covbuf[ib] += (float)(xb - ib) * w;
                    }
                    if (ia < ix0) ix0 = ia;
                    if (ib > ix1) ix1 = ib;
                    any = 1;
                }
            }
        }
        if (any && y >= 0 && y < r->s->h) {
            uint32_t *row = r->s->px + (long)y * r->s->stride;
            int x;
            if (ix1 >= r->s->w) ix1 = r->s->w - 1;
            for (x = ix0 < 0 ? 0 : ix0; x <= ix1; x++) {
                float c = covbuf[x];
                if (c > 0.001f) blend_px(&row[x], color, (c > 1 ? 1 : c) * alpha);
                covbuf[x] = 0;
            }
        } else if (any) {
            int x;
            for (x = ix0 < 0 ? 0 : ix0; x <= ix1 && x < covw; x++) covbuf[x] = 0;
        }
    }
    free(act);
    free(xs);
}

/* edge তালিকা জমানো */
typedef struct elist { edge *e; int n, cap; } elist;

static void add_edge(elist *l, double x0, double y0, double x1, double y1)
{
    edge *e;
    int dir = 1;
    if (y0 == y1) return;
    if (y0 > y1) { double t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; dir = -1; }
    if (l->n == l->cap) {
        edge *n;
        l->cap = l->cap ? l->cap * 2 : 256;
        n = (edge *)realloc(l->e, sizeof(edge) * (size_t)l->cap);
        if (!n) { l->cap = l->n; return; }
        l->e = n;
    }
    e = &l->e[l->n++];
    e->x0 = x0; e->y0 = y0; e->x1 = x1; e->y1 = y1; e->dir = dir;
}

static void add_poly(elist *l, const double *p, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        int j = (i + 1) % n;
        add_edge(l, p[2 * i], p[2 * i + 1], p[2 * j], p[2 * j + 1]);
    }
}

static void add_circle(elist *l, double cx, double cy, double rad)
{
    double p[32];
    int i, n = rad < 3 ? 8 : 16;
    for (i = 0; i < n; i++) {
        double a = i * 2 * 3.14159265358979 / n;
        p[2 * i] = cx + rad * cos(a);
        p[2 * i + 1] = cy - rad * sin(a);              /* stroke_seg-এর চতুর্ভুজের একই দিকে ঘোরা (nonzero-তে ফাঁক নয়) */
    }
    add_poly(l, p, n);
}

/* ---------- path ---------- */

static void path_reset(rctx *r)
{
    r->npts = 0;
    r->nsub = 0;
}

static void path_pt(rctx *r, double x, double y)           /* user space → device, জমা */
{
    double dx, dy;
    mat_apply(&GS->ctm, x, y, &dx, &dy);
    if (r->npts == r->cap) {
        int nc = r->cap ? r->cap * 2 : 1024;
        double *n = (double *)realloc(r->pts, sizeof(double) * 2 * (size_t)nc);
        if (!n) return;
        r->pts = n;
        r->cap = nc;
    }
    r->pts[2 * r->npts] = dx;
    r->pts[2 * r->npts + 1] = dy;
    r->npts++;
}

static void path_move(rctx *r, double x, double y)
{
    if (r->nsub == r->subcap) {
        int nc = r->subcap ? r->subcap * 2 : 64;
        int *n = (int *)realloc(r->sub, sizeof(int) * (size_t)nc);
        unsigned char *c = (unsigned char *)realloc(r->closed, (size_t)nc);
        if (!n || !c) { if (n) r->sub = n; if (c) r->closed = c; return; }
        r->sub = n;
        r->closed = c;
        r->subcap = nc;
    }
    if (r->nsub > 0 && r->sub[r->nsub - 1] == r->npts - 1 && r->npts > 0) {
        r->npts--;                                     /* একা moveto — বাদ */
        r->nsub--;
    }
    r->sub[r->nsub] = r->npts;
    r->closed[r->nsub] = 0;
    r->nsub++;
    path_pt(r, x, y);
    r->px = r->sx = x;
    r->py = r->sy = y;
}

static void path_line(rctx *r, double x, double y)
{
    if (r->nsub == 0) path_move(r, r->px, r->py);
    path_pt(r, x, y);
    r->px = x;
    r->py = y;
}

static void path_curve(rctx *r, double x1, double y1, double x2, double y2, double x3, double y3)
{
    double ax, ay, bx, by, cx, cy, dx, dy, len;
    int n, i;
    if (r->nsub == 0) path_move(r, r->px, r->py);
    mat_apply(&GS->ctm, r->px, r->py, &ax, &ay);
    mat_apply(&GS->ctm, x1, y1, &bx, &by);
    mat_apply(&GS->ctm, x2, y2, &cx, &cy);
    mat_apply(&GS->ctm, x3, y3, &dx, &dy);
    len = hypot(bx - ax, by - ay) + hypot(cx - bx, cy - by) + hypot(dx - cx, dy - cy);
    n = (int)(len / 3);
    if (n < 2) n = 2;
    if (n > 100) n = 100;
    for (i = 1; i <= n; i++) {
        double t = (double)i / n, u = 1 - t;
        double x = u * u * u * r->px + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3;
        double y = u * u * u * r->py + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3;
        path_pt(r, x, y);
    }
    r->px = x3;
    r->py = y3;
}

static void path_close(rctx *r)
{
    if (r->nsub > 0) {
        r->closed[r->nsub - 1] = 1;
        r->px = r->sx;
        r->py = r->sy;
    }
}

static int sub_end(rctx *r, int s) { return s + 1 < r->nsub ? r->sub[s + 1] : r->npts; }

static int paint_ok(rctx *r, int fill)
{
    gstate *g = GS;
    return !r->o->no_draw && (fill ? g->fill_n : g->stroke_n) > 0;
}

static void do_fill(rctx *r, int evenodd)
{
    elist l = { 0 };
    int s;
    if (!paint_ok(r, 1)) return;
    for (s = 0; s < r->nsub; s++) {
        int a = r->sub[s], b = sub_end(r, s), i;
        if (b - a < 2) continue;
        for (i = a; i < b; i++) {
            int j = i + 1 < b ? i + 1 : a;
            add_edge(&l, r->pts[2 * i], r->pts[2 * i + 1], r->pts[2 * j], r->pts[2 * j + 1]);
        }
    }
    fill_edges(r, l.e, l.n, evenodd, GS->fill, GS->fa);
    free(l.e);
}

static double dev_lw(rctx *r)
{
    gstate *g = GS;
    double sc = sqrt(fabs(g->ctm.a * g->ctm.d - g->ctm.b * g->ctm.c)), w = g->lw * sc;
    return w < 1.0 ? 1.0 : w;
}

static void stroke_seg(elist *l, double x0, double y0, double x1, double y1, double hw)
{
    double dx = x1 - x0, dy = y1 - y0, len = hypot(dx, dy), nx, ny, p[8];
    if (len < 1e-9) return;
    nx = -dy / len * hw;
    ny = dx / len * hw;
    p[0] = x0 + nx; p[1] = y0 + ny; p[2] = x1 + nx; p[3] = y1 + ny;
    p[4] = x1 - nx; p[5] = y1 - ny; p[6] = x0 - nx; p[7] = y0 - ny;
    if ((dx * ny - dy * nx) < 0) {                     /* দিক ঠিক রাখা (nonzero-তে একই ঘোরা) */
        double t;
        t = p[2]; p[2] = p[6]; p[6] = t; t = p[3]; p[3] = p[7]; p[7] = t;
    }
    add_poly(l, p, 4);
}

static void do_stroke(rctx *r)
{
    elist l = { 0 };
    gstate *g = GS;
    double hw = dev_lw(r) / 2, sc = sqrt(fabs(g->ctm.a * g->ctm.d - g->ctm.b * g->ctm.c));
    int s;
    if (!paint_ok(r, 0)) return;
    for (s = 0; s < r->nsub; s++) {
        int a = r->sub[s], b = sub_end(r, s), i, last = r->closed[s] ? b : b - 1;
        double dpos = 0, dlen = 0;
        int di = 0, on = 1;
        if (b - a < 1) continue;
        if (g->ndash > 0) {                            /* dash-এর শুরুর অবস্থা */
            double ph = g->dash_phase * sc;
            dlen = g->dash[0] * sc;
            while (ph > 0 && dlen > 0) {
                if (ph < dlen) { dlen -= ph; break; }
                ph -= dlen;
                di = (di + 1) % g->ndash;
                on = !on;
                dlen = g->dash[di] * sc;
            }
            dpos = 0;
        }
        for (i = a; i < last; i++) {
            int j = i + 1 < b ? i + 1 : a;
            double x0 = r->pts[2 * i], y0 = r->pts[2 * i + 1], x1 = r->pts[2 * j], y1 = r->pts[2 * j + 1];
            if (g->ndash > 0 && dlen > 0.05) {
                double seg = hypot(x1 - x0, y1 - y0), t = 0;
                while (t < seg) {
                    double take = dlen - dpos, t2;
                    if (take > seg - t) take = seg - t;
                    t2 = t + take;
                    if (on) stroke_seg(&l, x0 + (x1 - x0) * t / seg, y0 + (y1 - y0) * t / seg,
                                       x0 + (x1 - x0) * t2 / seg, y0 + (y1 - y0) * t2 / seg, hw);
                    dpos += take;
                    t = t2;
                    if (dpos >= dlen - 1e-9) {
                        dpos = 0;
                        di = (di + 1) % g->ndash;
                        on = !on;
                        dlen = g->dash[di] * sc;
                        if (dlen < 0.05) dlen = 0.05;
                    }
                }
            } else {
                stroke_seg(&l, x0, y0, x1, y1, hw);
            }
            if (hw > 1.2 && (g->join != 2 || g->ndash) && (j != a || r->closed[s]) && i + 1 < last + (r->closed[s] ? 1 : 0))
                add_circle(&l, x1, y1, hw);            /* জোড় মসৃণ (round/miter-এর কাছাকাছি) */
        }
        if (hw > 1.2 && !r->closed[s] && g->cap == 1 && b - a >= 1) {   /* round cap */
            add_circle(&l, r->pts[2 * a], r->pts[2 * a + 1], hw);
            add_circle(&l, r->pts[2 * (b - 1)], r->pts[2 * (b - 1) + 1], hw);
        }
        if (b - a == 1 && g->cap == 1) add_circle(&l, r->pts[2 * a], r->pts[2 * a + 1], hw);   /* বিন্দু */
    }
    fill_edges(r, l.e, l.n, 0, g->stroke, g->sa);
    free(l.e);
}

static void apply_clip(rctx *r)
{
    gstate *g = GS;
    double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
    int i;
    if (!r->pending_clip) return;
    r->pending_clip = 0;
    if (r->npts == 0) return;
    for (i = 0; i < r->npts; i++) {
        double x = r->pts[2 * i], y = r->pts[2 * i + 1];
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
    }
    if (x0 > g->cx0) g->cx0 = x0;
    if (y0 > g->cy0) g->cy0 = y0;
    if (x1 < g->cx1) g->cx1 = x1;
    if (y1 < g->cy1) g->cy1 = y1;
}

/* ---------- রং ---------- */

static uint32_t cmyk_rgb(double c, double m, double y, double k)
{
    double r = (1 - c) * (1 - k), g = (1 - m) * (1 - k), b = (1 - y) * (1 - k);
    return GFX_RGB((int)(r * 255 + 0.5), (int)(g * 255 + 0.5), (int)(b * 255 + 0.5));
}

static double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

/* color space → component সংখ্যা (০ = pattern/অচেনা); Indexed হলে *idx */
static int cs_ncomp(rctx *r, pobj *cs, pobj **idx, int depth)
{
    pdf_doc *d = r->d;
    cs = pdf_resolve(d, cs);
    if (idx) *idx = NULL;
    if (!cs || depth > 4) return 0;
    if (cs->type == P_NAME) {
        const char *n = (const char *)cs->u.str.s;
        if (!strcmp(n, "DeviceGray") || !strcmp(n, "G") || !strcmp(n, "CalGray")) return 1;
        if (!strcmp(n, "DeviceRGB") || !strcmp(n, "RGB") || !strcmp(n, "CalRGB")) return 3;
        if (!strcmp(n, "DeviceCMYK") || !strcmp(n, "CMYK")) return 4;
        if (!strcmp(n, "Pattern")) return 0;
        return -1;                                     /* resource-এ খুঁজতে হবে */
    }
    if (cs->type == P_ARRAY && cs->u.arr.n >= 1) {
        pobj *k = pdf_resolve(d, cs->u.arr.v[0]);
        const char *n = k && k->type == P_NAME ? (const char *)k->u.str.s : "";
        if (!strcmp(n, "ICCBased") && cs->u.arr.n >= 2) {
            pobj *st = pdf_resolve(d, cs->u.arr.v[1]);
            long nn = pdf_int(d, st, "N", 3);
            return nn == 1 || nn == 3 || nn == 4 ? (int)nn : 3;
        }
        if (!strcmp(n, "Indexed") || !strcmp(n, "I")) { if (idx) *idx = cs; return 1; }
        if (!strcmp(n, "Separation")) return 5;        /* tint → ধূসর */
        if (!strcmp(n, "DeviceN")) return 6;
        if (!strcmp(n, "CalRGB") || !strcmp(n, "Lab")) return 3;
        if (!strcmp(n, "CalGray")) return 1;
        if (!strcmp(n, "Pattern")) return 0;
    }
    return 3;
}

static int resolve_cs(rctx *r, pobj *res, pobj *name, pobj **idx)
{
    int n = cs_ncomp(r, name, idx, 0);
    if (n == -1) {
        pobj *csd = pdf_get(r->d, res, "ColorSpace");
        n = cs_ncomp(r, pdf_get(r->d, csd, (const char *)name->u.str.s), idx, 1);
        if (n == -1) n = 3;
    }
    return n;
}

/* Indexed-এর মূল color space আর lookup */
static uint32_t indexed_color(rctx *r, pobj *idx, int v)
{
    pdf_doc *d = r->d;
    pobj *base, *lk;
    int bn, hival;
    const unsigned char *tab = NULL;
    long tlen = 0;
    if (!idx || idx->u.arr.n < 4) return GFX_RGB(0, 0, 0);
    base = idx->u.arr.v[1];
    bn = cs_ncomp(r, base, NULL, 1);
    if (bn < 1 || bn > 4) bn = 3;
    hival = (int)pdf_num(pdf_resolve(d, idx->u.arr.v[2]), 255);
    lk = pdf_resolve(d, idx->u.arr.v[3]);
    if (lk && lk->type == P_STR) { tab = lk->u.str.s; tlen = lk->u.str.len; }
    else if (lk && lk->type == P_STREAM) tab = pdf_stream_data(d, lk, &tlen, 0, NULL);
    if (v > hival) v = hival;
    if (!tab || (long)(v + 1) * bn > tlen) return GFX_RGB(0, 0, 0);
    tab += v * bn;
    if (bn == 1) return GFX_RGB(tab[0], tab[0], tab[0]);
    if (bn == 4) return cmyk_rgb(tab[0] / 255.0, tab[1] / 255.0, tab[2] / 255.0, tab[3] / 255.0);
    return GFX_RGB(tab[0], tab[1], tab[2]);
}

static uint32_t comps_rgb(rctx *r, int n, pobj *idx, const double *v, int nv)
{
    if (n == 1 && idx) return indexed_color(r, idx, (int)v[0]);
    if (n == 1 || nv == 1) { int g = (int)(clamp01(v[0]) * 255 + .5); return GFX_RGB(g, g, g); }
    if (n == 5 || n == 6) { int g = (int)((1 - clamp01(v[0])) * 255 + .5); return GFX_RGB(g, g, g); }
    if (n == 4 && nv >= 4) return cmyk_rgb(clamp01(v[0]), clamp01(v[1]), clamp01(v[2]), clamp01(v[3]));
    if (nv >= 3) return GFX_RGB((int)(clamp01(v[0]) * 255 + .5), (int)(clamp01(v[1]) * 255 + .5), (int)(clamp01(v[2]) * 255 + .5));
    return 0;
}

/* ---------- লেখা ---------- */

static void add_text(rctx *r, uint32_t ch, double x0, double y0, double x1, double y1)
{
    pdf_textitem *t;
    if (!r->o->want_text) return;
    if (r->ntext == r->textcap) {
        int nc = r->textcap ? r->textcap * 2 : 1024;
        pdf_textitem *n = (pdf_textitem *)realloc(r->text, sizeof(pdf_textitem) * (size_t)nc);
        if (!n) return;
        r->text = n;
        r->textcap = nc;
    }
    t = &r->text[r->ntext++];
    t->ch = ch;
    t->x0 = (float)(x0 < x1 ? x0 : x1);
    t->x1 = (float)(x0 < x1 ? x1 : x0);
    t->y0 = (float)(y0 < y1 ? y0 : y1);
    t->y1 = (float)(y0 < y1 ? y1 : y0);
}

static void draw_glyph(rctx *r, pfont *f, int gid, const mat *trm, uint32_t color, double alpha)
{
    FT_Face face = f->face;
    FT_Matrix m;
    FT_Vector delta;
    FT_GlyphSlot sl;
    double ox, oy, scale;
    int x, y;
    if (!face || r->o->no_draw) return;
    scale = sqrt(fabs(trm->a * trm->d - trm->b * trm->c));
    if (scale < 0.3 || scale > 3000) return;
    if (f->italic && f->substituted) {                 /* কৃত্রিম হেলানো */
        mat sk = { 1, 0, 0.2, 1, 0, 0 }, t2 = mat_mul(sk, *trm);
        m.xx = (FT_Fixed)(t2.a / 64 * 65536);
        m.xy = (FT_Fixed)(t2.c / 64 * 65536);
        m.yx = (FT_Fixed)(-t2.b / 64 * 65536);
        m.yy = (FT_Fixed)(-t2.d / 64 * 65536);
    } else {
        m.xx = (FT_Fixed)(trm->a / 64 * 65536);
        m.xy = (FT_Fixed)(trm->c / 64 * 65536);
        m.yx = (FT_Fixed)(-trm->b / 64 * 65536);
        m.yy = (FT_Fixed)(-trm->d / 64 * 65536);
    }
    ox = trm->e;
    oy = trm->f;
    delta.x = (FT_Pos)((ox - floor(ox)) * 64);
    delta.y = (FT_Pos)(-(oy - floor(oy)) * 64);
    FT_Set_Transform(face, &m, &delta);
    if (FT_Load_Glyph(face, (FT_UInt)gid, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) != 0) return;
    sl = face->glyph;
    if (f->bold && f->substituted && sl->format == FT_GLYPH_FORMAT_OUTLINE) FT_GlyphSlot_Embolden(sl);
    if (FT_Render_Glyph(sl, FT_RENDER_MODE_NORMAL) != 0) return;
    {
        FT_Bitmap *bm = &sl->bitmap;
        int bx = (int)floor(ox) + sl->bitmap_left, by = (int)floor(oy) - sl->bitmap_top;
        gstate *g = GS;
        int cx0 = (int)ceil(g->cx0 > 0 ? g->cx0 : 0), cy0 = (int)ceil(g->cy0 > 0 ? g->cy0 : 0);
        int cx1 = (int)(g->cx1 < r->s->w ? g->cx1 : r->s->w), cy1 = (int)(g->cy1 < r->s->h ? g->cy1 : r->s->h);
        if (bm->pixel_mode != FT_PIXEL_MODE_GRAY) return;
        for (y = 0; y < (int)bm->rows; y++) {
            int dy = by + y;
            const unsigned char *src = bm->buffer + y * bm->pitch;
            uint32_t *row;
            if (dy < cy0 || dy >= cy1) continue;
            row = r->s->px + (long)dy * r->s->stride;
            for (x = 0; x < (int)bm->width; x++) {
                int dx = bx + x;
                if (src[x] && dx >= cx0 && dx < cx1) blend_px(&row[dx], color, src[x] / 255.0 * alpha);
            }
        }
    }
}

static void run_content(rctx *r, const unsigned char *data, long len, pobj *res);

static void show_string(rctx *r, const unsigned char *s, int n)
{
    gstate *g = GS;
    pfont *f = g->font;
    int i = 0;
    if (!f) return;
    while (i < n) {
        uint32_t code, cid = 0, u[8];
        int nb = next_code(f, s + i, n - i, &code), gid = 0, nu, k;
        double w0, tx, fs = g->fs, th = g->th / 100.0;
        mat trm, base;
        i += nb;
        if (f->kind == 0) {
            cid = f->identity && !f->ncmap ? code : code_to_cid(f, code);
            gid = cid_to_gid(f, cid, code);
        } else if (f->kind == 1) {
            gid = f->gid[code & 255];
        }
        w0 = font_width(f, code, cid, gid);
        /* Trm = [fs·th 0 0 fs 0 rise] × Tm × CTM */
        base.a = fs * th; base.b = 0; base.c = 0; base.d = fs; base.e = 0; base.f = g->ts;
        trm = mat_mul(mat_mul(base, r->tm), g->ctm);
        if (f->kind == 3) {
            mat full = mat_mul(f->fmatrix, trm);
            pobj *proc = (code < 256 && f->names[code]) ? pdf_get(r->d, f->charprocs, f->names[code]) : NULL;
            if (proc && proc->type == P_STREAM && r->depth < 6 && g->tr != 3 && g->tr != 7) {
                long plen;
                unsigned char *pd = pdf_stream_data(r->d, proc, &plen, 0, NULL);
                if (pd && r->gsp < 31) {
                    r->gsp++;
                    r->gs[r->gsp] = r->gs[r->gsp - 1];
                    GS->ctm = full;
                    {
                        mat save_tm = r->tm, save_tlm = r->tlm;
                        r->depth++;
                        run_content(r, pd, plen, f->resources ? f->resources : r->res);
                        r->depth--;
                        r->tm = save_tm;
                        r->tlm = save_tlm;
                    }
                    r->gsp--;
                    g = GS;
                }
            }
            w0 = (code < 256 && f->has_widths && f->widths[code] >= 0) ? f->widths[code] * f->fmatrix.a * 1000 : 0;
        } else if (g->tr != 3 && g->tr != 7) {
            uint32_t col = (g->tr == 1 || g->tr == 5) ? g->stroke : g->fill;
            int ncomp = (g->tr == 1 || g->tr == 5) ? g->stroke_n : g->fill_n;
            if (ncomp > 0) draw_glyph(r, f, gid, &trm, col, (g->tr == 1 || g->tr == 5) ? g->sa : g->fa);
        }
        /* লেখা বের করা */
        if (r->o->want_text && r->at_depth >= 0) {       /* ActualText-এর ভেতরে: শুধু বাক্স */
            double x0, y0, x1, y1, adv = w0 / 1000.0;
            mat_apply(&trm, 0, -0.22, &x0, &y0);
            mat_apply(&trm, adv > 0.05 ? adv : 0.5, 0.82, &x1, &y1);
            if (!r->at_has) { r->at_box[0] = x0; r->at_box[1] = y0; r->at_box[2] = x1; r->at_box[3] = y1; r->at_has = 1; }
            if (x0 < r->at_box[0]) r->at_box[0] = x0;
            if (y0 < r->at_box[1]) r->at_box[1] = y0;
            if (x1 > r->at_box[2]) r->at_box[2] = x1;
            if (y1 > r->at_box[3]) r->at_box[3] = y1;
        } else if (r->o->want_text) {
            double x0, y0, x1, y1, adv = w0 / 1000.0;
            mat_apply(&trm, 0, -0.22, &x0, &y0);
            mat_apply(&trm, adv > 0.05 ? adv : 0.5, 0.82, &x1, &y1);
            nu = font_unicode(f, code, u, 8);
            if (nu == 0 && f->kind != 0 && code >= 32 && code < 127 && !f->names[code]) { u[0] = code; nu = 1; }
            for (k = 0; k < nu; k++) {
                double a = x0 + (x1 - x0) * k / nu, b = x0 + (x1 - x0) * (k + 1) / nu;
                add_text(r, u[k], a, y0, b, y1);
            }
        }
        /* এগোনো */
        tx = (w0 / 1000.0 * fs + g->tc + ((nb == 1 && code == 32) ? g->tw : 0)) * th;
        if (f->vertical) {
            mat t = { 1, 0, 0, 1, 0, -(w0 / 1000.0 * fs + g->tc) };
            r->tm = mat_mul(t, r->tm);
        } else {
            mat t = { 1, 0, 0, 1, tx, 0 };
            r->tm = mat_mul(t, r->tm);
        }
    }
}

/* ---------- ছবি ---------- */

typedef struct img {
    int w, h;
    uint32_t *px;                     /* RGB (alpha নিচের a-তে) */
    unsigned char *a;                 /* NULL = সব অস্বচ্ছ */
} img;

static void free_img(img *im) { free(im->px); free(im->a); }

/* raw বিট থেকে sample */
static int sample(const unsigned char *row, int x, int bpc)
{
    switch (bpc) {
    case 1: return (row[x >> 3] >> (7 - (x & 7))) & 1;
    case 2: return (row[x >> 2] >> (6 - 2 * (x & 3))) & 3;
    case 4: return (row[x >> 1] >> (4 - 4 * (x & 1))) & 15;
    case 16: return row[2 * x];
    default: return row[x];
    }
}

static unsigned char *img_bytes(rctx *r, pobj *dict, const unsigned char *raw, long rawlen, long *len, const char **codec)
{
    pdf_doc *d = r->d;
    pobj *f = pdf_dict_raw(dict, "Filter"), *p = pdf_dict_raw(dict, "DecodeParms");
    if (!f) f = pdf_dict_raw(dict, "F");
    if (!p) p = pdf_dict_raw(dict, "DP");
    return pdf_decode_filters(d, raw, rawlen, f, p, len, 1, codec);
}

/* ছবির dict + ডেটা → img; stencil = ImageMask */
static int decode_image(rctx *r, pobj *dict, pobj *res, const unsigned char *raw, long rawlen, img *out, int *stencil, int as_alpha)
{
    pdf_doc *d = r->d;
    long w = pdf_int(d, dict, "Width", 0), h = pdf_int(d, dict, "Height", 0), len = 0, x, y;
    int bpc = (int)pdf_int(d, dict, "BitsPerComponent", 8), n, im;
    pobj *csn = pdf_get(d, dict, "ColorSpace"), *idx = NULL, *dec;
    const char *codec = NULL;
    unsigned char *data;
    int inv = 0;
    if (!w) w = pdf_int(d, dict, "W", 0);
    if (!h) h = pdf_int(d, dict, "H", 0);
    if (!pdf_get(d, dict, "BitsPerComponent")) bpc = (int)pdf_int(d, dict, "BPC", 8);
    if (!csn) csn = pdf_get(d, dict, "CS");
    im = (pdf_get(d, dict, "ImageMask") && pdf_get(d, dict, "ImageMask")->u.b) ||
         (pdf_get(d, dict, "IM") && pdf_get(d, dict, "IM")->u.b);
    if (w <= 0 || h <= 0 || w > 20000 || h > 20000) return -1;
    memset(out, 0, sizeof(*out));
    *stencil = im;
    data = img_bytes(r, dict, raw, rawlen, &len, &codec);
    if (!data) return -1;
    dec = pdf_get(d, dict, "Decode");
    if (!dec) dec = pdf_get(d, dict, "D");
    if (dec && dec->type == P_ARRAY && dec->u.arr.n >= 2 && pdf_num(pdf_resolve(d, dec->u.arr.v[0]), 0) > 0.5) inv = 1;
    if (codec && !strcmp(codec, "DCTDecode")) {
        gfx_surface *s = gfx_load_image_stb(data, len);
        free(data);
        if (!s) return -1;
        out->w = s->w;
        out->h = s->h;
        out->px = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)s->w * (size_t)s->h);
        if (!out->px) { gfx_surface_free(s); return -1; }
        for (y = 0; y < s->h; y++) memcpy(out->px + y * s->w, s->px + y * s->stride, sizeof(uint32_t) * (size_t)s->w);
        gfx_surface_free(s);
        if (as_alpha) {                                /* SMask-এ JPEG: ধূসর মান = alpha */
            out->a = (unsigned char *)malloc((size_t)out->w * (size_t)out->h);
            if (out->a) for (x = 0; x < (long)out->w * out->h; x++) out->a[x] = (unsigned char)GFX_G(out->px[x]);
        }
        return 0;
    }
    if (codec) {                                       /* JPX/JBIG2/CCITT — এখনো নয় */
        free(data);
        snprintf(warn, sizeof(warn), "an image in %s format is not shown", codec);
        return -1;
    }
    out->w = (int)w;
    out->h = (int)h;
    if (im || as_alpha) {
        n = 1;
        if (im) bpc = 1;
    } else {
        pobj *nm = csn;
        if (nm && nm->type == P_NAME && cs_ncomp(r, nm, NULL, 0) == -1) {
            pobj *csd = pdf_get(d, res, "ColorSpace");
            nm = pdf_get(d, csd, (const char *)nm->u.str.s);
        }
        n = cs_ncomp(r, nm, &idx, 0);
        if (n <= 0) n = 3;
        if (n == 5 || n == 6) n = n == 5 ? 1 : 4;      /* Separation/DeviceN: সরলীকরণ */
    }
    {
        long rowb = (w * n * bpc + 7) / 8;
        out->px = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)w * (size_t)h);
        if ((im || as_alpha)) out->a = (unsigned char *)malloc((size_t)w * (size_t)h);
        if (!out->px || ((im || as_alpha) && !out->a)) { free(data); free_img(out); return -1; }
        for (y = 0; y < h; y++) {
            const unsigned char *row = data + y * rowb;
            if ((y + 1) * rowb > len) {               /* ছোট ডেটা: বাকি সাদা */
                for (x = 0; x < w; x++) { out->px[y * w + x] = GFX_WHITE; if (out->a) out->a[y * w + x] = im ? 0 : 255; }
                continue;
            }
            for (x = 0; x < w; x++) {
                uint32_t c;
                int maxv = (1 << (bpc > 8 ? 8 : bpc)) - 1;
                if (im) {
                    int s = sample(row, (int)x, 1);
                    out->a[y * w + x] = (unsigned char)(((s == 0) != inv) ? 255 : 0);
                    out->px[y * w + x] = 0;
                    continue;
                }
                if (as_alpha) {
                    int s = sample(row, (int)x, bpc);
                    out->a[y * w + x] = (unsigned char)(s * 255 / maxv);
                    out->px[y * w + x] = 0;
                    continue;
                }
                if (idx) {
                    c = indexed_color(r, idx, sample(row, (int)x, bpc));
                } else if (n == 1) {
                    int s = sample(row, (int)x, bpc) * 255 / maxv;
                    if (inv) s = 255 - s;
                    c = GFX_RGB(s, s, s);
                } else if (n == 4) {
                    double v[4];
                    int k;
                    for (k = 0; k < 4; k++) v[k] = sample(row, (int)(x * 4 + k), bpc) / (double)maxv;
                    c = cmyk_rgb(v[0], v[1], v[2], v[3]);
                } else {
                    int v[3], k;
                    for (k = 0; k < 3; k++) v[k] = sample(row, (int)(x * 3 + k), bpc) * 255 / maxv;
                    c = GFX_RGB(v[0], v[1], v[2]);
                }
                out->px[y * w + x] = c;
            }
        }
    }
    free(data);
    return 0;
}

static void draw_img(rctx *r, img *im, int stencil)
{
    gstate *g = GS;
    mat inv;
    double xs[4], ys[4], minx = 1e30, miny = 1e30, maxx = -1e30, maxy = -1e30;
    int i, x, y, x0, y0, x1, y1;
    if (r->o->no_draw) return;
    if (mat_inv(&g->ctm, &inv) < 0) return;
    mat_apply(&g->ctm, 0, 0, &xs[0], &ys[0]);
    mat_apply(&g->ctm, 1, 0, &xs[1], &ys[1]);
    mat_apply(&g->ctm, 0, 1, &xs[2], &ys[2]);
    mat_apply(&g->ctm, 1, 1, &xs[3], &ys[3]);
    for (i = 0; i < 4; i++) {
        if (xs[i] < minx) minx = xs[i];
        if (xs[i] > maxx) maxx = xs[i];
        if (ys[i] < miny) miny = ys[i];
        if (ys[i] > maxy) maxy = ys[i];
    }
    if (minx < g->cx0) minx = g->cx0;
    if (miny < g->cy0) miny = g->cy0;
    if (maxx > g->cx1) maxx = g->cx1;
    if (maxy > g->cy1) maxy = g->cy1;
    x0 = (int)floor(minx < 0 ? 0 : minx);
    y0 = (int)floor(miny < 0 ? 0 : miny);
    x1 = (int)ceil(maxx > r->s->w ? r->s->w : maxx);
    y1 = (int)ceil(maxy > r->s->h ? r->s->h : maxy);
    {
        /* ছোট করে দেখালে কয়েকটা sample-এর গড় (ঝিরঝিরে না হয়) */
        double sx = hypot(g->ctm.a, g->ctm.b) / im->w, sy = hypot(g->ctm.c, g->ctm.d) / im->h;
        int box = (sx < 0.5 && sy < 0.5) ? 2 : 1;
        for (y = y0; y < y1; y++) {
            uint32_t *row = r->s->px + (long)y * r->s->stride;
            for (x = x0; x < x1; x++) {
                double u, v;
                int ix, iy;
                mat_apply(&inv, x + 0.5, y + 0.5, &u, &v);
                if (u < 0 || u >= 1 || v < 0 || v >= 1) continue;
                ix = (int)(u * im->w);
                iy = (int)((1 - v) * im->h);
                if (ix >= im->w) ix = im->w - 1;
                if (iy >= im->h) iy = im->h - 1;
                if (stencil) {
                    if (im->a[iy * im->w + ix]) blend_px(&row[x], g->fill, g->fa);
                } else if (box == 2 && ix + 1 < im->w && iy + 1 < im->h) {
                    uint32_t a = im->px[iy * im->w + ix], b = im->px[iy * im->w + ix + 1];
                    uint32_t c = im->px[(iy + 1) * im->w + ix], dd = im->px[(iy + 1) * im->w + ix + 1];
                    uint32_t avg = GFX_RGB((GFX_R(a) + GFX_R(b) + GFX_R(c) + GFX_R(dd)) / 4,
                                           (GFX_G(a) + GFX_G(b) + GFX_G(c) + GFX_G(dd)) / 4,
                                           (GFX_B(a) + GFX_B(b) + GFX_B(c) + GFX_B(dd)) / 4);
                    blend_px(&row[x], avg, (im->a ? im->a[iy * im->w + ix] / 255.0 : 1.0) * g->fa);
                } else {
                    blend_px(&row[x], im->px[iy * im->w + ix], (im->a ? im->a[iy * im->w + ix] / 255.0 : 1.0) * g->fa);
                }
            }
        }
    }
}

static void do_image(rctx *r, pobj *dict, pobj *res, const unsigned char *raw, long rawlen)
{
    img im;
    int stencil = 0;
    pobj *sm;
    if (r->o->no_draw) return;
    if (decode_image(r, dict, res, raw, rawlen, &im, &stencil, 0) < 0) {
        /* দেখানো যায় না: হালকা ধূসর বাক্স */
        gstate *g = GS;
        uint32_t keep = g->fill;
        int keepn = g->fill_n;
        path_reset(r);
        path_move(r, 0, 0); path_line(r, 1, 0); path_line(r, 1, 1); path_line(r, 0, 1); path_close(r);
        g->fill = GFX_RGB(225, 225, 225);
        g->fill_n = 1;
        do_fill(r, 0);
        g->fill = keep;
        g->fill_n = keepn;
        path_reset(r);
        return;
    }
    sm = pdf_get(r->d, dict, "SMask");
    if (!sm) {
        pobj *mk = pdf_get(r->d, dict, "Mask");
        if (mk && mk->type == P_STREAM) sm = mk;
    }
    if (!stencil && sm && sm->type == P_STREAM) {
        img a;
        int st2;
        int is_mask = sm == pdf_get(r->d, dict, "Mask");
        if (decode_image(r, sm, res, r->d->buf + sm->u.st.off, sm->u.st.len, &a, &st2, !is_mask) == 0) {
            int x, y;
            im.a = (unsigned char *)malloc((size_t)im.w * (size_t)im.h);
            if (im.a) {
                for (y = 0; y < im.h; y++)
                    for (x = 0; x < im.w; x++) {
                        int ax = x * a.w / im.w, ay = y * a.h / im.h;
                        im.a[y * im.w + x] = a.a ? (is_mask ? (unsigned char)(255 - a.a[ay * a.w + ax]) : a.a[ay * a.w + ax])
                                                 : 255;
                        if (is_mask && a.a) im.a[y * im.w + x] = a.a[ay * a.w + ax] ? 0 : 255;
                    }
            }
            free_img(&a);
        }
    }
    draw_img(r, &im, stencil);
    free_img(&im);
}

/* ---------- content চালানো ---------- */

static double opn(pobj **ops, int n, int i)
{
    return (i < n) ? pdf_num(ops[i], 0) : 0;
}

static void set_color(rctx *r, int fill, pobj **ops, int n)
{
    gstate *g = GS;
    double v[8];
    int i, k = 0, comps = fill ? g->fill_n : g->stroke_n;
    pobj *idx = fill ? g->fill_idx : g->stroke_idx;
    for (i = 0; i < n && k < 8; i++) if (ops[i]->type == P_INT || ops[i]->type == P_REAL) v[k++] = pdf_num(ops[i], 0);
    if (comps <= 0 || k == 0) return;                  /* pattern */
    if (fill) g->fill = comps_rgb(r, comps, idx, v, k);
    else g->stroke = comps_rgb(r, comps, idx, v, k);
}

static void do_xobject(rctx *r, pobj *res, const char *name)
{
    pdf_doc *d = r->d;
    pobj *xo = pdf_get(d, pdf_get(d, res, "XObject"), name);
    const char *st;
    if (!xo || xo->type != P_STREAM) return;
    st = pdf_name(d, xo, "Subtype");
    if (st && !strcmp(st, "Image")) {
        do_image(r, xo, res, d->buf + xo->u.st.off, xo->u.st.len);
    } else if (st && !strcmp(st, "Form") && r->depth < 12 && r->gsp < 31) {
        long len;
        unsigned char *data = pdf_stream_data(d, xo, &len, 0, NULL);
        pobj *fres = pdf_get(d, xo, "Resources"), *bbox = pdf_get(d, xo, "BBox");
        mat fm = mat_from_array(d, pdf_get(d, xo, "Matrix"), (mat){ 1, 0, 0, 1, 0, 0 });
        mat save_tm = r->tm, save_tlm = r->tlm;
        if (!data) return;
        r->gsp++;
        r->gs[r->gsp] = r->gs[r->gsp - 1];
        GS->ctm = mat_mul(fm, GS->ctm);
        if (bbox && bbox->type == P_ARRAY && bbox->u.arr.n >= 4) {
            double b[4];
            int i;
            for (i = 0; i < 4; i++) b[i] = pdf_num(pdf_resolve(d, bbox->u.arr.v[i]), 0);
            path_reset(r);
            path_move(r, b[0], b[1]); path_line(r, b[2], b[1]); path_line(r, b[2], b[3]); path_line(r, b[0], b[3]);
            r->pending_clip = 1;
            apply_clip(r);
            path_reset(r);
        }
        r->depth++;
        run_content(r, data, len, fres ? fres : res);
        r->depth--;
        r->gsp--;
        r->tm = save_tm;
        r->tlm = save_tlm;
    }
}

static void do_gs(rctx *r, pobj *res, const char *name)
{
    pdf_doc *d = r->d;
    pobj *eg = pdf_get(d, pdf_get(d, res, "ExtGState"), name), *o;
    gstate *g = GS;
    if (!eg) return;
    if ((o = pdf_get(d, eg, "LW"))) g->lw = pdf_num(o, g->lw);
    if ((o = pdf_get(d, eg, "ca"))) g->fa = clamp01(pdf_num(o, 1));
    if ((o = pdf_get(d, eg, "CA"))) g->sa = clamp01(pdf_num(o, 1));
    if ((o = pdf_get(d, eg, "LC"))) g->cap = (int)pdf_num(o, 0);
    if ((o = pdf_get(d, eg, "LJ"))) g->join = (int)pdf_num(o, 0);
    if ((o = pdf_get(d, eg, "Font")) && o->type == P_ARRAY && o->u.arr.n >= 2) {
        g->font = load_font(d, o->u.arr.v[0]);
        g->fs = pdf_num(pdf_resolve(d, o->u.arr.v[1]), 12);
    }
}

static int is_space_ch(int c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == 0; }

/* inline image: BI ... ID <ডেটা> EI */
static void do_inline_image(rctx *r, pdf_lexer *lx, pobj *res)
{
    pdf_doc *d = r->d;
    pobj *dict = (pobj *)pdf_alloc(d, sizeof(pobj));
    char **k = (char **)pdf_alloc(d, sizeof(char *) * 32);
    pobj **v = (pobj **)pdf_alloc(d, sizeof(pobj *) * 32);
    int n = 0;
    const unsigned char *start, *p;
    long w, h, bpc, comps = 1, rowb, need = -1;
    pobj *o;
    if (!dict || !k || !v) return;
    dict->type = P_DICT;
    while ((o = pdf_lex_object(lx)) != NULL) {
        if (o->type == P_KEYWORD) break;               /* ID */
        if (o->type == P_NAME && n < 32) {
            pobj *val = pdf_lex_object(lx);
            if (!val) return;
            k[n] = (char *)o->u.str.s;
            v[n] = val;
            n++;
        }
    }
    dict->u.dict.k = k;
    dict->u.dict.v = v;
    dict->u.dict.n = n;
    if (lx->p < lx->end && (*lx->p == ' ' || *lx->p == '\n' || *lx->p == '\r' || *lx->p == '\t')) lx->p++;
    start = lx->p;
    /* ফিল্টার না থাকলে দৈর্ঘ্য জানা */
    w = pdf_int(d, dict, "W", pdf_int(d, dict, "Width", 0));
    h = pdf_int(d, dict, "H", pdf_int(d, dict, "Height", 0));
    bpc = pdf_int(d, dict, "BPC", pdf_int(d, dict, "BitsPerComponent", 8));
    {
        pobj *cs = pdf_get(d, dict, "CS");
        if (!cs) cs = pdf_get(d, dict, "ColorSpace");
        if ((pdf_get(d, dict, "IM") && pdf_get(d, dict, "IM")->u.b) || (pdf_get(d, dict, "ImageMask") && pdf_get(d, dict, "ImageMask")->u.b)) {
            comps = 1;
            bpc = 1;
        } else if (cs) {
            pobj *idx;
            int nc = resolve_cs(r, res, cs, &idx);
            comps = nc == 4 ? 4 : nc == 3 ? 3 : 1;
        }
        rowb = (w * comps * bpc + 7) / 8;
        if (!pdf_get(d, dict, "F") && !pdf_get(d, dict, "Filter")) need = rowb * h;
    }
    if (need >= 0 && start + need <= lx->end) {
        p = start + need;
    } else {
        p = start;
        for (;;) {                                     /* "EI" যার দুপাশে ফাঁকা */
            while (p + 1 < lx->end && !(p[0] == 'E' && p[1] == 'I')) p++;
            if (p + 1 >= lx->end) { p = lx->end; break; }
            if (p > start && is_space_ch(p[-1]) && (p + 2 >= lx->end || is_space_ch(p[2]))) break;
            p++;
        }
    }
    do_image(r, dict, res, start, (long)(p - start));
    lx->p = p;
    while (lx->p < lx->end && is_space_ch(*lx->p)) lx->p++;
    if (lx->p + 1 < lx->end && lx->p[0] == 'E' && lx->p[1] == 'I') lx->p += 2;
}

static void run_content(rctx *r, const unsigned char *data, long len, pobj *res)
{
    pdf_doc *d = r->d;
    pdf_lexer lx;
    pobj *ops[64], *o, *prev_res = r->res;
    int n = 0;
    lx.d = d;
    lx.p = data;
    lx.end = data + len;
    r->res = res;
    while ((o = pdf_lex_object(&lx)) != NULL) {
        const char *op;
        gstate *g;
        if (o->type != P_KEYWORD) {
            if (n < 64) ops[n++] = o;
            continue;
        }
        if (++r->ops > 4000000) break;                 /* অস্বাভাবিক বড় */
        op = (const char *)o->u.str.s;
        g = GS;
        switch (op[0]) {
        case 'q':
            if (!op[1] && r->gsp < 31) { r->gsp++; r->gs[r->gsp] = r->gs[r->gsp - 1]; }
            break;
        case 'Q':
            if (!op[1] && r->gsp > 0) r->gsp--;
            break;
        case 'c':
            if (!strcmp(op, "cm") && n >= 6) {
                mat m = { opn(ops, n, 0), opn(ops, n, 1), opn(ops, n, 2), opn(ops, n, 3), opn(ops, n, 4), opn(ops, n, 5) };
                g->ctm = mat_mul(m, g->ctm);
            } else if (!op[1] && n >= 6) {
                path_curve(r, opn(ops, n, 0), opn(ops, n, 1), opn(ops, n, 2), opn(ops, n, 3), opn(ops, n, 4), opn(ops, n, 5));
            } else if (!strcmp(op, "cs") && n >= 1) {
                g->fill_n = resolve_cs(r, res, ops[0], &g->fill_idx);
                g->fill = 0;
            }
            break;
        case 'C':
            if (!strcmp(op, "CS") && n >= 1) {
                g->stroke_n = resolve_cs(r, res, ops[0], &g->stroke_idx);
                g->stroke = 0;
            }
            break;
        case 'w': if (!op[1] && n >= 1) g->lw = opn(ops, n, 0); break;
        case 'J': if (!op[1] && n >= 1) g->cap = (int)opn(ops, n, 0); break;
        case 'j': if (!op[1] && n >= 1) g->join = (int)opn(ops, n, 0); break;
        case 'd':
            if (!op[1] && n >= 2 && ops[0]->type == P_ARRAY) {
                int i;
                g->ndash = 0;
                for (i = 0; i < ops[0]->u.arr.n && i < 8; i++) g->dash[g->ndash++] = pdf_num(ops[0]->u.arr.v[i], 0);
                if (g->ndash == 1) { g->dash[1] = g->dash[0]; g->ndash = 2; }
                {
                    double tot = 0;
                    for (i = 0; i < g->ndash; i++) tot += g->dash[i];
                    if (tot <= 0) g->ndash = 0;
                }
                g->dash_phase = opn(ops, n, 1);
            }
            break;
        case 'g':
            if (!op[1] && n >= 1) { g->fill_n = 1; g->fill_idx = NULL; set_color(r, 1, ops, n); }
            else if (!strcmp(op, "gs") && n >= 1 && ops[0]->type == P_NAME) do_gs(r, res, (char *)ops[0]->u.str.s);
            break;
        case 'G': if (!op[1] && n >= 1) { g->stroke_n = 1; g->stroke_idx = NULL; set_color(r, 0, ops, n); } break;
        case 'r': if (!strcmp(op, "rg")) { g->fill_n = 3; g->fill_idx = NULL; set_color(r, 1, ops, n); }
                  else if (!strcmp(op, "re") && n >= 4) {
                      double x = opn(ops, n, 0), y = opn(ops, n, 1), w = opn(ops, n, 2), h = opn(ops, n, 3);
                      path_move(r, x, y); path_line(r, x + w, y); path_line(r, x + w, y + h); path_line(r, x, y + h);
                      path_close(r);
                  }
                  break;
        case 'R': if (!strcmp(op, "RG")) { g->stroke_n = 3; g->stroke_idx = NULL; set_color(r, 0, ops, n); } break;
        case 'k': if (!op[1]) { g->fill_n = 4; g->fill_idx = NULL; set_color(r, 1, ops, n); } break;
        case 'K': if (!op[1]) { g->stroke_n = 4; g->stroke_idx = NULL; set_color(r, 0, ops, n); } break;
        case 's':
            if (!strcmp(op, "sc") || !strcmp(op, "scn")) set_color(r, 1, ops, n);
            else if (!op[1]) { path_close(r); do_stroke(r); apply_clip(r); path_reset(r); }
            else if (!strcmp(op, "sh")) { /* shading — এখনো নয় */ }
            break;
        case 'S':
            if (!strcmp(op, "SC") || !strcmp(op, "SCN")) set_color(r, 0, ops, n);
            else if (!op[1]) { do_stroke(r); apply_clip(r); path_reset(r); }
            break;
        case 'm': if (!op[1] && n >= 2) path_move(r, opn(ops, n, 0), opn(ops, n, 1)); break;
        case 'l': if (!op[1] && n >= 2) path_line(r, opn(ops, n, 0), opn(ops, n, 1)); break;
        case 'v': if (!op[1] && n >= 4) path_curve(r, r->px, r->py, opn(ops, n, 0), opn(ops, n, 1), opn(ops, n, 2), opn(ops, n, 3)); break;
        case 'y': if (!op[1] && n >= 4) path_curve(r, opn(ops, n, 0), opn(ops, n, 1), opn(ops, n, 2), opn(ops, n, 3), opn(ops, n, 2), opn(ops, n, 3)); break;
        case 'h': if (!op[1]) path_close(r); break;
        case 'f': case 'F':
            do_fill(r, op[1] == '*');
            apply_clip(r);
            path_reset(r);
            break;
        case 'B':
            if (!strcmp(op, "BT")) { mat id = { 1, 0, 0, 1, 0, 0 }; r->tm = r->tlm = id; }
            else if (!strcmp(op, "BMC")) r->mc_depth++;
            else if (!strcmp(op, "BDC")) {
                pobj *props = n >= 2 ? ops[1] : NULL, *at;
                r->mc_depth++;
                if (props && props->type == P_NAME) props = pdf_get(d, pdf_get(d, res, "Properties"), (char *)props->u.str.s);
                at = props ? pdf_get(d, props, "ActualText") : NULL;
                if (at && at->type == P_STR && r->at_depth < 0 && r->o->want_text) {
                    const unsigned char *t = at->u.str.s;
                    int len = at->u.str.len;
                    if (len >= 2 && t[0] == 0xFE && t[1] == 0xFF) r->at_n = utf16_decode(t + 2, len - 2, r->at_text, 128);
                    else { int i; r->at_n = 0; for (i = 0; i < len && i < 128; i++) r->at_text[r->at_n++] = t[i]; }
                    r->at_depth = r->mc_depth;
                    r->at_has = 0;
                }
            }
            else if (!strcmp(op, "BI")) do_inline_image(r, &lx, res);
            else if (!op[1] || !strcmp(op, "B*")) { do_fill(r, op[1] == '*'); do_stroke(r); apply_clip(r); path_reset(r); }
            break;
        case 'b':
            if (!op[1] || !strcmp(op, "b*")) { path_close(r); do_fill(r, op[1] == '*'); do_stroke(r); apply_clip(r); path_reset(r); }
            break;
        case 'n': if (!op[1]) { apply_clip(r); path_reset(r); } break;
        case 'W': if (!op[1]) r->pending_clip = 1; else if (!strcmp(op, "W*")) r->pending_clip = 2; break;
        case 'T':
            switch (op[1]) {
            case 'f':
                if (n >= 2 && ops[0]->type == P_NAME) {
                    g->font = load_font(d, pdf_get(d, pdf_get(d, res, "Font"), (char *)ops[0]->u.str.s));
                    g->fs = opn(ops, n, 1);
                }
                break;
            case 'd': case 'D':
                if (n >= 2) {
                    mat t = { 1, 0, 0, 1, opn(ops, n, 0), opn(ops, n, 1) };
                    if (op[1] == 'D') g->tl = -opn(ops, n, 1);
                    r->tlm = mat_mul(t, r->tlm);
                    r->tm = r->tlm;
                }
                break;
            case 'm':
                if (n >= 6) {
                    mat m = { opn(ops, n, 0), opn(ops, n, 1), opn(ops, n, 2), opn(ops, n, 3), opn(ops, n, 4), opn(ops, n, 5) };
                    r->tm = r->tlm = m;
                }
                break;
            case '*': {
                mat t = { 1, 0, 0, 1, 0, -g->tl };
                r->tlm = mat_mul(t, r->tlm);
                r->tm = r->tlm;
                break;
            }
            case 'j':
                if (n >= 1 && ops[n - 1]->type == P_STR) show_string(r, ops[n - 1]->u.str.s, ops[n - 1]->u.str.len);
                break;
            case 'J':
                if (n >= 1 && ops[n - 1]->type == P_ARRAY) {
                    pobj *a = ops[n - 1];
                    int i;
                    for (i = 0; i < a->u.arr.n; i++) {
                        pobj *e = a->u.arr.v[i];
                        if (e->type == P_STR) show_string(r, e->u.str.s, e->u.str.len);
                        else if (e->type == P_INT || e->type == P_REAL) {
                            g = GS;
                            if (g->font && g->font->vertical) {
                                mat t = { 1, 0, 0, 1, 0, -(-pdf_num(e, 0) / 1000.0 * g->fs) };
                                r->tm = mat_mul(t, r->tm);
                            } else {
                                mat t = { 1, 0, 0, 1, -pdf_num(e, 0) / 1000.0 * g->fs * g->th / 100.0, 0 };
                                r->tm = mat_mul(t, r->tm);
                            }
                        }
                    }
                }
                break;
            case 'c': if (n >= 1) g->tc = opn(ops, n, 0); break;
            case 'w': if (n >= 1) g->tw = opn(ops, n, 0); break;
            case 'z': if (n >= 1) g->th = opn(ops, n, 0); break;
            case 'L': if (n >= 1) g->tl = opn(ops, n, 0); break;
            case 's': if (n >= 1) g->ts = opn(ops, n, 0); break;
            case 'r': if (n >= 1) g->tr = (int)opn(ops, n, 0); break;
            }
            break;
        case '\'': case '"': {
            mat t = { 1, 0, 0, 1, 0, -g->tl };
            if (op[0] == '"' && n >= 3) { g->tw = opn(ops, n, 0); g->tc = opn(ops, n, 1); }
            r->tlm = mat_mul(t, r->tlm);
            r->tm = r->tlm;
            if (n >= 1 && ops[n - 1]->type == P_STR) show_string(r, ops[n - 1]->u.str.s, ops[n - 1]->u.str.len);
            break;
        }
        case 'D':
            if (!strcmp(op, "Do") && n >= 1 && ops[0]->type == P_NAME) do_xobject(r, res, (char *)ops[0]->u.str.s);
            break;
        case 'E':
            if (!strcmp(op, "EMC") && r->mc_depth > 0) {
                if (r->at_depth == r->mc_depth) {      /* ActualText শেষ: পুরো লেখা glyph-গুলোর বাক্সে */
                    int i;
                    r->at_depth = -1;
                    if (r->at_has) {
                        for (i = 0; i < r->at_n; i++) {
                            double a = r->at_box[0] + (r->at_box[2] - r->at_box[0]) * i / r->at_n;
                            double b = r->at_box[0] + (r->at_box[2] - r->at_box[0]) * (i + 1) / r->at_n;
                            add_text(r, r->at_text[i], a, r->at_box[1], b, r->at_box[3]);
                        }
                    }
                }
                r->mc_depth--;
            }
            break;
        default:
            break;                                     /* BDC/EMC/ri/i/d0/d1 ... */
        }
        n = 0;
    }
    r->res = prev_res;
}

/* ---------- পাতা ---------- */

static mat page_ctm(pdf_page *p, double s, int *w, int *h)
{
    double x0 = p->box[0], y0 = p->box[1], x1 = p->box[2], y1 = p->box[3];
    double pw = (x1 - x0) * s, ph = (y1 - y0) * s;
    mat m;
    switch (p->rotate) {
    case 90:  m.a = 0;  m.b = s;  m.c = s;  m.d = 0;  m.e = -y0 * s; m.f = -x0 * s; *w = (int)(ph + .5); *h = (int)(pw + .5); break;
    case 180: m.a = -s; m.b = 0;  m.c = 0;  m.d = s;  m.e = x1 * s;  m.f = -y0 * s; *w = (int)(pw + .5); *h = (int)(ph + .5); break;
    case 270: m.a = 0;  m.b = -s; m.c = -s; m.d = 0;  m.e = y1 * s;  m.f = x1 * s;  *w = (int)(ph + .5); *h = (int)(pw + .5); break;
    default:  m.a = s;  m.b = 0;  m.c = 0;  m.d = -s; m.e = -x0 * s; m.f = y1 * s;  *w = (int)(pw + .5); *h = (int)(ph + .5); break;
    }
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
    return m;
}

void pdf_page_size(pdf_doc *d, int page, double scale, int *w, int *h)
{
    *w = *h = 1;
    if (page < 0 || page >= d->npages) return;
    page_ctm(&d->pages[page], scale, w, h);
}

gfx_surface *pdf_render_page(pdf_doc *d, int page, const pdf_render_opts *o, pdf_textitem **text, int *ntext)
{
    rctx *r;
    pdf_page *p;
    gfx_surface *s = NULL;
    int w, h;
    pobj *contents;
    if (text) *text = NULL;
    if (ntext) *ntext = 0;
    if (page < 0 || page >= d->npages) return NULL;
    warn[0] = 0;
    pdf_render_init();
    p = &d->pages[page];
    r = (rctx *)calloc(1, sizeof(rctx));
    if (!r) return NULL;
    r->d = d;
    r->o = o;
    r->at_depth = -1;
    r->gs[0].ctm = page_ctm(p, o->scale, &w, &h);
    if ((long)w * h > 60L * 1000 * 1000) { free(r); return NULL; }
    if (!o->no_draw || 1) {
        s = gfx_surface_new(w, h);
        if (!s) { free(r); return NULL; }
        gfx_clear(s, GFX_WHITE);
    }
    r->s = s;
    {
        gstate *g = &r->gs[0];
        g->fill = g->stroke = GFX_RGB(0, 0, 0);
        g->fa = g->sa = 1;
        g->fill_n = g->stroke_n = 1;
        g->lw = 1;
        g->th = 100;
        g->cx0 = 0; g->cy0 = 0; g->cx1 = w; g->cy1 = h;
    }
    contents = pdf_get(d, p->dict, "Contents");
    if (contents && contents->type == P_STREAM) {
        long len;
        unsigned char *data = pdf_stream_data(d, contents, &len, 0, NULL);
        if (data) run_content(r, data, len, p->resources);
    } else if (contents && contents->type == P_ARRAY) {
        /* কয়েকটা stream জোড়া লাগিয়ে (মাঝে ফাঁকা) — একটা token দুই stream-এ ভাগ হতে পারে */
        long tot = 0, len;
        int i;
        unsigned char *all;
        for (i = 0; i < contents->u.arr.n; i++) {
            pobj *st = pdf_resolve(d, contents->u.arr.v[i]);
            if (st && st->type == P_STREAM && pdf_stream_data(d, st, &len, 0, NULL)) tot += len + 1;
        }
        all = (unsigned char *)malloc((size_t)tot + 1);
        if (all) {
            long pos = 0;
            for (i = 0; i < contents->u.arr.n; i++) {
                pobj *st = pdf_resolve(d, contents->u.arr.v[i]);
                unsigned char *dd;
                if (st && st->type == P_STREAM && (dd = pdf_stream_data(d, st, &len, 0, NULL))) {
                    memcpy(all + pos, dd, (size_t)len);
                    pos += len;
                    all[pos++] = '\n';
                }
            }
            run_content(r, all, pos, p->resources);
            free(all);
        }
    }
    if (text) { *text = r->text; *ntext = r->ntext; } else free(r->text);
    free(r->pts);
    free(r->sub);
    free(r->closed);
    free(r);
    return s;
}
