/* rozlibc — wide অক্ষর ও মাল্টিবাইট (ধাপ ১৬ক)
 *
 * wchar_t = Unicode code point (৩২ বিট)। মাল্টিবাইট মানে সবসময় UTF-8: overlong রূপ,
 * সারোগেট (D800–DFFF) আর 10FFFF-এর বেশি ভুল (EILSEQ)। wcwidth: যুক্ত চিহ্ন (বাংলার
 * কার-চিহ্ন, হসন্ত ইত্যাদি সহ) = ০, CJK/ইমোজি = ২। wide FILE I/O = UTF-8 বাইট।
 * wprintf পরিবার: ফরম্যাট UTF-8-এ বদলে সাধারণ printf ইঞ্জিন, ফল আবার wide-এ। */

#include <wchar.h>
#include <wctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>

static mbstate_t internal_state;

/* ---------- UTF-8 ---------- */

wint_t btowc(int c)
{
    if (c == EOF) return WEOF;
    return (unsigned)c < 0x80 ? (wint_t)c : WEOF;
}

int wctob(wint_t c)
{
    return c < 0x80 ? (int)c : EOF;
}

int mbsinit(const mbstate_t *ps)
{
    return !ps || ps->count == 0;
}

size_t mbrtowc(wchar_t *wc, const char *s, size_t n, mbstate_t *ps)
{
    static mbstate_t own;
    const unsigned char *p = (const unsigned char *)s;
    size_t used = 0;
    unsigned c, need, acc, min;
    if (!ps) ps = &own;
    if (!s) {
        if (ps->count) { ps->count = 0; errno = EILSEQ; return (size_t)-1; }
        return 0;
    }
    if (n == 0) return (size_t)-2;
    need = ps->count;
    acc = ps->wc;
    if (!need) {
        c = p[used++];
        if (c < 0x80) {
            if (wc) *wc = (wchar_t)c;
            return c ? 1 : 0;
        }
        if (c >= 0xc2 && c <= 0xdf) { need = 1; acc = c & 0x1f; }
        else if (c >= 0xe0 && c <= 0xef) { need = 2; acc = c & 0x0f; }
        else if (c >= 0xf0 && c <= 0xf4) { need = 3; acc = c & 0x07; }
        else { errno = EILSEQ; return (size_t)-1; }
        /* আগের অবস্থায় কতগুলো বাইট মোট লাগবে তা acc-র ওপরের বিটে রাখা (overlong যাচাইয়ের জন্য) */
        acc |= (need << 24);
    }
    while (used < n && (need & 0xff)) {
        c = p[used];
        if ((c & 0xc0) != 0x80) { ps->count = 0; errno = EILSEQ; return (size_t)-1; }
        acc = (acc & 0xff000000u) | (((acc & 0x00ffffffu) << 6) | (c & 0x3f));
        used++;
        need--;
    }
    if (need) {
        ps->count = need;
        ps->wc = acc;
        return (size_t)-2;
    }
    ps->count = 0;
    {
        unsigned total = acc >> 24, v = acc & 0x00ffffffu;
        /* acc-র ওপরের বিটে শুরুর need ছিল, প্রতিটা ধারাবাহিক বাইটে ২৪-এর ওপরের মান বদলায়নি */
        min = total == 1 ? 0x80 : total == 2 ? 0x800 : 0x10000;
        if (v < min || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff)) {
            errno = EILSEQ;
            return (size_t)-1;
        }
        if (wc) *wc = (wchar_t)v;
        return v ? used : 0;
    }
}

size_t mbrlen(const char *s, size_t n, mbstate_t *ps)
{
    static mbstate_t own;
    return mbrtowc(NULL, s, n, ps ? ps : &own);
}

size_t wcrtomb(char *s, wchar_t wc, mbstate_t *ps)
{
    unsigned c = (unsigned)wc;
    unsigned char *p = (unsigned char *)s;
    if (ps) ps->count = 0;
    if (!s) return 1;
    if (c < 0x80) { p[0] = (unsigned char)c; return 1; }
    if (c < 0x800) {
        p[0] = (unsigned char)(0xc0 | (c >> 6));
        p[1] = (unsigned char)(0x80 | (c & 0x3f));
        return 2;
    }
    if (c >= 0xd800 && c <= 0xdfff) { errno = EILSEQ; return (size_t)-1; }
    if (c < 0x10000) {
        p[0] = (unsigned char)(0xe0 | (c >> 12));
        p[1] = (unsigned char)(0x80 | ((c >> 6) & 0x3f));
        p[2] = (unsigned char)(0x80 | (c & 0x3f));
        return 3;
    }
    if (c <= 0x10ffff) {
        p[0] = (unsigned char)(0xf0 | (c >> 18));
        p[1] = (unsigned char)(0x80 | ((c >> 12) & 0x3f));
        p[2] = (unsigned char)(0x80 | ((c >> 6) & 0x3f));
        p[3] = (unsigned char)(0x80 | (c & 0x3f));
        return 4;
    }
    errno = EILSEQ;
    return (size_t)-1;
}

size_t mbsnrtowcs(wchar_t *dst, const char **src, size_t nms, size_t n, mbstate_t *ps)
{
    const char *s = *src;
    size_t count = 0;
    if (!ps) ps = &internal_state;
    while (!dst || count < n) {
        wchar_t wc;
        size_t r;
        if (nms == 0) break;
        r = mbrtowc(&wc, s, nms, ps);
        if (r == (size_t)-1) { *src = s; return (size_t)-1; }
        if (r == (size_t)-2) { s += nms; nms = 0; break; }
        if (r == 0) {
            if (dst) { dst[count] = 0; *src = NULL; }
            return count;
        }
        if (dst) dst[count] = wc;
        count++;
        s += r;
        nms -= r;
    }
    if (dst) *src = s;
    return count;
}

size_t mbsrtowcs(wchar_t *dst, const char **src, size_t n, mbstate_t *ps)
{
    return mbsnrtowcs(dst, src, (size_t)-1, n, ps);
}

size_t wcsnrtombs(char *dst, const wchar_t **src, size_t nwc, size_t n, mbstate_t *ps)
{
    const wchar_t *s = *src;
    size_t count = 0;
    char tmp[4];
    (void)ps;
    while (nwc--) {
        size_t r;
        if (*s == 0) {
            if (dst) {
                if (count >= n) break;
                dst[count] = '\0';
                *src = NULL;
            }
            return count;
        }
        r = wcrtomb(tmp, *s, NULL);
        if (r == (size_t)-1) { if (dst) *src = s; return (size_t)-1; }
        if (dst) {
            if (count + r > n) break;
            memcpy(dst + count, tmp, r);
        }
        count += r;
        s++;
    }
    if (dst) *src = s;
    return count;
}

size_t wcsrtombs(char *dst, const wchar_t **src, size_t n, mbstate_t *ps)
{
    return wcsnrtombs(dst, src, (size_t)-1, n, ps);
}

/* stdlib.h-এর পুরনো ধাঁচের রূপগুলো */
int mblen(const char *s, size_t n)
{
    static mbstate_t st;
    size_t r;
    if (!s) { memset(&st, 0, sizeof(st)); return 0; }
    r = mbrtowc(NULL, s, n, &st);
    if (r == (size_t)-1 || r == (size_t)-2) { memset(&st, 0, sizeof(st)); return -1; }
    return (int)r;
}

int mbtowc(wchar_t *wc, const char *s, size_t n)
{
    static mbstate_t st;
    size_t r;
    if (!s) { memset(&st, 0, sizeof(st)); return 0; }
    r = mbrtowc(wc, s, n, &st);
    if (r == (size_t)-1 || r == (size_t)-2) { memset(&st, 0, sizeof(st)); return -1; }
    return (int)r;
}

int wctomb(char *s, wchar_t wc)
{
    if (!s) return 0;
    return (int)wcrtomb(s, wc, NULL);
}

size_t mbstowcs(wchar_t *dst, const char *src, size_t n)
{
    mbstate_t st;
    memset(&st, 0, sizeof(st));
    return mbsrtowcs(dst, &src, n, &st);
}

size_t wcstombs(char *dst, const wchar_t *src, size_t n)
{
    return wcsrtombs(dst, &src, n, NULL);
}

/* ---------- wide স্ট্রিং ---------- */

size_t wcslen(const wchar_t *s) { size_t n = 0; while (s[n]) n++; return n; }
size_t wcsnlen(const wchar_t *s, size_t max) { size_t n = 0; while (n < max && s[n]) n++; return n; }
wchar_t *wcscpy(wchar_t *d, const wchar_t *s) { wchar_t *r = d; while ((*d++ = *s++)) {} return r; }
wchar_t *wcpcpy(wchar_t *d, const wchar_t *s) { while ((*d = *s++)) d++; return d; }

wchar_t *wcsncpy(wchar_t *d, const wchar_t *s, size_t n)
{
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}

wchar_t *wcpncpy(wchar_t *d, const wchar_t *s, size_t n)
{
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    {
        wchar_t *end = d + i;
        for (; i < n; i++) d[i] = 0;
        return end;
    }
}

wchar_t *wcscat(wchar_t *d, const wchar_t *s) { wcscpy(d + wcslen(d), s); return d; }

wchar_t *wcsncat(wchar_t *d, const wchar_t *s, size_t n)
{
    wchar_t *p = d + wcslen(d);
    while (n-- && *s) *p++ = *s++;
    *p = 0;
    return d;
}

int wcscmp(const wchar_t *a, const wchar_t *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a < *b ? -1 : *a > *b;
}

int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b) return *a < *b ? -1 : 1;
        if (!*a) break;
    }
    return 0;
}

int wcscasecmp(const wchar_t *a, const wchar_t *b)
{
    while (*a && towlower((wint_t)*a) == towlower((wint_t)*b)) { a++; b++; }
    return (int)towlower((wint_t)*a) - (int)towlower((wint_t)*b);
}

int wcsncasecmp(const wchar_t *a, const wchar_t *b, size_t n)
{
    for (; n; n--, a++, b++) {
        wint_t x = towlower((wint_t)*a), y = towlower((wint_t)*b);
        if (x != y) return (int)x - (int)y;
        if (!x) break;
    }
    return 0;
}

int wcscoll(const wchar_t *a, const wchar_t *b) { return wcscmp(a, b); }

size_t wcsxfrm(wchar_t *d, const wchar_t *s, size_t n)
{
    size_t l = wcslen(s);
    if (l < n) wcscpy(d, s);
    return l;
}

wchar_t *wcschr(const wchar_t *s, wchar_t c)
{
    for (;; s++) {
        if (*s == c) return (wchar_t *)s;
        if (!*s) return NULL;
    }
}

wchar_t *wcsrchr(const wchar_t *s, wchar_t c)
{
    const wchar_t *r = NULL;
    for (;; s++) {
        if (*s == c) r = s;
        if (!*s) return (wchar_t *)r;
    }
}

wchar_t *wcsstr(const wchar_t *h, const wchar_t *n)
{
    size_t nl = wcslen(n);
    if (!nl) return (wchar_t *)h;
    for (; *h; h++) if (*h == *n && !wcsncmp(h, n, nl)) return (wchar_t *)h;
    return NULL;
}

size_t wcsspn(const wchar_t *s, const wchar_t *acc)
{
    size_t n = 0;
    while (s[n] && wcschr(acc, s[n])) n++;
    return n;
}

size_t wcscspn(const wchar_t *s, const wchar_t *rej)
{
    size_t n = 0;
    while (s[n] && !wcschr(rej, s[n])) n++;
    return n;
}

wchar_t *wcspbrk(const wchar_t *s, const wchar_t *acc)
{
    s += wcscspn(s, acc);
    return *s ? (wchar_t *)s : NULL;
}

wchar_t *wcstok(wchar_t *s, const wchar_t *delim, wchar_t **save)
{
    wchar_t *end;
    if (!s) s = *save;
    if (!s) return NULL;
    s += wcsspn(s, delim);
    if (!*s) { *save = NULL; return NULL; }
    end = s + wcscspn(s, delim);
    if (*end) { *end = 0; *save = end + 1; }
    else *save = NULL;
    return s;
}

wchar_t *wcsdup(const wchar_t *s)
{
    size_t n = wcslen(s) + 1;
    wchar_t *d = (wchar_t *)malloc(n * sizeof(wchar_t));
    if (d) memcpy(d, s, n * sizeof(wchar_t));
    return d;
}

wchar_t *wmemcpy(wchar_t *d, const wchar_t *s, size_t n) { return (wchar_t *)memcpy(d, s, n * sizeof(wchar_t)); }
wchar_t *wmemmove(wchar_t *d, const wchar_t *s, size_t n) { return (wchar_t *)memmove(d, s, n * sizeof(wchar_t)); }

wchar_t *wmemset(wchar_t *d, wchar_t c, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) d[i] = c;
    return d;
}

int wmemcmp(const wchar_t *a, const wchar_t *b, size_t n)
{
    for (; n; n--, a++, b++) if (*a != *b) return *a < *b ? -1 : 1;
    return 0;
}

wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n)
{
    for (; n; n--, s++) if (*s == c) return (wchar_t *)s;
    return NULL;
}

/* ---------- প্রস্থ ---------- */

struct range { unsigned lo, hi; };

/* যুক্ত (zero-width) চিহ্ন — সাধারণ ব্লকগুলো (বাংলা সহ ভারতীয় লিপির মাত্রা/হসন্ত) */
static const struct range zero_width[] = {
    { 0x0300, 0x036f }, { 0x0483, 0x0489 }, { 0x0591, 0x05bd }, { 0x05bf, 0x05bf }, { 0x05c1, 0x05c2 },
    { 0x05c4, 0x05c5 }, { 0x05c7, 0x05c7 }, { 0x0610, 0x061a }, { 0x064b, 0x065f }, { 0x0670, 0x0670 },
    { 0x06d6, 0x06dc }, { 0x06df, 0x06e4 }, { 0x06e7, 0x06e8 }, { 0x06ea, 0x06ed },
    { 0x0900, 0x0902 }, { 0x093a, 0x093a }, { 0x093c, 0x093c }, { 0x0941, 0x0948 }, { 0x094d, 0x094d },
    { 0x0951, 0x0957 }, { 0x0962, 0x0963 },
    { 0x0981, 0x0981 }, { 0x09bc, 0x09bc }, { 0x09c1, 0x09c4 }, { 0x09cd, 0x09cd }, { 0x09e2, 0x09e3 },
    { 0x09fe, 0x09fe },
    { 0x0a01, 0x0a02 }, { 0x0a3c, 0x0a3c }, { 0x0a41, 0x0a51 }, { 0x0a70, 0x0a71 }, { 0x0a75, 0x0a75 },
    { 0x0a81, 0x0a82 }, { 0x0abc, 0x0abc }, { 0x0ac1, 0x0ac8 }, { 0x0acd, 0x0acd },
    { 0x0b01, 0x0b01 }, { 0x0b3c, 0x0b3c }, { 0x0b3f, 0x0b3f }, { 0x0b41, 0x0b44 }, { 0x0b4d, 0x0b4d },
    { 0x0bc0, 0x0bc0 }, { 0x0bcd, 0x0bcd }, { 0x0c3e, 0x0c40 }, { 0x0c46, 0x0c56 }, { 0x0cbc, 0x0cbc },
    { 0x0ccc, 0x0ccd }, { 0x0d41, 0x0d44 }, { 0x0d4d, 0x0d4d }, { 0x0dca, 0x0dca }, { 0x0dd2, 0x0dd6 },
    { 0x0e31, 0x0e31 }, { 0x0e34, 0x0e3a }, { 0x0e47, 0x0e4e }, { 0x0eb1, 0x0eb1 }, { 0x0eb4, 0x0ebc },
    { 0x0ec8, 0x0ecd }, { 0x0f71, 0x0f7e }, { 0x1160, 0x11ff }, { 0x200b, 0x200f }, { 0x202a, 0x202e },
    { 0x2060, 0x2064 }, { 0x20d0, 0x20f0 }, { 0xfe00, 0xfe0f }, { 0xfe20, 0xfe2f }, { 0xfeff, 0xfeff },
    { 0x1f3fb, 0x1f3ff }, { 0xe0100, 0xe01ef },
};

static const struct range wide[] = {
    { 0x1100, 0x115f }, { 0x231a, 0x231b }, { 0x2329, 0x232a }, { 0x23e9, 0x23ec }, { 0x23f0, 0x23f0 },
    { 0x23f3, 0x23f3 }, { 0x25fd, 0x25fe }, { 0x2614, 0x2615 }, { 0x2648, 0x2653 }, { 0x267f, 0x267f },
    { 0x2693, 0x2693 }, { 0x26a1, 0x26a1 }, { 0x26aa, 0x26ab }, { 0x26bd, 0x26be }, { 0x26c4, 0x26c5 },
    { 0x26ce, 0x26ce }, { 0x26d4, 0x26d4 }, { 0x26ea, 0x26ea }, { 0x26f2, 0x26f3 }, { 0x26f5, 0x26f5 },
    { 0x26fa, 0x26fa }, { 0x26fd, 0x26fd }, { 0x2705, 0x2705 }, { 0x270a, 0x270b }, { 0x2728, 0x2728 },
    { 0x274c, 0x274c }, { 0x274e, 0x274e }, { 0x2753, 0x2755 }, { 0x2757, 0x2757 }, { 0x2795, 0x2797 },
    { 0x27b0, 0x27b0 }, { 0x27bf, 0x27bf }, { 0x2b1b, 0x2b1c }, { 0x2b50, 0x2b50 }, { 0x2b55, 0x2b55 },
    { 0x2e80, 0x303e }, { 0x3041, 0x33ff }, { 0x3400, 0x4dbf }, { 0x4e00, 0x9fff }, { 0xa000, 0xa4cf },
    { 0xa960, 0xa97f }, { 0xac00, 0xd7a3 }, { 0xf900, 0xfaff }, { 0xfe10, 0xfe19 }, { 0xfe30, 0xfe6f },
    { 0xff00, 0xff60 }, { 0xffe0, 0xffe6 }, { 0x16fe0, 0x16fe4 }, { 0x17000, 0x18cff },
    { 0x1b000, 0x1b2ff }, { 0x1f004, 0x1f004 }, { 0x1f0cf, 0x1f0cf }, { 0x1f18e, 0x1f18e },
    { 0x1f191, 0x1f19a }, { 0x1f200, 0x1f251 }, { 0x1f300, 0x1f64f }, { 0x1f680, 0x1f6ff },
    { 0x1f7e0, 0x1f7eb }, { 0x1f90c, 0x1f9ff }, { 0x1fa70, 0x1faff }, { 0x20000, 0x2fffd },
    { 0x30000, 0x3fffd },
};

static int in_ranges(unsigned c, const struct range *r, size_t n)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (c < r[mid].lo) hi = mid;
        else if (c > r[mid].hi) lo = mid + 1;
        else return 1;
    }
    return 0;
}

int wcwidth(wchar_t wc)
{
    unsigned c = (unsigned)wc;
    if (c == 0) return 0;
    if (c < 0x20 || (c >= 0x7f && c < 0xa0)) return -1;
    if (c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return -1;
    if (c == 0xad) return 1;
    if (in_ranges(c, zero_width, sizeof(zero_width) / sizeof(zero_width[0]))) return 0;
    if (in_ranges(c, wide, sizeof(wide) / sizeof(wide[0]))) return 2;
    return 1;
}

int wcswidth(const wchar_t *s, size_t n)
{
    int w = 0;
    for (; n && *s; n--, s++) {
        int k = wcwidth(*s);
        if (k < 0) return -1;
        w += k;
    }
    return w;
}

/* ---------- শ্রেণি (wctype) ---------- */

int iswdigit(wint_t c) { return c >= '0' && c <= '9'; }
int iswxdigit(wint_t c) { return c < 0x80 && isxdigit((int)c); }
int iswblank(wint_t c) { return c == ' ' || c == '\t' || c == 0x3000 || c == 0x1680 || (c >= 0x2000 && c <= 0x2006) || (c >= 0x2008 && c <= 0x200a) || c == 0x205f; }

int iswspace(wint_t c)
{
    return (c >= 9 && c <= 13) || c == ' ' || c == 0x85 || c == 0x1680 || (c >= 0x2000 && c <= 0x2006) ||
           (c >= 0x2008 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x205f || c == 0x3000;
}

int iswcntrl(wint_t c) { return c < 0x20 || (c >= 0x7f && c < 0xa0) || c == 0x2028 || c == 0x2029; }

int iswpunct(wint_t c)
{
    if (c < 0x80) return ispunct((int)c);
    return (c >= 0xa1 && c <= 0xbf && c != 0xaa && c != 0xb5 && c != 0xba) || c == 0xd7 || c == 0xf7 ||
           (c >= 0x2010 && c <= 0x2027) || (c >= 0x2030 && c <= 0x205e) || (c >= 0x2190 && c <= 0x2bff) ||
           (c >= 0x3001 && c <= 0x3003) || (c >= 0x3008 && c <= 0x3011) || c == 0x0964 || c == 0x0965 ||
           (c >= 0xff01 && c <= 0xff0f);
}

int iswupper(wint_t c)
{
    if (c < 0x80) return c >= 'A' && c <= 'Z';
    if (c >= 0xc0 && c <= 0xde) return c != 0xd7;
    if (c == 0x138 || c == 0x149 || c == 0x17f) return 0;
    if (c == 0x178) return 1;
    if (c >= 0x100 && c <= 0x17f) return !(c & 1) ^ (c >= 0x139 && c <= 0x148) ^ (c >= 0x179);
    if (c >= 0x391 && c <= 0x3ab) return c != 0x3a2;
    if (c >= 0x400 && c <= 0x42f) return 1;
    return 0;
}

int iswlower(wint_t c)
{
    if (c < 0x80) return c >= 'a' && c <= 'z';
    if (c >= 0xdf && c <= 0xff) return c != 0xf7;
    if (c >= 0x100 && c <= 0x17f) return !iswupper(c);
    if (c >= 0x3ac && c <= 0x3ce) return 1;
    if (c >= 0x430 && c <= 0x45f) return 1;
    return 0;
}

int iswalpha(wint_t c)
{
    if (c < 0x80) return isalpha((int)c);
    if (c > 0x10ffff) return 0;
    if (c == 0xaa || c == 0xb5 || c == 0xba) return 1;
    if (c >= 0xc0 && c <= 0x24f) return c != 0xd7 && c != 0xf7;
    if (iswspace(c) || iswpunct(c) || iswcntrl(c)) return 0;
    if (c >= 0x0966 && c <= 0x096f) return 0;              /* দেবনাগরী অঙ্ক */
    if (c >= 0x09e6 && c <= 0x09ef) return 0;              /* বাংলা অঙ্ক */
    if (c >= 0xff10 && c <= 0xff19) return 0;
    if (in_ranges(c, zero_width, sizeof(zero_width) / sizeof(zero_width[0]))) return 1;   /* মাত্রা = অক্ষরের অংশ */
    if (c >= 0xe000 && c <= 0xf8ff) return 0;              /* ব্যক্তিগত এলাকা */
    if (c >= 0xd800 && c <= 0xdfff) return 0;
    return 1;
}

int iswalnum(wint_t c)
{
    return iswalpha(c) || iswdigit(c) || (c >= 0x09e6 && c <= 0x09ef) || (c >= 0x0966 && c <= 0x096f);
}

int iswgraph(wint_t c) { return iswprint(c) && !iswspace(c); }
int iswprint(wint_t c) { return c >= 0x20 && !iswcntrl(c) && c <= 0x10ffff && !(c >= 0xd800 && c <= 0xdfff); }

wint_t towlower(wint_t c)
{
    if (c < 0x80) return (wint_t)tolower((int)c);
    if (c >= 0xc0 && c <= 0xde && c != 0xd7) return c + 0x20;
    if (c == 0x178) return 0xff;
    if (c >= 0x100 && c <= 0x17f && iswupper(c)) return c + 1;
    if (c >= 0x391 && c <= 0x3ab && c != 0x3a2) return c + 0x20;
    if (c >= 0x410 && c <= 0x42f) return c + 0x20;
    if (c >= 0x400 && c <= 0x40f) return c + 0x50;
    return c;
}

wint_t towupper(wint_t c)
{
    if (c < 0x80) return (wint_t)toupper((int)c);
    if (c >= 0xe0 && c <= 0xfe && c != 0xf7) return c - 0x20;
    if (c == 0xff) return 0x178;
    if (c >= 0x100 && c <= 0x17f && iswlower(c) && c != 0x138 && c != 0x149 && c != 0x17f) return c - 1;
    if (c >= 0x3b1 && c <= 0x3cb && c != 0x3c2) return c - 0x20;
    if (c >= 0x430 && c <= 0x44f) return c - 0x20;
    if (c >= 0x450 && c <= 0x45f) return c - 0x50;
    return c;
}

static const char *const class_names[] = {
    "alnum", "alpha", "blank", "cntrl", "digit", "graph", "lower", "print", "punct", "space",
    "upper", "xdigit",
};

wctype_t wctype(const char *name)
{
    size_t i;
    for (i = 0; i < sizeof(class_names) / sizeof(class_names[0]); i++) {
        if (!strcmp(name, class_names[i])) return (wctype_t)(i + 1);
    }
    return 0;
}

int iswctype(wint_t c, wctype_t t)
{
    switch (t) {
    case 1: return iswalnum(c);
    case 2: return iswalpha(c);
    case 3: return iswblank(c);
    case 4: return iswcntrl(c);
    case 5: return iswdigit(c);
    case 6: return iswgraph(c);
    case 7: return iswlower(c);
    case 8: return iswprint(c);
    case 9: return iswpunct(c);
    case 10: return iswspace(c);
    case 11: return iswupper(c);
    case 12: return iswxdigit(c);
    }
    return 0;
}

static const int trans_lower = 1, trans_upper = 2;

wctrans_t wctrans(const char *name)
{
    if (!strcmp(name, "tolower")) return &trans_lower;
    if (!strcmp(name, "toupper")) return &trans_upper;
    return NULL;
}

wint_t towctrans(wint_t c, wctrans_t t)
{
    if (t == &trans_lower) return towlower(c);
    if (t == &trans_upper) return towupper(c);
    return c;
}

/* ---------- সংখ্যা: UTF-8-এ বদলে সাধারণ strto* ---------- */

#define WNUM_MAX 128

/* সংখ্যার জন্য ASCII অংশটুকু নেওয়া; *skip = শুরুর ফাঁকা কয়টা */
static size_t wnum_copy(const wchar_t *s, char *buf, size_t *skip)
{
    size_t i = 0, k = 0;
    while (iswspace((wint_t)s[i])) i++;
    *skip = i;
    while (s[i] && s[i] < 0x80 && k + 1 < WNUM_MAX) buf[k++] = (char)s[i++];
    buf[k] = '\0';
    return k;
}

#define WSTRTO(name, type, call)                                              \
    type name(const wchar_t *s, wchar_t **end, int base)                      \
    {                                                                         \
        char buf[WNUM_MAX], *e;                                               \
        size_t skip;                                                          \
        type v;                                                               \
        wnum_copy(s, buf, &skip);                                             \
        v = call(buf, &e, base);                                              \
        if (end) *end = (wchar_t *)(e == buf ? s : s + skip + (size_t)(e - buf)); \
        return v;                                                             \
    }
WSTRTO(wcstol, long, strtol)
WSTRTO(wcstoul, unsigned long, strtoul)
WSTRTO(wcstoll, long long, strtoll)
WSTRTO(wcstoull, unsigned long long, strtoull)

#define WSTRTOF(name, type, call)                                             \
    type name(const wchar_t *s, wchar_t **end)                                \
    {                                                                         \
        char buf[WNUM_MAX], *e;                                               \
        size_t skip;                                                          \
        type v;                                                               \
        wnum_copy(s, buf, &skip);                                             \
        v = call(buf, &e);                                                    \
        if (end) *end = (wchar_t *)(e == buf ? s : s + skip + (size_t)(e - buf)); \
        return v;                                                             \
    }
WSTRTOF(wcstod, double, strtod)
WSTRTOF(wcstof, float, strtof)
WSTRTOF(wcstold, long double, strtold)

/* ---------- wide FILE I/O (UTF-8 বাইট) ---------- */

wint_t fgetwc(FILE *f)
{
    char b[4];
    int c = fgetc(f), n, i;
    wchar_t wc;
    mbstate_t st;
    if (c == EOF) return WEOF;
    b[0] = (char)c;
    n = c < 0x80 ? 1 : c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
    for (i = 1; i < n; i++) {
        c = fgetc(f);
        if (c == EOF) { errno = EILSEQ; return WEOF; }
        b[i] = (char)c;
    }
    memset(&st, 0, sizeof(st));
    if (mbrtowc(&wc, b, (size_t)n, &st) > (size_t)n) { errno = EILSEQ; return WEOF; }
    return (wint_t)wc;
}

wint_t getwc(FILE *f) { return fgetwc(f); }
wint_t getwchar(void) { return fgetwc(stdin); }

wint_t fputwc(wchar_t c, FILE *f)
{
    char b[4];
    size_t n = wcrtomb(b, c, NULL);
    if (n == (size_t)-1) return WEOF;
    return fwrite(b, 1, n, f) == n ? (wint_t)c : WEOF;
}

wint_t putwc(wchar_t c, FILE *f) { return fputwc(c, f); }
wint_t putwchar(wchar_t c) { return fputwc(c, stdout); }

wint_t ungetwc(wint_t c, FILE *f)
{
    /* FILE-এ এক বাইট ফেরতের জায়গা — ASCII-ই ফেরত দেওয়া যায় */
    if (c == WEOF || c >= 0x80) return WEOF;
    return ungetc((int)c, f) == EOF ? WEOF : c;
}

wchar_t *fgetws(wchar_t *s, int n, FILE *f)
{
    int i = 0;
    if (n <= 0) return NULL;
    while (i < n - 1) {
        wint_t c = fgetwc(f);
        if (c == WEOF) break;
        s[i++] = (wchar_t)c;
        if (c == '\n') break;
    }
    if (i == 0) return NULL;
    s[i] = 0;
    return s;
}

int fputws(const wchar_t *s, FILE *f)
{
    for (; *s; s++) if (fputwc(*s, f) == WEOF) return -1;
    return 0;
}

int fwide(FILE *f, int mode)
{
    (void)f;
    return mode;                     /* একই FILE-এ দুই ধরনই চলে */
}

/* wide ফরম্যাট → UTF-8 (malloc); %ls/%lc আগের মতোই থাকে কারণ printf নিজেই বোঝে */
static char *w2mb(const wchar_t *w)
{
    size_t n = wcstombs(NULL, w, 0);
    char *s;
    if (n == (size_t)-1) return NULL;
    s = (char *)malloc(n + 1);
    if (s) wcstombs(s, w, n + 1);
    return s;
}

int vfwprintf(FILE *f, const wchar_t *fmt, va_list ap)
{
    char *mf = w2mb(fmt);
    int r;
    if (!mf) { errno = EILSEQ; return -1; }
    r = vfprintf(f, mf, ap);
    free(mf);
    return r;
}

int vwprintf(const wchar_t *fmt, va_list ap) { return vfwprintf(stdout, fmt, ap); }

int vswprintf(wchar_t *s, size_t n, const wchar_t *fmt, va_list ap)
{
    char *mf = w2mb(fmt), *out = NULL;
    int r;
    size_t wl;
    if (!mf) { errno = EILSEQ; return -1; }
    r = vasprintf(&out, mf, ap);
    free(mf);
    if (r < 0) return -1;
    wl = mbstowcs(NULL, out, 0);
    if (wl == (size_t)-1 || wl >= n) {
        if (n) { mbstowcs(s, out, n - 1); s[n - 1] = 0; }
        free(out);
        return -1;                   /* C মান: না ধরলে ঋণাত্মক */
    }
    mbstowcs(s, out, n);
    free(out);
    return (int)wl;
}

int wprintf(const wchar_t *fmt, ...)
{ va_list ap; int r; va_start(ap, fmt); r = vfwprintf(stdout, fmt, ap); va_end(ap); return r; }
int fwprintf(FILE *f, const wchar_t *fmt, ...)
{ va_list ap; int r; va_start(ap, fmt); r = vfwprintf(f, fmt, ap); va_end(ap); return r; }
int swprintf(wchar_t *s, size_t n, const wchar_t *fmt, ...)
{ va_list ap; int r; va_start(ap, fmt); r = vswprintf(s, n, fmt, ap); va_end(ap); return r; }

int vswscanf(const wchar_t *s, const wchar_t *fmt, va_list ap)
{
    char *ms = w2mb(s), *mf = w2mb(fmt);
    int r = -1;
    if (ms && mf) r = vsscanf(ms, mf, ap);
    free(ms);
    free(mf);
    return r;
}

int swscanf(const wchar_t *s, const wchar_t *fmt, ...)
{ va_list ap; int r; va_start(ap, fmt); r = vswscanf(s, fmt, ap); va_end(ap); return r; }

int fwscanf(FILE *f, const wchar_t *fmt, ...)
{
    char *mf = w2mb(fmt);
    va_list ap;
    int r = -1;
    if (!mf) return -1;
    va_start(ap, fmt);
    r = vfscanf(f, mf, ap);
    va_end(ap);
    free(mf);
    return r;
}

int wscanf(const wchar_t *fmt, ...)
{
    char *mf = w2mb(fmt);
    va_list ap;
    int r = -1;
    if (!mf) return -1;
    va_start(ap, fmt);
    r = vfscanf(stdin, mf, ap);
    va_end(ap);
    free(mf);
    return r;
}

size_t wcsftime(wchar_t *s, size_t n, const wchar_t *fmt, const struct tm *tm)
{
    char *mf = w2mb(fmt), buf[512];
    size_t r, wl;
    if (!mf) return 0;
    r = strftime(buf, sizeof(buf), mf, tm);
    free(mf);
    if (!r && *fmt) return 0;
    wl = mbstowcs(NULL, buf, 0);
    if (wl == (size_t)-1 || wl >= n) return 0;
    mbstowcs(s, buf, n);
    return wl;
}
