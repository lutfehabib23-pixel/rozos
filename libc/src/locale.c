/* rozlibc — locale, langinfo ও iconv (ধাপ ১৬ক)
 *
 * locale: "C", "POSIX", "" (পরিবেশ: LC_ALL → LC_* → LANG) আর যেকোনো "…UTF-8"/"…utf8"
 * নাম গ্রহণ; সংখ্যা/তারিখের নিয়ম সবসময় C-র, এনকোডিং সবসময় UTF-8।
 * iconv: প্রতিটা অক্ষর code point-এ পড়ে আবার লেখা; ভুল ইনপুটে EILSEQ, জায়গা না থাকলে
 * E2BIG, অসম্পূর্ণ শেষে EINVAL — POSIX-এর মতো। //IGNORE = ভুল বাদ, //TRANSLIT = '?'। */

#include <locale.h>
#include <langinfo.h>
#include <iconv.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <limits.h>
#include <wchar.h>

/* ---------- locale ---------- */

static char cur_names[LC_ALL][32] = { "C", "C", "C", "C", "C", "C" };
static char all_name[200];

static int name_ok(const char *n)
{
    size_t l = strlen(n);
    if (!strcmp(n, "C") || !strcmp(n, "POSIX")) return 1;
    if (l >= 5 && (!strcasecmp(n + l - 5, "UTF-8") || !strcasecmp(n + l - 4, "utf8"))) return 1;
    if (strchr(n, '@')) {                       /* bn_BD.UTF-8@modifier */
        const char *dot = strchr(n, '.');
        return dot && (!strncasecmp(dot + 1, "UTF-8", 5) || !strncasecmp(dot + 1, "utf8", 4));
    }
    return 0;
}

static const char *env_name(int cat)
{
    static const char *const vars[LC_ALL] = { "LC_CTYPE", "LC_NUMERIC", "LC_TIME", "LC_COLLATE",
                                              "LC_MONETARY", "LC_MESSAGES" };
    const char *v = getenv("LC_ALL");
    if (v && *v) return v;
    v = getenv(vars[cat]);
    if (v && *v) return v;
    v = getenv("LANG");
    if (v && *v) return v;
    return "C";
}

char *setlocale(int cat, const char *name)
{
    int i;
    if (cat < 0 || cat > LC_ALL) return NULL;
    if (name) {
        if (cat == LC_ALL) {
            const char *n[LC_ALL];
            for (i = 0; i < LC_ALL; i++) {
                n[i] = *name ? name : env_name(i);
                if (!name_ok(n[i])) n[i] = *name ? NULL : "C";
                if (!n[i]) return NULL;
            }
            for (i = 0; i < LC_ALL; i++) {
                strncpy(cur_names[i], n[i], sizeof(cur_names[i]) - 1);
                cur_names[i][sizeof(cur_names[i]) - 1] = '\0';
            }
        } else {
            const char *n = *name ? name : env_name(cat);
            if (!name_ok(n)) {
                if (*name) return NULL;
                n = "C";
            }
            strncpy(cur_names[cat], n, sizeof(cur_names[cat]) - 1);
            cur_names[cat][sizeof(cur_names[cat]) - 1] = '\0';
        }
    }
    if (cat != LC_ALL) return cur_names[cat];
    for (i = 1; i < LC_ALL; i++) if (strcmp(cur_names[i], cur_names[0])) break;
    if (i == LC_ALL) return cur_names[0];
    /* আলাদা আলাদা হলে glibc-র মতো "LC_CTYPE=…;LC_NUMERIC=…" */
    {
        static const char *const nm[LC_ALL] = { "LC_CTYPE", "LC_NUMERIC", "LC_TIME", "LC_COLLATE",
                                                "LC_MONETARY", "LC_MESSAGES" };
        size_t o = 0;
        all_name[0] = '\0';
        for (i = 0; i < LC_ALL && o + 48 < sizeof(all_name); i++) {
            size_t a = strlen(nm[i]), b = strlen(cur_names[i]);
            if (i) all_name[o++] = ';';
            memcpy(all_name + o, nm[i], a); o += a;
            all_name[o++] = '=';
            memcpy(all_name + o, cur_names[i], b); o += b;
            all_name[o] = '\0';
        }
        return all_name;
    }
}

struct lconv *localeconv(void)
{
    static struct lconv lc = {
        (char *)".", (char *)"", (char *)"",
        (char *)"", (char *)"", (char *)"", (char *)"", (char *)"", (char *)"", (char *)"",
        CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX,
        CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX,
    };
    return &lc;
}

/* locale_t: আসলে কোনো অবস্থা নেই — নাম না মিললে ব্যর্থ, নইলে একটা চিহ্ন */
struct __rozlibc_locale { int dummy; };
static struct __rozlibc_locale the_locale;
static locale_t current = LC_GLOBAL_LOCALE;

locale_t newlocale(int mask, const char *name, locale_t base)
{
    (void)mask;
    (void)base;
    if (!name || (*name && !name_ok(name))) { errno = ENOENT; return NULL; }
    return &the_locale;
}

locale_t duplocale(locale_t l) { (void)l; return &the_locale; }
void freelocale(locale_t l) { (void)l; }

locale_t uselocale(locale_t l)
{
    locale_t old = current;
    if (l) current = l;
    return old;
}

/* ---------- nl_langinfo ---------- */

static const char *const abday[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const day[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday",
                                    "Friday", "Saturday" };
static const char *const abmon[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug",
                                       "Sep", "Oct", "Nov", "Dec" };
static const char *const mon[12] = { "January", "February", "March", "April", "May", "June",
                                     "July", "August", "September", "October", "November",
                                     "December" };

char *nl_langinfo(nl_item item)
{
    if (item >= ABDAY_1 && item <= ABDAY_7) return (char *)abday[item - ABDAY_1];
    if (item >= DAY_1 && item <= DAY_7) return (char *)day[item - DAY_1];
    if (item >= ABMON_1 && item <= ABMON_12) return (char *)abmon[item - ABMON_1];
    if (item >= MON_1 && item <= MON_12) return (char *)mon[item - MON_1];
    switch (item) {
    case CODESET:    return (char *)"UTF-8";
    case RADIXCHAR:  return (char *)".";
    case THOUSEP:    return (char *)"";
    case D_T_FMT:    return (char *)"%a %b %e %H:%M:%S %Y";
    case D_FMT:      return (char *)"%m/%d/%y";
    case T_FMT:      return (char *)"%H:%M:%S";
    case T_FMT_AMPM: return (char *)"%I:%M:%S %p";
    case AM_STR:     return (char *)"AM";
    case PM_STR:     return (char *)"PM";
    case YESEXPR:    return (char *)"^[yY]";
    case NOEXPR:     return (char *)"^[nN]";
    case CRNCYSTR:   return (char *)"-";
    }
    return (char *)"";
}

char *nl_langinfo_l(nl_item item, locale_t l)
{
    (void)l;
    return nl_langinfo(item);
}

/* ---------- iconv ---------- */

enum { CS_UTF8 = 1, CS_ASCII, CS_LATIN1, CS_UTF16, CS_UTF16LE, CS_UTF16BE, CS_UTF32, CS_UTF32LE,
       CS_UTF32BE, CS_WCHAR, CS_CP1252 };

struct iconv_state {
    int from, to;
    int ignore, translit;
    int from_bom_done, to_bom_done;
    int from_be;                 /* UTF-16/32 BOM দেখে ঠিক হয় (ডিফল্ট BE, glibc-র মতো) */
};

/* CP1252-এর 0x80–0x9F */
static const unsigned short cp1252_hi[32] = {
    0x20ac, 0, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017d, 0,
    0, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0, 0x017e, 0x0178,
};

static int cs_lookup(const char *name, int *ignore, int *translit)
{
    char n[40];
    size_t i, k = 0;
    const char *sl = strstr(name, "//");
    if (sl) {
        if (strstr(sl, "IGNORE") || strstr(sl, "ignore")) *ignore = 1;
        if (strstr(sl, "TRANSLIT") || strstr(sl, "translit")) *translit = 1;
    }
    for (i = 0; name[i] && (!sl || name + i < sl) && k + 1 < sizeof(n); i++) {
        char c = name[i];
        if (c == '-' || c == '_') continue;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        n[k++] = c;
    }
    n[k] = '\0';
    if (!strcmp(n, "UTF8") || !k) return CS_UTF8;
    if (!strcmp(n, "ASCII") || !strcmp(n, "USASCII") || !strcmp(n, "ANSIX3.41968") || !strcmp(n, "646")) return CS_ASCII;
    if (!strcmp(n, "ISO88591") || !strcmp(n, "LATIN1") || !strcmp(n, "L1") || !strcmp(n, "ISO885911987")) return CS_LATIN1;
    if (!strcmp(n, "UTF16")) return CS_UTF16;
    if (!strcmp(n, "UTF16LE") || !strcmp(n, "UCS2LE")) return CS_UTF16LE;
    if (!strcmp(n, "UTF16BE") || !strcmp(n, "UCS2BE") || !strcmp(n, "UCS2")) return CS_UTF16BE;
    if (!strcmp(n, "UTF32") || !strcmp(n, "UCS4")) return CS_UTF32;
    if (!strcmp(n, "UTF32LE") || !strcmp(n, "UCS4LE")) return CS_UTF32LE;
    if (!strcmp(n, "UTF32BE") || !strcmp(n, "UCS4BE")) return CS_UTF32BE;
    if (!strcmp(n, "WCHART")) return CS_WCHAR;
    if (!strcmp(n, "CP1252") || !strcmp(n, "WINDOWS1252")) return CS_CP1252;
    return 0;
}

iconv_t iconv_open(const char *to, const char *from)
{
    struct iconv_state *st = (struct iconv_state *)calloc(1, sizeof(*st));
    int dummy = 0;
    if (!st) return (iconv_t)-1;
    st->to = cs_lookup(to, &st->ignore, &st->translit);
    st->from = cs_lookup(from, &dummy, &dummy);
    if (!st->to || !st->from) {
        free(st);
        errno = EINVAL;
        return (iconv_t)-1;
    }
    st->from_be = 1;
    return (iconv_t)st;
}

int iconv_close(iconv_t cd)
{
    free(cd);
    return 0;
}

/* একটা অক্ষর পড়া: ফেরত খরচ হওয়া বাইট, 0 = অসম্পূর্ণ, -1 = ভুল */
static int dec(struct iconv_state *st, const unsigned char *p, size_t n, unsigned *cp)
{
    switch (st->from) {
    case CS_UTF8: {
        mbstate_t ms;
        wchar_t wc;
        size_t r;
        memset(&ms, 0, sizeof(ms));
        r = mbrtowc(&wc, (const char *)p, n, &ms);
        if (r == (size_t)-2) return 0;
        if (r == (size_t)-1) return -1;
        *cp = (unsigned)wc;
        return r ? (int)r : 1;
    }
    case CS_ASCII:
        if (p[0] >= 0x80) return -1;
        *cp = p[0];
        return 1;
    case CS_LATIN1:
        *cp = p[0];
        return 1;
    case CS_CP1252:
        if (p[0] >= 0x80 && p[0] < 0xa0) {
            if (!cp1252_hi[p[0] - 0x80]) return -1;
            *cp = cp1252_hi[p[0] - 0x80];
        } else *cp = p[0];
        return 1;
    case CS_UTF16:
    case CS_UTF16LE:
    case CS_UTF16BE: {
        int be = st->from == CS_UTF16BE || (st->from == CS_UTF16 && st->from_be);
        unsigned u, u2;
        if (n < 2) return 0;
        u = be ? (unsigned)(p[0] << 8 | p[1]) : (unsigned)(p[1] << 8 | p[0]);
        if (u >= 0xd800 && u <= 0xdbff) {
            if (n < 4) return 0;
            u2 = be ? (unsigned)(p[2] << 8 | p[3]) : (unsigned)(p[3] << 8 | p[2]);
            if (u2 < 0xdc00 || u2 > 0xdfff) return -1;
            *cp = 0x10000 + ((u - 0xd800) << 10) + (u2 - 0xdc00);
            return 4;
        }
        if (u >= 0xdc00 && u <= 0xdfff) return -1;
        *cp = u;
        return 2;
    }
    case CS_UTF32:
    case CS_UTF32LE:
    case CS_UTF32BE:
    case CS_WCHAR: {
        int be = st->from == CS_UTF32BE || (st->from == CS_UTF32 && st->from_be);
        unsigned u;
        if (n < 4) return 0;
        if (st->from == CS_WCHAR) be = 0;
        u = be ? (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3]
               : (unsigned)p[3] << 24 | (unsigned)p[2] << 16 | (unsigned)p[1] << 8 | p[0];
        if (u > 0x10ffff || (u >= 0xd800 && u <= 0xdfff)) return -1;
        *cp = u;
        return 4;
    }
    }
    return -1;
}

/* একটা অক্ষর লেখা: ফেরত লেখা বাইট, -1 = এই এনকোডিংয়ে নেই, -2 = জায়গা নেই */
static int enc(struct iconv_state *st, unsigned cp, unsigned char *o, size_t n)
{
    switch (st->to) {
    case CS_UTF8: {
        char b[4];
        size_t r = wcrtomb(b, (wchar_t)cp, NULL);
        if (r == (size_t)-1) return -1;
        if (r > n) return -2;
        memcpy(o, b, r);
        return (int)r;
    }
    case CS_ASCII:
    case CS_LATIN1:
    case CS_CP1252: {
        unsigned lim = st->to == CS_ASCII ? 0x80 : 0x100;
        unsigned v = cp;
        if (st->to == CS_CP1252 && cp >= 0x100) {
            int i;
            v = 0x100;
            for (i = 0; i < 32; i++) if (cp1252_hi[i] == cp) { v = 0x80 + (unsigned)i; break; }
        } else if (st->to == CS_CP1252 && cp >= 0x80 && cp < 0xa0) v = 0x100;
        if (v >= lim) return -1;
        if (n < 1) return -2;
        o[0] = (unsigned char)v;
        return 1;
    }
    case CS_UTF16:
    case CS_UTF16LE:
    case CS_UTF16BE: {
        int be = st->to != CS_UTF16LE, k = 0;
        unsigned u[2];
        int cnt, i;
        if (st->to == CS_UTF16 && !st->to_bom_done) {
            if (n < 2) return -2;
            o[0] = 0xfe; o[1] = 0xff;
            st->to_bom_done = 1;
            k = 2;
        }
        if (cp >= 0x10000) {
            u[0] = 0xd800 + ((cp - 0x10000) >> 10);
            u[1] = 0xdc00 + ((cp - 0x10000) & 0x3ff);
            cnt = 2;
        } else {
            u[0] = cp;
            cnt = 1;
        }
        if (n < (size_t)(k + cnt * 2)) { if (k) st->to_bom_done = 0; return -2; }
        for (i = 0; i < cnt; i++) {
            o[k + i * 2] = (unsigned char)(be ? u[i] >> 8 : u[i]);
            o[k + i * 2 + 1] = (unsigned char)(be ? u[i] : u[i] >> 8);
        }
        return k + cnt * 2;
    }
    case CS_UTF32:
    case CS_UTF32LE:
    case CS_UTF32BE:
    case CS_WCHAR: {
        int be = st->to == CS_UTF32BE || st->to == CS_UTF32, k = 0;
        if (st->to == CS_UTF32 && !st->to_bom_done) {
            if (n < 4) return -2;
            o[0] = 0; o[1] = 0; o[2] = 0xfe; o[3] = 0xff;
            st->to_bom_done = 1;
            k = 4;
        }
        if (n < (size_t)(k + 4)) { if (k) st->to_bom_done = 0; return -2; }
        if (be) { o[k] = (unsigned char)(cp >> 24); o[k + 1] = (unsigned char)(cp >> 16); o[k + 2] = (unsigned char)(cp >> 8); o[k + 3] = (unsigned char)cp; }
        else { o[k + 3] = (unsigned char)(cp >> 24); o[k + 2] = (unsigned char)(cp >> 16); o[k + 1] = (unsigned char)(cp >> 8); o[k] = (unsigned char)cp; }
        return k + 4;
    }
    }
    return -1;
}

size_t iconv(iconv_t cd, char **in, size_t *inleft, char **out, size_t *outleft)
{
    struct iconv_state *st = (struct iconv_state *)cd;
    size_t count = 0;
    if (!st || cd == (iconv_t)-1) { errno = EBADF; return (size_t)-1; }
    if (!in || !*in) {
        /* অবস্থা ফেরানো */
        st->from_bom_done = 0;
        st->to_bom_done = 0;
        st->from_be = 1;
        return 0;
    }
    while (*inleft) {
        unsigned cp;
        const unsigned char *p = (const unsigned char *)*in;
        int r, w;
        if (!st->from_bom_done && (st->from == CS_UTF16 || st->from == CS_UTF32)) {
            size_t need = st->from == CS_UTF16 ? 2 : 4;
            if (*inleft < need) { errno = EINVAL; return (size_t)-1; }
            st->from_bom_done = 1;
            if (need == 2 && p[0] == 0xff && p[1] == 0xfe) { st->from_be = 0; *in += 2; *inleft -= 2; continue; }
            if (need == 2 && p[0] == 0xfe && p[1] == 0xff) { st->from_be = 1; *in += 2; *inleft -= 2; continue; }
            if (need == 4 && p[0] == 0xff && p[1] == 0xfe && !p[2] && !p[3]) { st->from_be = 0; *in += 4; *inleft -= 4; continue; }
            if (need == 4 && !p[0] && !p[1] && p[2] == 0xfe && p[3] == 0xff) { st->from_be = 1; *in += 4; *inleft -= 4; continue; }
        }
        r = dec(st, p, *inleft, &cp);
        if (r == 0) { errno = EINVAL; return (size_t)-1; }
        if (r < 0) {
            if (st->ignore) { (*in)++; (*inleft)--; count++; continue; }
            errno = EILSEQ;
            return (size_t)-1;
        }
        w = enc(st, cp, (unsigned char *)*out, *outleft);
        if (w == -1) {
            if (st->ignore) { *in += r; *inleft -= (size_t)r; count++; continue; }
            if (st->translit) w = enc(st, '?', (unsigned char *)*out, *outleft);
            if (w == -1) { errno = EILSEQ; return (size_t)-1; }
            count++;
        }
        if (w == -2) { errno = E2BIG; return (size_t)-1; }
        *out += w;
        *outleft -= (size_t)w;
        *in += r;
        *inleft -= (size_t)r;
    }
    return count;
}
