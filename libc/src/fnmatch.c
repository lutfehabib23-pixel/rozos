/* rozlibc — fnmatch (ধাপ ১১-এ posix.c-তে ছিল; ধাপ ১৭ক-এ আলাদা ফাইল)
 * আলাদা object: binutils/GCC-এর libiberty নিজের fnmatch দেয় — posix.c-র অন্য ফাংশন
 * (system, glob …) টানলে একই object-এ fnmatch থাকলে "multiple definition" হতো। */

#include <string.h>
#include <fnmatch.h>

/* ---------- fnmatch ---------- */

/* [...] শ্রেণি; মিললে *pp শ্রেণির পরে, ফেরত 1/0; ভুল শ্রেণি হলে -1 */
static int match_class(const char **pp, int c, int flags)
{
    const char *p = *pp + 1;
    int neg = 0, ok = 0;
    if (*p == '!' || *p == '^') { neg = 1; p++; }
    if (*p == ']') { ok = (c == ']'); p++; }
    while (*p && *p != ']') {
        int lo = (unsigned char)*p, hi;
        if (lo == '\\' && !(flags & FNM_NOESCAPE) && p[1]) lo = (unsigned char)*++p;
        p++;
        hi = lo;
        if (*p == '-' && p[1] && p[1] != ']') {
            hi = (unsigned char)p[1];
            if (hi == '\\' && !(flags & FNM_NOESCAPE) && p[2]) { hi = (unsigned char)p[2]; p++; }
            p += 2;
        }
        if (c >= lo && c <= hi) ok = 1;
    }
    if (*p != ']') return -1;
    *pp = p + 1;
    return ok ^ neg;
}

static int fnm(const char *p, const char *s, int flags, const char *start)
{
    for (;;) {
        int c = (unsigned char)*s;
        switch (*p) {
        case '\0':
            return c ? FNM_NOMATCH : 0;
        case '?':
            if (!c || ((flags & FNM_PATHNAME) && c == '/')) return FNM_NOMATCH;
            if ((flags & FNM_PERIOD) && c == '.' && (s == start || ((flags & FNM_PATHNAME) && s[-1] == '/')))
                return FNM_NOMATCH;
            p++; s++;
            break;
        case '*':
            while (*p == '*') p++;
            if ((flags & FNM_PERIOD) && c == '.' && (s == start || ((flags & FNM_PATHNAME) && s[-1] == '/')))
                return FNM_NOMATCH;
            if (!*p) return ((flags & FNM_PATHNAME) && strchr(s, '/')) ? FNM_NOMATCH : 0;
            for (; *s; s++) {
                if (!fnm(p, s, flags & ~FNM_PERIOD, start)) return 0;
                if ((flags & FNM_PATHNAME) && *s == '/') return FNM_NOMATCH;
            }
            return fnm(p, s, flags & ~FNM_PERIOD, start);
        case '[': {
            const char *q = p;
            int r;
            if (!c || ((flags & FNM_PATHNAME) && c == '/')) return FNM_NOMATCH;
            r = match_class(&q, c, flags);
            if (r < 0) {                       /* ভুল শ্রেণি: '[' আক্ষরিক */
                if (c != '[') return FNM_NOMATCH;
                p++; s++;
                break;
            }
            if (!r) return FNM_NOMATCH;
            p = q; s++;
            break;
        }
        case '\\':
            if (!(flags & FNM_NOESCAPE) && p[1]) p++;
            /* fall through */
        default:
            if ((unsigned char)*p != c) return FNM_NOMATCH;
            p++; s++;
            break;
        }
    }
}

int fnmatch(const char *pattern, const char *string, int flags)
{
    return fnm(pattern, string, flags, string);
}
