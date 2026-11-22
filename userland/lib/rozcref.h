/* rozOS — rozcref: C সহায়িকার ডেটা পড়া (ধাপ ১৯ক)
 *
 * সহায়িকা থাকে /usr/share/cref-এর .ref ফাইলে (উৎস: docs/cref)। প্রতিটা ফাইলে অনেকগুলো এন্ট্রি:
 *
 *   @entry printf                 নাম (ফাংশন বা বিষয়)
 *   @also vprintf                 একই পাতায় আরও নাম (খুঁজলে এই পাতাই আসে)
 *   @header stdio.h               হেডার; ভাষার বিষয়ে "language"
 *   @syntax int printf(const char *format, ...);     (একাধিক লাইন হতে পারে)
 *   @summary Print formatted text.
 *   @desc                         এর পরের লাইনগুলো @ পর্যন্ত — ব্যাখ্যা
 *   @returns ...
 *   @see fprintf sprintf
 *   @check run | compile | none   যাচাই-টুল উদাহরণটা কী করবে (ডিফল্ট run)
 *   @input ...                    উদাহরণ চালানোর সময় stdin-এ যাবে (একাধিক লাইন)
 *   @example                      এর পরের লাইনগুলো @end পর্যন্ত — পুরো চালানো যায় এমন প্রোগ্রাম
 *   @end
 *
 * ব্যবহার করে: cref (টার্মিনাল), crefview (উইন্ডো), Code আর edit-এর Ctrl+F1। */
#ifndef ROZCREF_H
#define ROZCREF_H

#ifdef __cplusplus
extern "C" {
#endif

#define CREF_DIR "/usr/share/cref"

typedef struct cref_entry {
    char *name;
    char *also;          /* স্পেস দিয়ে আলাদা নাম, না থাকলে "" */
    char *header;        /* "stdio.h", "language" ... */
    char *syntax;        /* একাধিক লাইন '\n' দিয়ে */
    char *summary;
    char *desc;
    char *returns;
    char *see;
    char *example;
    char *check;         /* "run", "compile", "none" */
    char *input;
    const char *file;    /* কোন .ref ফাইল থেকে */
    int seq;             /* পড়ার ক্রম (ভাষার বিষয় এই ক্রমেই দেখানো হয়) */
} cref_entry;

int  cref_load(const char *dir);                  /* NULL = CREF_DIR; এন্ট্রির সংখ্যা, ব্যর্থ হলে −1 */
int  cref_load_file(const char *path);            /* একটা ফাইল যোগ; এন্ট্রির সংখ্যা */
int  cref_count(void);
const cref_entry *cref_at(int i);                 /* হেডার, তারপর নাম অনুযায়ী সাজানো */
const cref_entry *cref_find(const char *name);    /* নাম বা @also; না পেলে NULL */
/* name/summary/desc/syntax-এ word আছে এমন এন্ট্রি (নামে মিল আগে); পাওয়া সংখ্যা */
int  cref_search(const char *word, const cref_entry **out, int max);
/* পড়ার মতো লেখা (প্রতিটা লাইন ≤ width অক্ষর); malloc করা — free করো */
char *cref_format(const cref_entry *e, int width);
/* হেডারের তালিকা (সাজানো); সংখ্যা ফেরত */
int  cref_headers(const char **out, int max);

#ifdef __cplusplus
}
#endif

#endif
