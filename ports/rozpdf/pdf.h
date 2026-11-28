/* rozpdf — PDF পড়ার ভেতরের অংশ (ধাপ ২৬খ-১)
 *
 *   pdfparse.c  : lexer, object, xref (টেবিল + stream, /Prev, object stream), ভাঙা ফাইলে
 *                 "n g obj" খুঁজে নতুন xref, stream-এর filter (Flate, LZW, ASCII85, ASCIIHex,
 *                 RunLength; PNG/TIFF predictor), পাতার গাছ।
 *   pdfrender.c : content stream চালানো — path (anti-aliased), লেখা (PDF-এর নিজের ফন্ট
 *                 FreeType দিয়ে; না থাকলে rozOS-এর ফন্ট), ছবি (JPEG, raw, Indexed, mask),
 *                 Form XObject, Type3 ফন্ট; লেখা বের করা (খোঁজা/কপির জন্য)।
 *   rozpdf.c    : উইন্ডো (rozui) আর কমান্ড-লাইন (-t লেখা, -i তথ্য, -r PNG)।
 *
 * মেমোরি: একটা দলিলের সব object একটা arena-য়; pdf_close সব একসাথে ছাড়ে। */
#ifndef ROZPDF_PDF_H
#define ROZPDF_PDF_H

#include <stddef.h>
#include <stdint.h>

enum {
    P_NULL, P_BOOL, P_INT, P_REAL, P_STR, P_NAME, P_ARRAY, P_DICT, P_REF, P_STREAM,
    P_KEYWORD                         /* content stream-এর operator ("Tj", "re" ...) */
};

typedef struct pobj pobj;
struct pobj {
    int type;
    union {
        int b;
        long i;
        double r;
        struct { unsigned char *s; int len; } str;       /* P_STR, P_NAME, P_KEYWORD (শেষে '\0') */
        struct { pobj **v; int n; } arr;
        struct { char **k; pobj **v; int n; } dict;
        struct { int num, gen; } ref;
        struct { pobj *dict; long off; long len; unsigned char *data; long dlen; int decoded; } st;
    } u;
};

typedef struct pdf_xent {
    long off;                         /* type 1: ফাইলের offset; type 2: object stream-এর নম্বর */
    int gen;                          /* type 2: stream-এর ভেতরের index */
    char type;                        /* 0 = নেই, 1 = সাধারণ, 2 = object stream-এ */
    pobj *cache;
    char busy;                        /* লুপ ধরা */
} pdf_xent;

typedef struct pdf_page {
    pobj *dict;
    pobj *resources;
    double box[4];                    /* CropBox (না থাকলে MediaBox) */
    int rotate;                       /* 0/90/180/270 */
} pdf_page;

typedef struct pdf_doc {
    unsigned char *buf;               /* পুরো ফাইল মেমোরিতে */
    long len;
    pdf_xent *x;
    int nx;
    pobj *trailer;
    pdf_page *pages;
    int npages;
    int encrypted;
    int repaired;                     /* xref ভাঙা ছিল, খুঁজে বানানো */
    char version[8];
    /* arena */
    struct arena_blk *arena;
    char err[160];
} pdf_doc;

/* ---------- pdfparse.c ---------- */
pdf_doc *pdf_open(const char *path, char *err, int errsz);   /* NULL হলে err-এ কারণ */
void     pdf_close(pdf_doc *d);
void    *pdf_alloc(pdf_doc *d, size_t n);                    /* arena থেকে, শূন্য-করা */

pobj *pdf_resolve(pdf_doc *d, pobj *o);                      /* ref হলে আসল object */
pobj *pdf_get(pdf_doc *d, pobj *dict, const char *key);      /* resolve-সহ; না থাকলে NULL */
pobj *pdf_dict_raw(pobj *dict, const char *key);             /* resolve ছাড়া */
int   pdf_is_name(pobj *o, const char *name);
double pdf_num(pobj *o, double def);                          /* INT/REAL → double */
long  pdf_int(pdf_doc *d, pobj *dict, const char *key, long def);
double pdf_real(pdf_doc *d, pobj *dict, const char *key, double def);
const char *pdf_name(pdf_doc *d, pobj *dict, const char *key);   /* NAME-এর লেখা, নইলে NULL */
pobj *pdf_object(pdf_doc *d, int num);                        /* নম্বর দিয়ে */

/* stream-এর filter খুলে ডেটা (stream-এর ভেতরে রাখা থাকে; ছাড়তে হয় না)।
 * stop_at_image = 1: শেষ filter DCTDecode/JPXDecode হলে সেখানে থামে (*last_filter-এ নাম) */
unsigned char *pdf_stream_data(pdf_doc *d, pobj *st, long *len, int stop_at_image, const char **last_filter);
/* আলাদা বাফারে filter খোলা (inline image-এর জন্য): ফেরত malloc করা */
unsigned char *pdf_decode_filters(pdf_doc *d, const unsigned char *in, long inlen, pobj *filters,
                                  pobj *parms, long *outlen, int stop_at_image, const char **last_filter);

/* content stream পড়া: পরের token/object; শেষ হলে NULL */
typedef struct pdf_lexer {
    pdf_doc *d;
    const unsigned char *p, *end;
} pdf_lexer;
pobj *pdf_lex_object(pdf_lexer *lx);                          /* KEYWORD-ও ফেরত দেয় */

/* ---------- pdfrender.c ---------- */
typedef struct pdf_textitem {         /* খোঁজা/কপির জন্য পাতার লেখা */
    uint32_t ch;                      /* Unicode (০ = অজানা) */
    float x0, y0, x1, y1;             /* device পিক্সেলে (render-এর scale-এ) */
} pdf_textitem;

typedef struct pdf_render_opts {
    double scale;                     /* ১.০ = ৭২ dpi (১ pt = ১ px) */
    int want_text;                    /* textitem জমাবে */
    int no_draw;                      /* শুধু লেখা বের করা (দ্রুত) */
} pdf_render_opts;

/* পাতা আঁকা: নতুন surface (সাদা পটভূমি); text/ntext ঐচ্ছিক (malloc করা, caller ছাড়ে) */
struct gfx_surface;
struct gfx_surface *pdf_render_page(pdf_doc *d, int page, const pdf_render_opts *o,
                                     pdf_textitem **text, int *ntext);
void pdf_page_size(pdf_doc *d, int page, double scale, int *w, int *h);
void pdf_render_init(void);                                    /* FreeType একবার */
void pdf_render_forget(pdf_doc *d);                            /* দলিল বন্ধের আগে ফন্ট cache খালি */
const char *pdf_render_warning(void);                          /* শেষ render-এর সতর্কবার্তা (ফন্ট নেই ...) */

/* UTF-8 লেখা (ch, n) → buf */
int pdf_utf8(uint32_t ch, char *out);

#endif
