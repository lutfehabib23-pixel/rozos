/* rozpdf — PDF ফাইল পড়া: lexer, object, xref, stream filter, পাতার গাছ (pdf.h দেখুন) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "pdf.h"
#include "zlib.h"

/* ---------- arena ---------- */

struct arena_blk {
    struct arena_blk *next;
    size_t used, size;
    /* তারপর ডেটা */
};

void *pdf_alloc(pdf_doc *d, size_t n)
{
    struct arena_blk *b = d->arena;
    void *p;
    n = (n + 15) & ~(size_t)15;
    if (!b || b->used + n > b->size) {
        size_t sz = n > 60000 ? n : 60000;
        b = (struct arena_blk *)malloc(sizeof(struct arena_blk) + 32 + sz);
        if (!b) return NULL;
        b->size = sz;
        b->used = 0;
        b->next = d->arena;
        d->arena = b;
    }
    p = (void *)((((uintptr_t)(b + 1)) + 15) & ~(uintptr_t)15);   /* ১৬-এ সাজানো শুরু */
    p = (char *)p + b->used;
    b->used += n;
    memset(p, 0, n);
    return p;
}

static pobj *new_obj(pdf_doc *d, int type)
{
    pobj *o = (pobj *)pdf_alloc(d, sizeof(pobj));
    if (o) o->type = type;
    return o;
}

static pobj null_obj = { P_NULL };

/* ---------- lexer ---------- */

static int is_ws(int c) { return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32; }
static int is_delim(int c)
{
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' || c == '}' ||
           c == '/' || c == '%';
}
static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void skip_ws(const unsigned char **pp, const unsigned char *end)
{
    const unsigned char *p = *pp;
    while (p < end) {
        if (is_ws(*p)) {
            p++;
        } else if (*p == '%') {
            while (p < end && *p != '\n' && *p != '\r') p++;
        } else {
            break;
        }
    }
    *pp = p;
}

enum { T_EOF, T_NUM, T_STR, T_NAME, T_KW, T_ARR_OPEN, T_ARR_CLOSE, T_DICT_OPEN, T_DICT_CLOSE, T_BRACE };

typedef struct tok {
    int t;
    double num;
    int is_int;
    long ival;
    unsigned char *s;                 /* T_STR/T_NAME/T_KW: arena-য় কপি */
    int len;
} tok;

static int next_tok(pdf_doc *d, const unsigned char **pp, const unsigned char *end, tok *t)
{
    const unsigned char *p;
    skip_ws(pp, end);
    p = *pp;
    memset(t, 0, sizeof(*t));
    if (p >= end) { t->t = T_EOF; return T_EOF; }
    if (*p == '[') { *pp = p + 1; return t->t = T_ARR_OPEN; }
    if (*p == ']') { *pp = p + 1; return t->t = T_ARR_CLOSE; }
    if (*p == '{' || *p == '}') { *pp = p + 1; return t->t = T_BRACE; }
    if (*p == '<' && p + 1 < end && p[1] == '<') { *pp = p + 2; return t->t = T_DICT_OPEN; }
    if (*p == '>' && p + 1 < end && p[1] == '>') { *pp = p + 2; return t->t = T_DICT_CLOSE; }
    if (*p == '>') { *pp = p + 1; return next_tok(d, pp, end, t); }   /* ভুল '>' — বাদ */
    if (*p == '/') {                                   /* নাম, #xx escape */
        const unsigned char *q = p + 1;
        int n = 0;
        unsigned char *o;
        while (q < end && !is_ws(*q) && !is_delim(*q)) q++;
        o = (unsigned char *)pdf_alloc(d, (size_t)(q - p) + 1);
        if (!o) return t->t = T_EOF;
        for (p = p + 1; p < q; p++) {
            if (*p == '#' && p + 2 < q && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) {
                o[n++] = (unsigned char)(hexval(p[1]) * 16 + hexval(p[2]));
                p += 2;
            } else {
                o[n++] = *p;
            }
        }
        o[n] = 0;
        t->s = o;
        t->len = n;
        *pp = q;
        return t->t = T_NAME;
    }
    if (*p == '(') {                                   /* লেখা: ব্যাকস্ল্যাশ, জোড়া বন্ধনী */
        const unsigned char *q = p + 1;
        int depth = 1, n = 0;
        unsigned char *o;
        while (q < end && depth) {                     /* আগে দৈর্ঘ্য */
            if (*q == '\\') q++;
            else if (*q == '(') depth++;
            else if (*q == ')') depth--;
            q++;
        }
        o = (unsigned char *)pdf_alloc(d, (size_t)(q - p) + 1);
        if (!o) return t->t = T_EOF;
        depth = 1;
        for (p = p + 1; p < end; p++) {
            int c = *p;
            if (c == '\\' && p + 1 < end) {
                c = *++p;
                switch (c) {
                case 'n': o[n++] = '\n'; break;
                case 'r': o[n++] = '\r'; break;
                case 't': o[n++] = '\t'; break;
                case 'b': o[n++] = '\b'; break;
                case 'f': o[n++] = '\f'; break;
                case '\r': if (p + 1 < end && p[1] == '\n') p++; break;   /* লাইন জোড়া */
                case '\n': break;
                default:
                    if (c >= '0' && c <= '7') {
                        int v = c - '0', k;
                        for (k = 0; k < 2 && p + 1 < end && p[1] >= '0' && p[1] <= '7'; k++) v = v * 8 + (*++p - '0');
                        o[n++] = (unsigned char)v;
                    } else {
                        o[n++] = (unsigned char)c;
                    }
                }
                continue;
            }
            if (c == '(') depth++;
            if (c == ')' && --depth == 0) { p++; break; }
            o[n++] = (unsigned char)c;
        }
        o[n] = 0;
        t->s = o;
        t->len = n;
        *pp = p;
        return t->t = T_STR;
    }
    if (*p == '<') {                                   /* hex লেখা */
        const unsigned char *q = p + 1;
        int n = 0, hi = -1;
        unsigned char *o;
        while (q < end && *q != '>') q++;
        o = (unsigned char *)pdf_alloc(d, (size_t)(q - p) / 2 + 2);
        if (!o) return t->t = T_EOF;
        for (p = p + 1; p < q; p++) {
            int v = hexval(*p);
            if (v < 0) continue;
            if (hi < 0) hi = v;
            else { o[n++] = (unsigned char)(hi * 16 + v); hi = -1; }
        }
        if (hi >= 0) o[n++] = (unsigned char)(hi * 16);
        o[n] = 0;
        t->s = o;
        t->len = n;
        *pp = q < end ? q + 1 : q;
        return t->t = T_STR;
    }
    if ((*p >= '0' && *p <= '9') || *p == '-' || *p == '+' || *p == '.') {   /* সংখ্যা */
        const unsigned char *q = p;
        int neg = 0, dot = 0, any = 0;
        double v = 0, frac = 0.1;
        long iv = 0;
        if (*q == '-' || *q == '+') { neg = *q == '-'; q++; }
        while (q < end && *q == '-') q++;              /* "--5" দেখা যায় */
        for (; q < end; q++) {
            if (*q >= '0' && *q <= '9') {
                any = 1;
                if (dot) { v += (*q - '0') * frac; frac *= 0.1; }
                else { v = v * 10 + (*q - '0'); if (iv < 100000000000L) iv = iv * 10 + (*q - '0'); }
            } else if (*q == '.' && !dot) {
                dot = 1;
            } else {
                break;
            }
        }
        if (any || dot) {
            t->num = neg ? -v : v;
            t->is_int = !dot;
            t->ival = neg ? -iv : iv;
            *pp = q;
            return t->t = T_NUM;
        }
    }
    {                                                  /* keyword */
        const unsigned char *q = p;
        unsigned char *o;
        while (q < end && !is_ws(*q) && !is_delim(*q)) q++;
        if (q == p) q++;                               /* একক অচেনা অক্ষর */
        o = (unsigned char *)pdf_alloc(d, (size_t)(q - p) + 1);
        if (!o) return t->t = T_EOF;
        memcpy(o, p, (size_t)(q - p));
        t->s = o;
        t->len = (int)(q - p);
        *pp = q;
        return t->t = T_KW;
    }
}

static pobj *parse_obj(pdf_doc *d, const unsigned char **pp, const unsigned char *end, int allow_ref, int depth);

/* tok থেকে object (আগে পড়া token) */
static pobj *obj_from_tok(pdf_doc *d, tok *t, const unsigned char **pp, const unsigned char *end, int allow_ref, int depth)
{
    pobj *o;
    if (depth > 64) return &null_obj;
    switch (t->t) {
    case T_NUM:
        if (t->is_int && allow_ref && t->ival >= 0) {  /* "n g R"? */
            const unsigned char *save = *pp;
            tok a, b;
            if (next_tok(d, pp, end, &a) == T_NUM && a.is_int && a.ival >= 0 &&
                next_tok(d, pp, end, &b) == T_KW && b.len == 1 && b.s[0] == 'R') {
                o = new_obj(d, P_REF);
                if (!o) return &null_obj;
                o->u.ref.num = (int)t->ival;
                o->u.ref.gen = (int)a.ival;
                return o;
            }
            *pp = save;
        }
        o = new_obj(d, t->is_int ? P_INT : P_REAL);
        if (!o) return &null_obj;
        if (t->is_int) o->u.i = t->ival; else o->u.r = t->num;
        return o;
    case T_STR:
    case T_NAME:
        o = new_obj(d, t->t == T_STR ? P_STR : P_NAME);
        if (!o) return &null_obj;
        o->u.str.s = t->s;
        o->u.str.len = t->len;
        return o;
    case T_KW:
        if (t->len == 4 && !memcmp(t->s, "true", 4)) { o = new_obj(d, P_BOOL); if (o) o->u.b = 1; return o ? o : &null_obj; }
        if (t->len == 5 && !memcmp(t->s, "false", 5)) { o = new_obj(d, P_BOOL); return o ? o : &null_obj; }
        if (t->len == 4 && !memcmp(t->s, "null", 4)) return &null_obj;
        o = new_obj(d, P_KEYWORD);
        if (!o) return &null_obj;
        o->u.str.s = t->s;
        o->u.str.len = t->len;
        return o;
    case T_ARR_OPEN: {
        pobj **v = NULL;
        int n = 0, cap = 0;
        o = new_obj(d, P_ARRAY);
        if (!o) return &null_obj;
        for (;;) {
            tok e;
            const unsigned char *save = *pp;
            if (next_tok(d, pp, end, &e) == T_EOF) break;
            if (e.t == T_ARR_CLOSE) break;
            if (e.t == T_DICT_CLOSE || (e.t == T_KW && (!strcmp((char *)e.s, "endobj") || !strcmp((char *)e.s, "stream")))) {
                *pp = save;                            /* ভাঙা array — এখানেই শেষ */
                break;
            }
            if (n == cap) {
                pobj **nv;
                cap = cap ? cap * 2 : 8;
                nv = (pobj **)pdf_alloc(d, sizeof(pobj *) * (size_t)cap);
                if (!nv) break;
                if (n) memcpy(nv, v, sizeof(pobj *) * (size_t)n);
                v = nv;
            }
            v[n++] = obj_from_tok(d, &e, pp, end, allow_ref, depth + 1);
        }
        o->u.arr.v = v;
        o->u.arr.n = n;
        return o;
    }
    case T_DICT_OPEN: {
        char **k = NULL;
        pobj **v = NULL;
        int n = 0, cap = 0;
        o = new_obj(d, P_DICT);
        if (!o) return &null_obj;
        for (;;) {
            tok kt, vt;
            const unsigned char *save = *pp;
            if (next_tok(d, pp, end, &kt) == T_EOF || kt.t == T_DICT_CLOSE) break;
            if (kt.t != T_NAME) {
                if (kt.t == T_KW && (!strcmp((char *)kt.s, "endobj") || !strcmp((char *)kt.s, "stream"))) {
                    *pp = save;
                    break;
                }
                continue;                              /* ভাঙা — বাদ দিয়ে এগোনো */
            }
            if (next_tok(d, pp, end, &vt) == T_EOF) break;
            if (vt.t == T_DICT_CLOSE) break;
            if (n == cap) {
                char **nk;
                pobj **nv;
                cap = cap ? cap * 2 : 8;
                nk = (char **)pdf_alloc(d, sizeof(char *) * (size_t)cap);
                nv = (pobj **)pdf_alloc(d, sizeof(pobj *) * (size_t)cap);
                if (!nk || !nv) break;
                if (n) { memcpy(nk, k, sizeof(char *) * (size_t)n); memcpy(nv, v, sizeof(pobj *) * (size_t)n); }
                k = nk;
                v = nv;
            }
            k[n] = (char *)kt.s;
            v[n] = obj_from_tok(d, &vt, pp, end, allow_ref, depth + 1);
            n++;
        }
        o->u.dict.k = k;
        o->u.dict.v = v;
        o->u.dict.n = n;
        return o;
    }
    default:
        return &null_obj;
    }
}

static pobj *parse_obj(pdf_doc *d, const unsigned char **pp, const unsigned char *end, int allow_ref, int depth)
{
    tok t;
    if (next_tok(d, pp, end, &t) == T_EOF) return NULL;
    return obj_from_tok(d, &t, pp, end, allow_ref, depth);
}

pobj *pdf_lex_object(pdf_lexer *lx)
{
    tok t;
    for (;;) {
        if (next_tok(lx->d, &lx->p, lx->end, &t) == T_EOF) return NULL;
        if (t.t == T_ARR_CLOSE || t.t == T_DICT_CLOSE || t.t == T_BRACE) continue;   /* ভুল চিহ্ন বাদ */
        return obj_from_tok(lx->d, &t, &lx->p, lx->end, 0, 0);
    }
}

/* ---------- dict সাহায্যকারী ---------- */

pobj *pdf_dict_raw(pobj *dict, const char *key)
{
    int i;
    if (!dict) return NULL;
    if (dict->type == P_STREAM) dict = dict->u.st.dict;
    if (!dict || dict->type != P_DICT) return NULL;
    for (i = 0; i < dict->u.dict.n; i++) {
        if (!strcmp(dict->u.dict.k[i], key)) return dict->u.dict.v[i];
    }
    return NULL;
}

pobj *pdf_get(pdf_doc *d, pobj *dict, const char *key)
{
    pobj *o = pdf_resolve(d, pdf_dict_raw(pdf_resolve(d, dict), key));
    return (o && o->type != P_NULL) ? o : NULL;
}

int pdf_is_name(pobj *o, const char *name)
{
    return o && o->type == P_NAME && !strcmp((char *)o->u.str.s, name);
}

double pdf_num(pobj *o, double def)
{
    if (!o) return def;
    if (o->type == P_INT) return (double)o->u.i;
    if (o->type == P_REAL) return o->u.r;
    return def;
}

long pdf_int(pdf_doc *d, pobj *dict, const char *key, long def)
{
    pobj *o = pdf_get(d, dict, key);
    if (!o) return def;
    if (o->type == P_INT) return o->u.i;
    if (o->type == P_REAL) return (long)o->u.r;
    return def;
}

double pdf_real(pdf_doc *d, pobj *dict, const char *key, double def)
{
    return pdf_num(pdf_get(d, dict, key), def);
}

const char *pdf_name(pdf_doc *d, pobj *dict, const char *key)
{
    pobj *o = pdf_get(d, dict, key);
    return (o && o->type == P_NAME) ? (const char *)o->u.str.s : NULL;
}

/* ---------- object পড়া ---------- */

static const unsigned char *find_bytes(const unsigned char *p, const unsigned char *end, const char *s)
{
    size_t n = strlen(s);
    while (p + n <= end) {
        const unsigned char *q = (const unsigned char *)memchr(p, s[0], (size_t)(end - p));
        if (!q || q + n > end) return NULL;
        if (!memcmp(q, s, n)) return q;
        p = q + 1;
    }
    return NULL;
}

/* p-তে "stream" keyword-এর পরে: stream object বানানো */
static pobj *make_stream(pdf_doc *d, pobj *dict, const unsigned char *p)
{
    pobj *o = new_obj(d, P_STREAM), *lo;
    const unsigned char *end = d->buf + d->len, *es;
    long len = -1;
    if (!o) return dict;
    if (p < end && *p == '\r') p++;
    if (p < end && *p == '\n') p++;
    o->u.st.dict = dict;
    o->u.st.off = (long)(p - d->buf);
    lo = pdf_dict_raw(dict, "Length");
    if (lo && lo->type == P_INT) len = lo->u.i;
    else if (lo && lo->type == P_REF) {
        pobj *r = pdf_resolve(d, lo);
        if (r && r->type == P_INT) len = r->u.i;
    }
    /* দৈর্ঘ্য যাচাই: শেষে "endstream" থাকতে হবে, নইলে খুঁজে নেওয়া */
    if (len >= 0 && o->u.st.off + len <= d->len) {
        const unsigned char *q = d->buf + o->u.st.off + len;
        while (q < end && is_ws(*q)) q++;
        if (q + 9 <= end && !memcmp(q, "endstream", 9)) {
            o->u.st.len = len;
            return o;
        }
    }
    es = find_bytes(p, end, "endstream");
    if (!es) es = end;
    len = (long)(es - p);
    while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r')) len--;
    o->u.st.len = len;
    return o;
}

/* offset-এ "n g obj ... endobj" পড়া */
static pobj *parse_indirect_at(pdf_doc *d, long off, int *num_out)
{
    const unsigned char *p = d->buf + off, *end = d->buf + d->len;
    tok a, b, c;
    pobj *o;
    if (off < 0 || off >= d->len) return NULL;
    if (next_tok(d, &p, end, &a) != T_NUM || next_tok(d, &p, end, &b) != T_NUM ||
        next_tok(d, &p, end, &c) != T_KW || strcmp((char *)c.s, "obj")) return NULL;
    if (num_out) *num_out = (int)a.ival;
    o = parse_obj(d, &p, end, 1, 0);
    if (!o) return &null_obj;
    if (o->type == P_DICT) {
        const unsigned char *q = p;
        tok s;
        if (next_tok(d, &q, end, &s) == T_KW && !strcmp((char *)s.s, "stream")) {
            return make_stream(d, o, q);
        }
    }
    return o;
}

static pobj *load_from_objstm(pdf_doc *d, int stmnum, int index, int want)
{
    pobj *st = pdf_object(d, stmnum);
    unsigned char *data;
    long len, first, n, i;
    const unsigned char *p, *end;
    if (!st || st->type != P_STREAM) return NULL;
    data = pdf_stream_data(d, st, &len, 0, NULL);
    if (!data) return NULL;
    first = pdf_int(d, st, "First", 0);
    n = pdf_int(d, st, "N", 0);
    p = data;
    end = data + len;
    for (i = 0; i < n; i++) {                          /* index-এর ভরসা না করে নম্বর মিলিয়ে */
        tok a, b;
        if (next_tok(d, &p, end, &a) != T_NUM || next_tok(d, &p, end, &b) != T_NUM) break;
        if ((int)a.ival == want || (i == index && want < 0)) {
            const unsigned char *q = data + first + b.ival;
            if (first + b.ival >= len) return NULL;
            return parse_obj(d, &q, end, 1, 0);
        }
    }
    return NULL;
}

pobj *pdf_object(pdf_doc *d, int num)
{
    pdf_xent *x;
    pobj *o = NULL;
    if (num <= 0 || num >= d->nx) return NULL;
    x = &d->x[num];
    if (x->cache) return x->cache;
    if (x->busy || !x->type) return NULL;
    x->busy = 1;
    if (x->type == 1) o = parse_indirect_at(d, x->off, NULL);
    else if (x->type == 2) o = load_from_objstm(d, (int)x->off, x->gen, num);
    x->busy = 0;
    if (!o) o = &null_obj;
    x->cache = o;
    return o;
}

pobj *pdf_resolve(pdf_doc *d, pobj *o)
{
    int guard = 0;
    while (o && o->type == P_REF && guard++ < 32) o = pdf_object(d, o->u.ref.num);
    return o;
}

/* ---------- filter ---------- */

typedef struct dbuf { unsigned char *p; long n, cap; } dbuf;

static int dput(dbuf *b, const unsigned char *s, long n)
{
    if (b->n + n > b->cap) {
        long nc = b->cap ? b->cap : 4096;
        unsigned char *np;
        while (nc < b->n + n) nc *= 2;
        np = (unsigned char *)realloc(b->p, (size_t)nc);
        if (!np) return -1;
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->n, s, (size_t)n);
    b->n += n;
    return 0;
}

static unsigned char *f_flate(const unsigned char *in, long n, long *outn)
{
    z_stream z;
    dbuf b = { 0 };
    unsigned char tmp[16384];
    int r = Z_OK;
    memset(&z, 0, sizeof(z));
    if (inflateInit(&z) != Z_OK) return NULL;
    z.next_in = (unsigned char *)in;
    z.avail_in = (uInt)n;
    while (r == Z_OK) {
        z.next_out = tmp;
        z.avail_out = sizeof(tmp);
        r = inflate(&z, Z_NO_FLUSH);
        if (dput(&b, tmp, (long)(sizeof(tmp) - z.avail_out)) < 0) break;
        if (r == Z_BUF_ERROR && z.avail_in == 0) break;
        if (r == Z_DATA_ERROR && b.n == 0) {           /* zlib হেডার ছাড়া (raw deflate)? */
            inflateEnd(&z);
            memset(&z, 0, sizeof(z));
            if (inflateInit2(&z, -15) != Z_OK) break;
            z.next_in = (unsigned char *)in;
            z.avail_in = (uInt)n;
            r = Z_OK;
        }
    }
    inflateEnd(&z);
    *outn = b.n;                                       /* ভুলেও যা পাওয়া গেছে তা-ই */
    if (!b.p) b.p = (unsigned char *)malloc(1);
    return b.p;
}

static unsigned char *f_lzw(const unsigned char *in, long n, long *outn, int early)
{
    /* টেবিল: prefix + শেষ বাইট + দৈর্ঘ্য */
    static int prefix[4096];
    static unsigned char last[4096], first[4096];
    static int lens[4096];
    dbuf b = { 0 };
    long bitpos = 0, total = n * 8;
    int next = 258, width = 9, prev = -1, i;
    unsigned char stack[4096];
    for (i = 0; i < 256; i++) { prefix[i] = -1; last[i] = (unsigned char)i; first[i] = (unsigned char)i; lens[i] = 1; }
    while (bitpos + width <= total) {
        int code = 0, k, sp = 0, c;
        for (k = 0; k < width; k++, bitpos++) code = (code << 1) | ((in[bitpos >> 3] >> (7 - (bitpos & 7))) & 1);
        if (code == 257) break;
        if (code == 256) { next = 258; width = 9; prev = -1; continue; }
        if (code < next) {
            for (c = code; c >= 0 && sp < 4096; c = prefix[c]) stack[sp++] = last[c];
            if (prev >= 0 && next < 4096) {
                prefix[next] = prev; last[next] = first[code]; first[next] = first[prev]; lens[next] = lens[prev] + 1; next++;
            }
        } else if (prev >= 0 && next < 4096) {         /* KwKwK */
            prefix[next] = prev; last[next] = first[prev]; first[next] = first[prev]; lens[next] = lens[prev] + 1; next++;
            for (c = next - 1; c >= 0 && sp < 4096; c = prefix[c]) stack[sp++] = last[c];
        } else {
            break;
        }
        while (sp) { unsigned char ch = stack[--sp]; if (dput(&b, &ch, 1) < 0) break; }
        prev = code;
        if (next + early >= (1 << width) && width < 12) width++;
    }
    *outn = b.n;
    if (!b.p) b.p = (unsigned char *)malloc(1);
    return b.p;
}

static unsigned char *f_a85(const unsigned char *in, long n, long *outn)
{
    dbuf b = { 0 };
    unsigned long v = 0;
    int k = 0;
    long i;
    if (n >= 2 && in[0] == '<' && in[1] == '~') { in += 2; n -= 2; }
    for (i = 0; i < n; i++) {
        int c = in[i];
        if (c == '~') break;
        if (is_ws(c)) continue;
        if (c == 'z' && k == 0) { unsigned char z4[4] = { 0, 0, 0, 0 }; dput(&b, z4, 4); continue; }
        if (c < '!' || c > 'u') continue;
        v = v * 85 + (unsigned long)(c - '!');
        if (++k == 5) {
            unsigned char o[4] = { (unsigned char)(v >> 24), (unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v };
            dput(&b, o, 4);
            v = 0;
            k = 0;
        }
    }
    if (k > 1) {
        int j;
        unsigned char o[4];
        for (j = k; j < 5; j++) v = v * 85 + 84;
        o[0] = (unsigned char)(v >> 24); o[1] = (unsigned char)(v >> 16); o[2] = (unsigned char)(v >> 8); o[3] = (unsigned char)v;
        dput(&b, o, k - 1);
    }
    *outn = b.n;
    if (!b.p) b.p = (unsigned char *)malloc(1);
    return b.p;
}

static unsigned char *f_ahx(const unsigned char *in, long n, long *outn)
{
    unsigned char *o = (unsigned char *)malloc((size_t)n / 2 + 2);
    long i, m = 0;
    int hi = -1;
    if (!o) return NULL;
    for (i = 0; i < n && in[i] != '>'; i++) {
        int v = hexval(in[i]);
        if (v < 0) continue;
        if (hi < 0) hi = v; else { o[m++] = (unsigned char)(hi * 16 + v); hi = -1; }
    }
    if (hi >= 0) o[m++] = (unsigned char)(hi * 16);
    *outn = m;
    return o;
}

static unsigned char *f_rl(const unsigned char *in, long n, long *outn)
{
    dbuf b = { 0 };
    long i = 0;
    while (i < n) {
        int c = in[i++];
        if (c == 128) break;
        if (c < 128) {
            long cnt = c + 1;
            if (i + cnt > n) cnt = n - i;
            dput(&b, in + i, cnt);
            i += cnt;
        } else if (i < n) {
            unsigned char rep[128];
            memset(rep, in[i++], sizeof(rep));
            dput(&b, rep, 257 - c);
        }
    }
    *outn = b.n;
    if (!b.p) b.p = (unsigned char *)malloc(1);
    return b.p;
}

/* PNG (১০–১৫) আর TIFF (২) predictor */
static unsigned char *predict(unsigned char *data, long *n, int pred, int colors, int bpc, int cols)
{
    long bpp = (colors * bpc + 7) / 8, row = (cols * colors * bpc + 7) / 8, rows, r, i;
    unsigned char *out, *prev;
    if (pred < 2 || row <= 0) return data;
    if (bpp < 1) bpp = 1;
    if (pred == 2) {                                   /* TIFF: শুধু ৮-বিট */
        if (bpc != 8) return data;
        rows = *n / row;
        for (r = 0; r < rows; r++) {
            unsigned char *p = data + r * row;
            for (i = bpp; i < row; i++) p[i] = (unsigned char)(p[i] + p[i - bpp]);
        }
        return data;
    }
    rows = *n / (row + 1);
    out = (unsigned char *)malloc((size_t)(rows * row + 1));
    prev = (unsigned char *)calloc((size_t)row + 1, 1);
    if (!out || !prev) { free(out); free(prev); return data; }
    for (r = 0; r < rows; r++) {
        unsigned char *src = data + r * (row + 1), *dst = out + r * row;
        int ft = src[0];
        src++;
        for (i = 0; i < row; i++) {
            int a = i >= bpp ? dst[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0, v = src[i];
            switch (ft) {
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) / 2; break;
            case 4: {
                int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
                v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                break;
            }
            default: break;
            }
            dst[i] = (unsigned char)v;
        }
        memcpy(prev, dst, (size_t)row);
    }
    free(prev);
    free(data);
    *n = rows * row;
    return out;
}

static const char *short_filter(const char *f)
{
    if (!strcmp(f, "Fl")) return "FlateDecode";
    if (!strcmp(f, "LZW")) return "LZWDecode";
    if (!strcmp(f, "A85")) return "ASCII85Decode";
    if (!strcmp(f, "AHx")) return "ASCIIHexDecode";
    if (!strcmp(f, "RL")) return "RunLengthDecode";
    if (!strcmp(f, "DCT")) return "DCTDecode";
    if (!strcmp(f, "CCF")) return "CCITTFaxDecode";
    return f;
}

unsigned char *pdf_decode_filters(pdf_doc *d, const unsigned char *in, long inlen, pobj *filters,
                                  pobj *parms, long *outlen, int stop_at_image, const char **last_filter)
{
    unsigned char *cur;
    long n = inlen;
    int nf = 0, i;
    pobj *fl[8], *pa[8];
    filters = pdf_resolve(d, filters);
    parms = pdf_resolve(d, parms);
    if (last_filter) *last_filter = NULL;
    if (filters && filters->type == P_NAME) { fl[0] = filters; pa[0] = parms; nf = 1; }
    else if (filters && filters->type == P_ARRAY) {
        for (i = 0; i < filters->u.arr.n && nf < 8; i++) {
            fl[nf] = pdf_resolve(d, filters->u.arr.v[i]);
            pa[nf] = (parms && parms->type == P_ARRAY && i < parms->u.arr.n) ? pdf_resolve(d, parms->u.arr.v[i]) : NULL;
            nf++;
        }
    }
    cur = (unsigned char *)malloc((size_t)n + 1);
    if (!cur) return NULL;
    memcpy(cur, in, (size_t)n);
    for (i = 0; i < nf; i++) {
        const char *f = (fl[i] && fl[i]->type == P_NAME) ? short_filter((char *)fl[i]->u.str.s) : "";
        unsigned char *nx = NULL;
        long nn = 0;
        if (!strcmp(f, "DCTDecode") || !strcmp(f, "JPXDecode") || !strcmp(f, "CCITTFaxDecode") ||
            !strcmp(f, "JBIG2Decode")) {
            if (last_filter) *last_filter = f;
            break;                                     /* ছবির কোডেক — আঁকার অংশ দেখে */
        }
        if (!strcmp(f, "FlateDecode")) nx = f_flate(cur, n, &nn);
        else if (!strcmp(f, "LZWDecode")) nx = f_lzw(cur, n, &nn, (int)pdf_int(d, pa[i], "EarlyChange", 1));
        else if (!strcmp(f, "ASCII85Decode")) nx = f_a85(cur, n, &nn);
        else if (!strcmp(f, "ASCIIHexDecode")) nx = f_ahx(cur, n, &nn);
        else if (!strcmp(f, "RunLengthDecode")) nx = f_rl(cur, n, &nn);
        else if (!strcmp(f, "Crypt")) continue;
        else { if (last_filter) *last_filter = f; break; }   /* অচেনা */
        free(cur);
        if (!nx) return NULL;
        cur = nx;
        n = nn;
        if ((!strcmp(f, "FlateDecode") || !strcmp(f, "LZWDecode")) && pa[i]) {
            int pred = (int)pdf_int(d, pa[i], "Predictor", 1);
            if (pred > 1) {
                cur = predict(cur, &n, pred, (int)pdf_int(d, pa[i], "Colors", 1),
                              (int)pdf_int(d, pa[i], "BitsPerComponent", 8), (int)pdf_int(d, pa[i], "Columns", 1));
            }
        }
    }
    (void)stop_at_image;
    *outlen = n;
    return cur;
}

unsigned char *pdf_stream_data(pdf_doc *d, pobj *st, long *len, int stop_at_image, const char **last_filter)
{
    unsigned char *raw, *out;
    long n;
    if (!st || st->type != P_STREAM) return NULL;
    if (st->u.st.decoded && !stop_at_image) {
        *len = st->u.st.dlen;
        if (last_filter) *last_filter = NULL;
        return st->u.st.data;
    }
    raw = d->buf + st->u.st.off;
    if (st->u.st.off + st->u.st.len > d->len) return NULL;
    out = pdf_decode_filters(d, raw, st->u.st.len, pdf_dict_raw(st, "Filter"), pdf_dict_raw(st, "DecodeParms"),
                             &n, stop_at_image, last_filter);
    if (!out) return NULL;
    if (!stop_at_image) {                              /* arena-য় রেখে দেওয়া (content, font, objstm) */
        unsigned char *keep = (unsigned char *)pdf_alloc(d, (size_t)n + 1);
        if (keep) {
            memcpy(keep, out, (size_t)n);
            free(out);
            st->u.st.data = keep;
            st->u.st.dlen = n;
            st->u.st.decoded = 1;
            *len = n;
            return keep;
        }
        free(out);
        return NULL;
    }
    /* ছবি: caller ছাড়বে না — তাই arena-য় নয়, malloc-এ রেখে stream-এ নয়; এখানে arena-য় কপি */
    {
        unsigned char *keep = (unsigned char *)pdf_alloc(d, (size_t)n + 1);
        if (keep) memcpy(keep, out, (size_t)n);
        free(out);
        *len = n;
        return keep;
    }
}

/* ---------- xref ---------- */

static int ensure_nx(pdf_doc *d, int n)
{
    if (n <= d->nx) return 0;
    if (n > 8000000) return -1;
    {
        pdf_xent *nx = (pdf_xent *)realloc(d->x, sizeof(pdf_xent) * (size_t)n);
        if (!nx) return -1;
        memset(nx + d->nx, 0, sizeof(pdf_xent) * (size_t)(n - d->nx));
        d->x = nx;
        d->nx = n;
    }
    return 0;
}

static void set_x(pdf_doc *d, int num, char type, long off, int gen, int overwrite)
{
    if (num <= 0 || ensure_nx(d, num + 1) < 0) return;
    if (d->x[num].type && !overwrite) return;          /* নতুন অংশ আগে পড়া — সেটাই থাকে */
    d->x[num].type = type;
    d->x[num].off = off;
    d->x[num].gen = gen;
    d->x[num].cache = NULL;
}

static void merge_trailer(pdf_doc *d, pobj *t)
{
    int i;
    if (!t || t->type != P_DICT) return;
    if (!d->trailer) { d->trailer = t; return; }
    for (i = 0; i < t->u.dict.n; i++) {                /* পুরোনোর key নতুনে না থাকলে যোগ */
        if (!pdf_dict_raw(d->trailer, t->u.dict.k[i])) {
            pobj *tr = d->trailer;
            char **k = (char **)pdf_alloc(d, sizeof(char *) * (size_t)(tr->u.dict.n + 1));
            pobj **v = (pobj **)pdf_alloc(d, sizeof(pobj *) * (size_t)(tr->u.dict.n + 1));
            if (!k || !v) return;
            memcpy(k, tr->u.dict.k, sizeof(char *) * (size_t)tr->u.dict.n);
            memcpy(v, tr->u.dict.v, sizeof(pobj *) * (size_t)tr->u.dict.n);
            k[tr->u.dict.n] = t->u.dict.k[i];
            v[tr->u.dict.n] = t->u.dict.v[i];
            tr->u.dict.k = k;
            tr->u.dict.v = v;
            tr->u.dict.n++;
        }
    }
}

static int read_xref_stream(pdf_doc *d, long off, long *prev)
{
    int num;
    pobj *st = parse_indirect_at(d, off, &num), *w, *idx;
    unsigned char *data;
    long len, pos = 0;
    int W[3], i, k;
    if (!st || st->type != P_STREAM || !pdf_is_name(pdf_get(d, st, "Type"), "XRef")) return -1;
    w = pdf_get(d, st, "W");
    if (!w || w->type != P_ARRAY || w->u.arr.n < 3) return -1;
    for (i = 0; i < 3; i++) W[i] = (int)pdf_num(w->u.arr.v[i], 0);
    if (W[0] < 0 || W[1] < 0 || W[2] < 0 || W[0] > 8 || W[1] > 8 || W[2] > 8) return -1;
    data = pdf_stream_data(d, st, &len, 0, NULL);
    if (!data) return -1;
    idx = pdf_get(d, st, "Index");
    {
        long size = pdf_int(d, st, "Size", 0);
        int nsub = (idx && idx->type == P_ARRAY) ? idx->u.arr.n / 2 : 1;
        for (k = 0; k < nsub; k++) {
            long start = idx && idx->type == P_ARRAY ? (long)pdf_num(idx->u.arr.v[2 * k], 0) : 0;
            long count = idx && idx->type == P_ARRAY ? (long)pdf_num(idx->u.arr.v[2 * k + 1], 0) : size;
            long j;
            for (j = 0; j < count; j++) {
                long f[3];
                int c;
                if (pos + W[0] + W[1] + W[2] > len) break;
                for (c = 0; c < 3; c++) {
                    int b;
                    f[c] = 0;
                    for (b = 0; b < W[c]; b++) f[c] = (f[c] << 8) | data[pos++];
                }
                if (W[0] == 0) f[0] = 1;
                if (f[0] == 1) set_x(d, (int)(start + j), 1, f[1], (int)f[2], 0);
                else if (f[0] == 2) set_x(d, (int)(start + j), 2, f[1], (int)f[2], 0);
                else if (f[0] == 0 && start + j > 0 && ensure_nx(d, (int)(start + j) + 1) == 0 && !d->x[start + j].type)
                    d->x[start + j].type = 3;          /* মুক্ত — পুরোনো অংশ যেন না বসায় */
            }
        }
    }
    merge_trailer(d, st->u.st.dict);
    *prev = pdf_int(d, st, "Prev", -1);
    return 0;
}

static int read_xref_at(pdf_doc *d, long off, int depth)
{
    const unsigned char *p, *end = d->buf + d->len;
    tok t;
    long prev = -1;
    if (depth > 64 || off <= 0 || off >= d->len) return -1;
    p = d->buf + off;
    if (next_tok(d, &p, end, &t) == T_KW && !strcmp((char *)t.s, "xref")) {
        pobj *tr;
        for (;;) {
            tok a, b;
            const unsigned char *save = p;
            long j;
            if (next_tok(d, &p, end, &a) != T_NUM) { p = save; break; }
            if (next_tok(d, &p, end, &b) != T_NUM) return -1;
            for (j = 0; j < b.ival; j++) {
                tok o, g, f;
                if (next_tok(d, &p, end, &o) != T_NUM || next_tok(d, &p, end, &g) != T_NUM ||
                    next_tok(d, &p, end, &f) != T_KW) return -1;
                if (f.s[0] == 'n' && o.ival > 0) set_x(d, (int)(a.ival + j), 1, o.ival, (int)g.ival, 0);
                else if (f.s[0] == 'f' && a.ival + j > 0 && ensure_nx(d, (int)(a.ival + j) + 1) == 0 &&
                         !d->x[a.ival + j].type) d->x[a.ival + j].type = 3;
            }
        }
        if (next_tok(d, &p, end, &t) != T_KW || strcmp((char *)t.s, "trailer")) return -1;
        tr = parse_obj(d, &p, end, 1, 0);
        if (!tr || tr->type != P_DICT) return -1;
        {
            pobj *xs = pdf_dict_raw(tr, "XRefStm");   /* hybrid ফাইল */
            long xp;
            if (xs && xs->type == P_INT) read_xref_stream(d, xs->u.i, &xp);
        }
        merge_trailer(d, tr);
        {
            pobj *pv = pdf_dict_raw(tr, "Prev");
            prev = pv ? (long)pdf_num(pv, -1) : -1;
        }
    } else if (read_xref_stream(d, off, &prev) < 0) {
        return -1;
    }
    if (prev > 0 && prev != off) return read_xref_at(d, prev, depth + 1) < 0 ? 1 : 0;   /* পুরোনো ভাঙা হলেও চলে */
    return 0;
}

/* ভাঙা ফাইল: "n g obj" খুঁজে সব object, trailer/Catalog খুঁজে Root */
static int repair(pdf_doc *d)
{
    const unsigned char *p = d->buf, *end = d->buf + d->len;
    int i;
    for (i = 0; i < d->nx; i++) d->x[i].type = 0, d->x[i].cache = NULL;
    d->trailer = NULL;
    while ((p = find_bytes(p, end, "obj")) != NULL) {
        const unsigned char *q = p;
        long gen = 0, num = 0, mul = 1;
        if (p + 3 < end && !is_ws(p[3]) && !is_delim(p[3])) { p += 3; continue; }   /* "objects" ইত্যাদি */
        q--;
        while (q > d->buf && is_ws(*q)) q--;
        while (q >= d->buf && *q >= '0' && *q <= '9') { gen += (*q - '0') * mul; mul *= 10; q--; }
        if (mul == 1 || q < d->buf || !is_ws(*q)) { p += 3; continue; }
        while (q > d->buf && is_ws(*q)) q--;
        mul = 1;
        while (q >= d->buf && *q >= '0' && *q <= '9') { num += (*q - '0') * mul; mul *= 10; q--; }
        if (mul == 1) { p += 3; continue; }
        if (num > 0 && num < 8000000) set_x(d, (int)num, 1, (long)(q + 1 - d->buf), (int)gen, 1);
        p += 3;
    }
    /* trailer */
    p = d->buf;
    while ((p = find_bytes(p, end, "trailer")) != NULL) {
        const unsigned char *q = p + 7;
        pobj *t = parse_obj(d, &q, end, 1, 0);
        if (t && t->type == P_DICT && pdf_dict_raw(t, "Root")) d->trailer = t;   /* শেষেরটা */
        p += 7;
    }
    /* object stream-এর ভেতরের object আর Catalog */
    for (i = 1; i < d->nx; i++) {
        pobj *o;
        if (d->x[i].type != 1) continue;
        o = pdf_object(d, i);
        if (o && o->type == P_STREAM && pdf_is_name(pdf_get(d, o, "Type"), "ObjStm")) {
            long len, n = pdf_int(d, o, "N", 0), k;
            unsigned char *data = pdf_stream_data(d, o, &len, 0, NULL);
            const unsigned char *s = data, *e = data + len;
            for (k = 0; data && k < n; k++) {
                tok a, b;
                if (next_tok(d, &s, e, &a) != T_NUM || next_tok(d, &s, e, &b) != T_NUM) break;
                if (a.ival > 0 && a.ival < d->nx + 1000000 && (ensure_nx(d, (int)a.ival + 1) == 0) && !d->x[a.ival].type)
                    set_x(d, (int)a.ival, 2, i, (int)k, 0);
            }
        }
    }
    if (!d->trailer || !pdf_get(d, d->trailer, "Root")) {
        for (i = 1; i < d->nx; i++) {
            pobj *o = d->x[i].type ? pdf_object(d, i) : NULL;
            if (o && o->type == P_DICT && pdf_is_name(pdf_get(d, o, "Type"), "Catalog")) {
                pobj *t = new_obj(d, P_DICT), *r = new_obj(d, P_REF);
                char **k = (char **)pdf_alloc(d, sizeof(char *));
                pobj **v = (pobj **)pdf_alloc(d, sizeof(pobj *));
                if (!t || !r || !k || !v) return -1;
                r->u.ref.num = i;
                k[0] = (char *)"Root";
                v[0] = r;
                t->u.dict.k = k;
                t->u.dict.v = v;
                t->u.dict.n = 1;
                d->trailer = t;
                break;
            }
        }
    }
    d->repaired = 1;
    if (!d->trailer) {                                 /* Catalog নেই: খালি trailer — পাতা খুঁজে নেওয়া হবে */
        d->trailer = new_obj(d, P_DICT);
        if (!d->trailer) return -1;
    }
    return 0;
}

/* ---------- পাতা ---------- */

static int add_page(pdf_doc *d, pobj *pg, pobj *res, pobj *mbox, pobj *cbox, int rot, int *cap)
{
    pdf_page *p;
    pobj *box = cbox ? cbox : mbox;
    int i;
    if (d->npages == *cap) {
        pdf_page *np;
        *cap = *cap ? *cap * 2 : 16;
        np = (pdf_page *)realloc(d->pages, sizeof(pdf_page) * (size_t)*cap);
        if (!np) return -1;
        d->pages = np;
    }
    p = &d->pages[d->npages++];
    p->dict = pg;
    p->resources = res;
    p->box[0] = 0; p->box[1] = 0; p->box[2] = 612; p->box[3] = 792;   /* Letter */
    if (box && box->type == P_ARRAY && box->u.arr.n >= 4) {
        double v[4], t;
        for (i = 0; i < 4; i++) v[i] = pdf_num(pdf_resolve(d, box->u.arr.v[i]), 0);
        if (v[0] > v[2]) { t = v[0]; v[0] = v[2]; v[2] = t; }
        if (v[1] > v[3]) { t = v[1]; v[1] = v[3]; v[3] = t; }
        if (v[2] - v[0] >= 1 && v[3] - v[1] >= 1) memcpy(p->box, v, sizeof(v));
    }
    rot = ((rot % 360) + 360) % 360;
    p->rotate = (rot / 90) * 90;
    return 0;
}

static void walk_pages(pdf_doc *d, pobj *node, pobj *res, pobj *mbox, pobj *cbox, int rot, int depth, int *cap, int *budget)
{
    pobj *kids, *o;
    int i;
    node = pdf_resolve(d, node);
    if (!node || node->type != P_DICT || depth > 64 || --*budget < 0) return;
    if ((o = pdf_get(d, node, "Resources"))) res = o;
    if ((o = pdf_get(d, node, "MediaBox"))) mbox = o;
    if ((o = pdf_get(d, node, "CropBox"))) cbox = o;
    if ((o = pdf_get(d, node, "Rotate"))) rot = (int)pdf_num(o, rot);
    kids = pdf_get(d, node, "Kids");
    if (pdf_is_name(pdf_get(d, node, "Type"), "Page") || (!kids && pdf_get(d, node, "Contents"))) {
        add_page(d, node, res, mbox, cbox, rot, cap);
        return;
    }
    if (!kids || kids->type != P_ARRAY) return;
    for (i = 0; i < kids->u.arr.n; i++) walk_pages(d, kids->u.arr.v[i], res, mbox, cbox, rot, depth + 1, cap, budget);
}

static int load_pages(pdf_doc *d)
{
    pobj *root = pdf_get(d, d->trailer, "Root");
    int cap = 0, budget = 2000000, i;
    if (root) walk_pages(d, pdf_get(d, root, "Pages"), NULL, NULL, NULL, 0, 0, &cap, &budget);
    if (d->npages == 0) {                              /* গাছ ভাঙা: সব /Type /Page ক্রমে */
        for (i = 1; i < d->nx; i++) {
            pobj *o = d->x[i].type ? pdf_object(d, i) : NULL;
            if (o && o->type == P_DICT && pdf_is_name(pdf_get(d, o, "Type"), "Page")) {
                pobj *par = pdf_get(d, o, "Parent");
                add_page(d, o, pdf_get(d, o, "Resources") ? pdf_get(d, o, "Resources") : pdf_get(d, par, "Resources"),
                         pdf_get(d, o, "MediaBox") ? pdf_get(d, o, "MediaBox") : pdf_get(d, par, "MediaBox"),
                         pdf_get(d, o, "CropBox"), (int)pdf_int(d, o, "Rotate", 0), &cap);
            }
        }
    }
    return d->npages > 0 ? 0 : -1;
}

/* ---------- খোলা/বন্ধ ---------- */

pdf_doc *pdf_open(const char *path, char *err, int errsz)
{
    pdf_doc *d;
    struct stat st;
    int fd;
    long got = 0, sx = -1, i;
    const unsigned char *hdr;

    if (stat(path, &st) < 0 || (fd = open(path, O_RDONLY)) < 0) {
        snprintf(err, (size_t)errsz, "cannot open %s", path);
        return NULL;
    }
    d = (pdf_doc *)calloc(1, sizeof(pdf_doc));
    if (!d || !(d->buf = (unsigned char *)malloc((size_t)st.st_size + 16))) {
        free(d);
        close(fd);
        snprintf(err, (size_t)errsz, "out of memory (%ld bytes)", (long)st.st_size);
        return NULL;
    }
    while (got < st.st_size) {
        ssize_t r = read(fd, d->buf + got, (size_t)(st.st_size - got));
        if (r <= 0) break;
        got += r;
    }
    close(fd);
    d->len = got;
    memset(d->buf + got, 0, 16);
    hdr = find_bytes(d->buf, d->buf + (got < 1024 ? got : 1024), "%PDF-");
    if (!hdr) {
        snprintf(err, (size_t)errsz, "not a PDF file");
        pdf_close(d);
        return NULL;
    }
    snprintf(d->version, sizeof(d->version), "%.3s", (const char *)hdr + 5);
    if (hdr > d->buf) {                                /* শুরুতে আবর্জনা: offset সেই হিসেবে */
        long shift = (long)(hdr - d->buf);
        memmove(d->buf, hdr, (size_t)(got - shift));
        d->len = got - shift;
    }
    for (i = d->len - 9; i >= 0 && i > d->len - 4096; i--) {
        if (!memcmp(d->buf + i, "startxref", 9)) {
            const unsigned char *p = d->buf + i + 9;
            tok t;
            if (next_tok(d, &p, d->buf + d->len, &t) == T_NUM) sx = t.ival;
            break;
        }
    }
    if (sx <= 0 || read_xref_at(d, sx, 0) < 0 || !d->trailer || !pdf_get(d, d->trailer, "Root")) {
        if (repair(d) < 0 || d->nx == 0) {
            snprintf(err, (size_t)errsz, "damaged PDF (no catalog found)");
            pdf_close(d);
            return NULL;
        }
    }
    if (pdf_dict_raw(d->trailer, "Encrypt")) {
        d->encrypted = 1;
        snprintf(err, (size_t)errsz, "this PDF is encrypted (password protected) - not supported yet");
        pdf_close(d);
        return NULL;
    }
    if (load_pages(d) < 0 && !d->repaired) {
        repair(d);
        load_pages(d);
    }
    if (d->npages == 0) {
        snprintf(err, (size_t)errsz, "no pages found");
        pdf_close(d);
        return NULL;
    }
    return d;
}

void pdf_close(pdf_doc *d)
{
    struct arena_blk *b, *n;
    if (!d) return;
    pdf_render_forget(d);
    for (b = d->arena; b; b = n) { n = b->next; free(b); }
    free(d->x);
    free(d->pages);
    free(d->buf);
    free(d);
}

int pdf_utf8(uint32_t c, char *o)
{
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | (c >> 6)); o[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) { o[0] = (char)(0xE0 | (c >> 12)); o[1] = (char)(0x80 | ((c >> 6) & 63)); o[2] = (char)(0x80 | (c & 63)); return 3; }
    o[0] = (char)(0xF0 | (c >> 18)); o[1] = (char)(0x80 | ((c >> 12) & 63)); o[2] = (char)(0x80 | ((c >> 6) & 63));
    o[3] = (char)(0x80 | (c & 63));
    return 4;
}
