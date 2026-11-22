/* rozOS — rozcref: C সহায়িকার .ref ফাইল পড়া, খোঁজা, সাজিয়ে লেখা (ধাপ ১৯ক; rozcref.h দেখুন) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include "rozcref.h"

static cref_entry *ents;
static int nents, capents, sorted;

static char *dupn(const char *s, size_t n)
{
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static char *dupz(const char *s) { return dupn(s, strlen(s)); }

/* buf-এ লাইন যোগ ('\n' দিয়ে) */
static void append(char **buf, const char *line)
{
    size_t a = *buf ? strlen(*buf) : 0, b = strlen(line);
    char *n = (char *)realloc(*buf, a + b + 2);
    if (!n) return;
    if (a) n[a++] = '\n';
    memcpy(n + a, line, b + 1);
    *buf = n;
}

static char *trim_end(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
    return s;
}

static void fill_defaults(cref_entry *e)
{
    if (!e->also) e->also = dupz("");
    if (!e->header) e->header = dupz("");
    if (!e->syntax) e->syntax = dupz("");
    if (!e->summary) e->summary = dupz("");
    if (!e->desc) e->desc = dupz("");
    if (!e->returns) e->returns = dupz("");
    if (!e->see) e->see = dupz("");
    if (!e->example) e->example = dupz("");
    if (!e->check) e->check = dupz("run");
    if (!e->input) e->input = dupz("");
}

static cref_entry *new_entry(void)
{
    if (nents == capents) {
        int nc = capents ? capents * 2 : 256;
        cref_entry *n = (cref_entry *)realloc(ents, sizeof(cref_entry) * (size_t)nc);
        if (!n) return NULL;
        ents = n;
        capents = nc;
    }
    memset(&ents[nents], 0, sizeof(cref_entry));
    ents[nents].seq = nents;
    return &ents[nents++];
}

int cref_load_file(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[1024];
    cref_entry *e = NULL;
    char **multi = NULL;                 /* এখন কোন বহু-লাইনের ঘরে লেখা যাচ্ছে */
    int in_example = 0, count = 0;
    const char *fname;
    if (!f) return -1;
    fname = dupz(path);
    while (fgets(line, sizeof(line), f)) {
        trim_end(line);
        if (in_example) {
            if (!strcmp(line, "@end")) { in_example = 0; continue; }
            append(&e->example, line);
            continue;
        }
        if (line[0] == '@') {
            char *kw = line + 1, *val = kw;
            while (*val && *val != ' ' && *val != '\t') val++;
            if (*val) *val++ = 0;
            while (*val == ' ' || *val == '\t') val++;
            multi = NULL;
            if (!strcmp(kw, "entry")) {
                if (e) fill_defaults(e);
                e = new_entry();
                if (!e) break;
                e->name = dupz(val);
                e->file = fname;
                count++;
                continue;
            }
            if (!e) continue;
            if (!strcmp(kw, "also")) e->also = dupz(val);
            else if (!strcmp(kw, "header")) e->header = dupz(val);
            else if (!strcmp(kw, "syntax")) append(&e->syntax, val);
            else if (!strcmp(kw, "summary")) e->summary = dupz(val);
            else if (!strcmp(kw, "returns")) { append(&e->returns, val); multi = &e->returns; }
            else if (!strcmp(kw, "see")) e->see = dupz(val);
            else if (!strcmp(kw, "check")) e->check = dupz(val);
            else if (!strcmp(kw, "desc")) { if (*val) append(&e->desc, val); multi = &e->desc; }
            else if (!strcmp(kw, "input")) { if (*val) append(&e->input, val); multi = &e->input; }
            else if (!strcmp(kw, "example")) in_example = 1;
            continue;
        }
        if (line[0] == '#' && !multi) continue;           /* মন্তব্য (ব্যাখ্যার বাইরে) */
        if (multi && e) append(multi, line);
    }
    if (e) fill_defaults(e);
    fclose(f);
    sorted = 0;
    return count;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int cref_load(const char *dir)
{
    DIR *d;
    struct dirent *de;
    char path[512], *names[256];
    int total = 0, n = 0, i;
    if (nents > 0) return nents;
    if (!dir) dir = CREF_DIR;
    d = opendir(dir);
    if (!d) return -1;
    while ((de = readdir(d)) != NULL && n < 256) {
        size_t len = strlen(de->d_name);
        if (len < 5 || strcmp(de->d_name + len - 4, ".ref")) continue;
        names[n] = dupz(de->d_name);
        if (names[n]) n++;
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof(char *), cmp_str);   /* নামের ক্রমে — ভাষার পাঠ ঠিক ক্রমে আসে */
    for (i = 0; i < n; i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        if (cref_load_file(path) > 0) total++;
        free(names[i]);
    }
    return total > 0 ? nents : -1;
}

static int is_lang(const cref_entry *e) { return !strcmp(e->header, "language"); }

static int cmp_ent(const void *a, const void *b)
{
    const cref_entry *x = (const cref_entry *)a, *y = (const cref_entry *)b;
    int lx = is_lang(x), ly = is_lang(y), c;
    if (lx != ly) return ly - lx;                         /* ভাষা আগে */
    if (lx) return x->seq - y->seq;                       /* ফাইলের ক্রম */
    c = strcmp(x->header, y->header);
    if (c) return c;
    return strcmp(x->name, y->name);
}

static void sort_all(void)
{
    if (sorted) return;
    if (nents > 1) qsort(ents, (size_t)nents, sizeof(cref_entry), cmp_ent);
    sorted = 1;
}

int cref_count(void) { sort_all(); return nents; }

const cref_entry *cref_at(int i)
{
    sort_all();
    return i >= 0 && i < nents ? &ents[i] : NULL;
}

static int word_in(const char *list, const char *w)
{
    size_t n = strlen(w);
    const char *p = list;
    while (p && *p) {
        while (*p == ' ') p++;
        if (!strncmp(p, w, n) && (p[n] == ' ' || p[n] == 0)) return 1;
        while (*p && *p != ' ') p++;
    }
    return 0;
}

const cref_entry *cref_find(const char *name)
{
    int i;
    sort_all();
    if (!name || !*name) return NULL;
    for (i = 0; i < nents; i++)
        if (!strcmp(ents[i].name, name)) return &ents[i];
    for (i = 0; i < nents; i++)
        if (word_in(ents[i].also, name)) return &ents[i];
    for (i = 0; i < nents; i++)
        if (!strcasecmp(ents[i].name, name)) return &ents[i];
    return NULL;
}

int cref_search(const char *word, const cref_entry **out, int max)
{
    int i, n = 0, pass;
    sort_all();
    if (!word || !*word) return 0;
    for (pass = 0; pass < 3 && n < max; pass++)
        for (i = 0; i < nents && n < max; i++) {
            const cref_entry *e = &ents[i];
            int hit;
            if (pass == 0) hit = strcasestr(e->name, word) != NULL || strcasestr(e->also, word) != NULL;
            else if (pass == 1) hit = !(strcasestr(e->name, word) || strcasestr(e->also, word)) &&
                                       (strcasestr(e->summary, word) || strcasestr(e->syntax, word));
            else hit = !(strcasestr(e->name, word) || strcasestr(e->also, word) || strcasestr(e->summary, word) ||
                         strcasestr(e->syntax, word)) && strcasestr(e->desc, word);
            if (hit) out[n++] = e;
        }
    return n;
}

int cref_headers(const char **out, int max)
{
    int i, n = 0, j;
    sort_all();
    for (i = 0; i < nents; i++) {
        for (j = 0; j < n; j++) if (!strcmp(out[j], ents[i].header)) break;
        if (j == n && n < max) out[n++] = ents[i].header;
    }
    return n;
}

/* ---------- সাজিয়ে লেখা ---------- */

typedef struct { char *s; size_t n, cap; } sbuf;

static void sb_add(sbuf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t nc = (b->n + n + 1) * 2;
        char *p = (char *)realloc(b->s, nc);
        if (!p) return;
        b->s = p;
        b->cap = nc;
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}

static void sb_str(sbuf *b, const char *s) { sb_add(b, s, strlen(s)); }

/* indent স্পেস দিয়ে, width-এ শব্দ ভেঙে; ফাঁকা লাইন অনুচ্ছেদ আলাদা রাখে, স্পেস দিয়ে শুরু লাইন যেমন আছে */
static void sb_wrap(sbuf *b, const char *text, int indent, int width)
{
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[1024];
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = 0;
        if (line[0] == ' ' || line[0] == '\t' || (int)len + indent <= width) {
            int k;
            for (k = 0; k < indent; k++) sb_str(b, " ");
            sb_str(b, line);
            sb_str(b, "\n");
        } else {
            char *w = line;
            int col = 0;
            while (*w) {
                char *e = w;
                int wl;
                while (*e && *e != ' ') e++;
                wl = (int)(e - w);
                if (col > 0 && indent + col + 1 + wl > width) {
                    sb_str(b, "\n");
                    col = 0;
                }
                if (col == 0) {
                    int k;
                    for (k = 0; k < indent; k++) sb_str(b, " ");
                } else {
                    sb_str(b, " ");
                    col++;
                }
                sb_add(b, w, (size_t)wl);
                col += wl;
                w = e;
                while (*w == ' ') w++;
            }
            sb_str(b, "\n");
        }
        p = eol ? eol + 1 : p + strlen(p);
    }
}

char *cref_format(const cref_entry *e, int width)
{
    sbuf b;
    char head[256];
    if (!e) return NULL;
    if (width < 40) width = 40;
    memset(&b, 0, sizeof(b));
    if (strcmp(e->header, "language"))
        snprintf(head, sizeof(head), "%s  -  <%s>\n", e->name, e->header);
    else
        snprintf(head, sizeof(head), "%s  -  C language\n", e->name);
    sb_str(&b, head);
    if (*e->summary) sb_wrap(&b, e->summary, 2, width);
    if (*e->also) {
        sb_str(&b, "Also:     ");
        sb_str(&b, e->also);
        sb_str(&b, "\n");
    }
    if (*e->syntax) {
        sb_str(&b, "\nSyntax:\n");
        sb_wrap(&b, e->syntax, 4, 1000);
    }
    if (*e->desc) {
        sb_str(&b, "\nDescription:\n");
        sb_wrap(&b, e->desc, 2, width);
    }
    if (*e->returns) {
        sb_str(&b, "\nReturns:\n");
        sb_wrap(&b, e->returns, 2, width);
    }
    if (*e->see) {
        sb_str(&b, "\nSee also: ");
        sb_str(&b, e->see);
        sb_str(&b, "\n");
    }
    if (*e->example) {
        sb_str(&b, "\nExample:\n");
        sb_wrap(&b, e->example, 4, 1000);
    }
    return b.s;
}
