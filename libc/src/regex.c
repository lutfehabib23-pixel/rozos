/* rozlibc — POSIX regex (ধাপ ১৬ক)
 *
 * regcomp প্যাটার্নকে একটা ছোট গাছে (node-এর তালিকা) বদলায়; regexec প্রতিটা শুরুর
 * জায়গা থেকে backtracking করে মিল খোঁজে। POSIX-এর নিয়মে সবচেয়ে বাঁয়ের মিল, আর তার
 * মধ্যে সবচেয়ে লম্বাটা — সব সম্ভাবনা দেখে (ধাপের একটা সীমা আছে; সীমা পেরোলে প্রথম
 * পাওয়া মিলেই থামে, যাতে খারাপ প্যাটার্নে আটকে না যায়)।
 *
 * BRE: \( \) \{ \} \1-\9, * (শুরুতে আক্ষরিক), GNU-র \+ \? \|
 * ERE: ( ) { } | + ? *, \1-\9 (GNU)
 * দুটোতেই: . [..] [^..] [a-z] [[:alpha:]] ^ $ \w \W \s \S \b \B \< \> \` \'
 * এক অক্ষর = এক বাইট (UTF-8-এর বহু-বাইট অক্ষর '.'-এ বাইট ধরে মেলে)। */

#include <regex.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

#define RE_DUP_MAX_ROZ 255

enum {
    N_EMPTY, N_CHAR, N_ANY, N_SET, N_BOL, N_EOL, N_WORDB, N_NWORDB, N_WBEG, N_WEND,
    N_BUFBEG, N_BUFEND, N_GROUP, N_BREF, N_CAT, N_ALT, N_REP
};

struct node {
    int type;
    int a, b;                    /* CHAR: অক্ষর; GROUP/BREF: দল নং; REP: min, max (-1 = অসীম) */
    int child;                   /* GROUP/REP */
    int nkids, capkids;          /* CAT/ALT */
    int *kids;
    unsigned char *set;          /* SET: ২৫৬ বিট (৩২ বাইট) */
};

struct prog {
    struct node *n;
    int count, cap;
    int root;
    int nsub;
    int flags;
};

/* ---------- গাছ বানানো ---------- */

static int new_node(struct prog *p, int type)
{
    if (p->count == p->cap) {
        int nc = p->cap ? p->cap * 2 : 32;
        struct node *nn = (struct node *)realloc(p->n, sizeof(struct node) * (size_t)nc);
        if (!nn) return -1;
        p->n = nn;
        p->cap = nc;
    }
    memset(&p->n[p->count], 0, sizeof(struct node));
    p->n[p->count].type = type;
    p->n[p->count].child = -1;
    return p->count++;
}

static int add_kid(struct prog *p, int parent, int kid)
{
    struct node *n = &p->n[parent];
    if (n->nkids == n->capkids) {
        int nc = n->capkids ? n->capkids * 2 : 4;
        int *nk = (int *)realloc(n->kids, sizeof(int) * (size_t)nc);
        if (!nk) return -1;
        n->kids = nk;
        n->capkids = nc;
    }
    n->kids[n->nkids++] = kid;
    return 0;
}

struct parser {
    struct prog *p;
    const char *s;
    int ere, icase, err;
    int depth;
};

#define SET_ADD(set, c) ((set)[(unsigned char)(c) >> 3] |= (unsigned char)(1 << ((unsigned char)(c) & 7)))
#define SET_HAS(set, c) ((set)[(unsigned char)(c) >> 3] & (1 << ((unsigned char)(c) & 7)))

static int class_match(const char *name, size_t len, int c)
{
#define CL(nm, fn) if (len == sizeof(nm) - 1 && !strncmp(name, nm, len)) return fn(c) != 0
    CL("alpha", isalpha); CL("digit", isdigit); CL("alnum", isalnum); CL("upper", isupper);
    CL("lower", islower); CL("space", isspace); CL("blank", isblank); CL("punct", ispunct);
    CL("print", isprint); CL("graph", isgraph); CL("cntrl", iscntrl); CL("xdigit", isxdigit);
#undef CL
    return -1;
}

static void set_case(unsigned char *set)
{
    int c;
    for (c = 'a'; c <= 'z'; c++) {
        if (SET_HAS(set, c) || SET_HAS(set, c - 32)) { SET_ADD(set, c); SET_ADD(set, c - 32); }
    }
}

static int make_set(struct parser *ps, int negate, unsigned char *bits)
{
    int id = new_node(ps->p, N_SET);
    int i;
    if (id < 0) { ps->err = REG_ESPACE; return -1; }
    ps->p->n[id].set = (unsigned char *)malloc(32);
    if (!ps->p->n[id].set) { ps->err = REG_ESPACE; return -1; }
    if (ps->icase) set_case(bits);
    if (negate) {
        for (i = 0; i < 32; i++) bits[i] = (unsigned char)~bits[i];
        bits[0] &= (unsigned char)~1;                       /* \0 কখনো নয় */
        if (ps->p->flags & REG_NEWLINE) bits['\n' >> 3] &= (unsigned char)~(1 << ('\n' & 7));
    }
    memcpy(ps->p->n[id].set, bits, 32);
    return id;
}

/* '[' এর পরে থেকে */
static int parse_bracket(struct parser *ps)
{
    unsigned char bits[32];
    int negate = 0, first = 1, prev = -1;
    memset(bits, 0, sizeof(bits));
    if (*ps->s == '^') { negate = 1; ps->s++; }
    for (;;) {
        int c = (unsigned char)*ps->s;
        if (!c) { ps->err = REG_EBRACK; return -1; }
        if (c == ']' && !first) { ps->s++; break; }
        first = 0;
        if (c == '[' && (ps->s[1] == ':' || ps->s[1] == '=' || ps->s[1] == '.')) {
            char kind = ps->s[1];
            const char *start = ps->s + 2, *end = strchr(start, kind);
            if (!end || end[1] != ']') { ps->err = REG_EBRACK; return -1; }
            if (kind == ':') {
                int ch;
                if (class_match(start, (size_t)(end - start), 'a') < 0) { ps->err = REG_ECTYPE; return -1; }
                for (ch = 1; ch < 256; ch++) if (class_match(start, (size_t)(end - start), ch) > 0) SET_ADD(bits, ch);
                prev = -1;
            } else {
                /* [=x=] ও [.x.]: এক অক্ষরের রূপ */
                if (end - start != 1) { ps->err = REG_ECOLLATE; return -1; }
                SET_ADD(bits, *start);
                prev = (unsigned char)*start;
            }
            ps->s = end + 2;
            continue;
        }
        if (c == '-' && prev >= 0 && ps->s[1] && ps->s[1] != ']') {
            int hi = (unsigned char)ps->s[1], ch;
            if (hi == '[' && ps->s[2] == '.') {
                const char *e = strchr(ps->s + 3, '.');
                if (!e || e - (ps->s + 3) != 1) { ps->err = REG_ECOLLATE; return -1; }
                hi = (unsigned char)ps->s[3];
                ps->s = e + 2;
            } else {
                ps->s += 2;
            }
            if (hi < prev) { ps->err = REG_ERANGE; return -1; }
            for (ch = prev; ch <= hi; ch++) SET_ADD(bits, ch);
            prev = -1;
            continue;
        }
        SET_ADD(bits, c);
        prev = c;
        ps->s++;
    }
    return make_set(ps, negate, bits);
}

static int char_node(struct parser *ps, int c)
{
    int id;
    if (ps->icase && isalpha(c)) {
        unsigned char bits[32];
        memset(bits, 0, sizeof(bits));
        SET_ADD(bits, tolower(c));
        SET_ADD(bits, toupper(c));
        return make_set(ps, 0, bits);
    }
    id = new_node(ps->p, N_CHAR);
    if (id < 0) { ps->err = REG_ESPACE; return -1; }
    ps->p->n[id].a = c;
    return id;
}

static int class_node(struct parser *ps, int kind)
{
    unsigned char bits[32];
    int c, neg = isupper(kind);
    memset(bits, 0, sizeof(bits));
    for (c = 1; c < 256; c++) {
        int in = (tolower(kind) == 'w') ? (isalnum(c) || c == '_') : isspace(c);
        if (in) SET_ADD(bits, c);
    }
    return make_set(ps, neg, bits);
}

static int parse_alt(struct parser *ps);

/* একটা atom (গুণক ছাড়া); *done = 1 হলে এখানে sequence শেষ */
static int parse_atom(struct parser *ps, int at_start, int *done)
{
    int c = (unsigned char)*ps->s, id;
    *done = 0;
    if (!c) { *done = 1; return -1; }
    if (ps->ere) {
        if (c == '|' || c == ')') { *done = 1; return -1; }
        if (c == '(') {
            int g = ++ps->p->nsub, inner;
            ps->s++;
            if (++ps->depth > 200) { ps->err = REG_ESPACE; return -1; }
            inner = parse_alt(ps);
            ps->depth--;
            if (ps->err) return -1;
            if (*ps->s != ')') { ps->err = REG_EPAREN; return -1; }
            ps->s++;
            id = new_node(ps->p, N_GROUP);
            if (id < 0) { ps->err = REG_ESPACE; return -1; }
            ps->p->n[id].a = g;
            ps->p->n[id].child = inner;
            return id;
        }
        if (c == '*' || c == '+' || c == '?' || c == '{') {
            if (at_start && c != '{') { ps->err = REG_BADRPT; return -1; }
            if (c == '{' && !isdigit((unsigned char)ps->s[1])) { ps->s++; return char_node(ps, '{'); }
            ps->err = REG_BADRPT;
            return -1;
        }
    } else {
        if (c == '\\' && (ps->s[1] == ')' || ps->s[1] == '|')) { *done = 1; return -1; }
        if (c == '\\' && ps->s[1] == '(') {
            int g = ++ps->p->nsub, inner;
            ps->s += 2;
            if (++ps->depth > 200) { ps->err = REG_ESPACE; return -1; }
            inner = parse_alt(ps);
            ps->depth--;
            if (ps->err) return -1;
            if (ps->s[0] != '\\' || ps->s[1] != ')') { ps->err = REG_EPAREN; return -1; }
            ps->s += 2;
            id = new_node(ps->p, N_GROUP);
            if (id < 0) { ps->err = REG_ESPACE; return -1; }
            ps->p->n[id].a = g;
            ps->p->n[id].child = inner;
            return id;
        }
        if (c == '*' && at_start) { ps->s++; return char_node(ps, '*'); }
    }
    if (c == '^') {
        /* ERE-তে সবখানে anchor; BRE-তে শুধু শুরুতে, নইলে আক্ষরিক */
        ps->s++;
        if (ps->ere || at_start) return new_node(ps->p, N_BOL);
        return char_node(ps, '^');
    }
    if (c == '$') {
        ps->s++;
        if (ps->ere || !*ps->s || (ps->s[0] == '\\' && (ps->s[1] == ')' || ps->s[1] == '|')))
            return new_node(ps->p, N_EOL);
        return char_node(ps, '$');
    }
    if (c == '.') {
        ps->s++;
        return new_node(ps->p, N_ANY);
    }
    if (c == '[') {
        ps->s++;
        return parse_bracket(ps);
    }
    if (c == '\\') {
        int e = (unsigned char)ps->s[1];
        if (!e) { ps->err = REG_EESCAPE; return -1; }
        ps->s += 2;
        if (e >= '1' && e <= '9') {
            if (e - '0' > ps->p->nsub) { ps->err = REG_ESUBREG; return -1; }
            id = new_node(ps->p, N_BREF);
            if (id < 0) { ps->err = REG_ESPACE; return -1; }
            ps->p->n[id].a = e - '0';
            return id;
        }
        switch (e) {
        case 'w': case 'W': case 's': case 'S': return class_node(ps, e);
        case 'b': return new_node(ps->p, N_WORDB);
        case 'B': return new_node(ps->p, N_NWORDB);
        case '<': return new_node(ps->p, N_WBEG);
        case '>': return new_node(ps->p, N_WEND);
        case '`': return new_node(ps->p, N_BUFBEG);
        case '\'': return new_node(ps->p, N_BUFEND);
        case 'n': return char_node(ps, '\n');
        case 't': return char_node(ps, '\t');
        }
        return char_node(ps, e);
    }
    ps->s++;
    return char_node(ps, c);
}

/* {m}, {m,}, {m,n} — s '{' (ERE) বা "\{" (BRE)-এর পরে */
static int parse_bound(struct parser *ps, int *mn, int *mx)
{
    int a = 0, b, have = 0;
    while (isdigit((unsigned char)*ps->s)) { a = a * 10 + (*ps->s++ - '0'); have = 1; if (a > RE_DUP_MAX_ROZ) break; }
    if (!have) return REG_BADBR;
    b = a;
    if (*ps->s == ',') {
        ps->s++;
        if (isdigit((unsigned char)*ps->s)) {
            b = 0;
            while (isdigit((unsigned char)*ps->s)) { b = b * 10 + (*ps->s++ - '0'); if (b > RE_DUP_MAX_ROZ) break; }
        } else b = -1;
    }
    if (ps->ere) {
        if (*ps->s != '}') return REG_EBRACE;
        ps->s++;
    } else {
        if (ps->s[0] != '\\' || ps->s[1] != '}') return REG_EBRACE;
        ps->s += 2;
    }
    if (a > RE_DUP_MAX_ROZ || b > RE_DUP_MAX_ROZ || (b >= 0 && b < a)) return REG_BADBR;
    *mn = a;
    *mx = b;
    return 0;
}

static int parse_seq(struct parser *ps)
{
    int cat = new_node(ps->p, N_CAT), at_start = 1;
    if (cat < 0) { ps->err = REG_ESPACE; return -1; }
    for (;;) {
        int done, atom = parse_atom(ps, at_start, &done);
        if (ps->err) return -1;
        if (done) break;
        if (atom < 0) { ps->err = REG_ESPACE; return -1; }
        /* BRE-তে ^-এর পরের * আক্ষরিক */
        at_start = !ps->ere && ps->p->n[atom].type == N_BOL;
        for (;;) {
            int mn = -2, mx = -2, c = (unsigned char)*ps->s, rep;
            if (!ps->ere && ps->p->n[atom].type == N_BOL) break;      /* BRE: ^* = ^ আর আক্ষরিক * */
            if (c == '*') { mn = 0; mx = -1; ps->s++; }
            else if (ps->ere && c == '+') { mn = 1; mx = -1; ps->s++; }
            else if (ps->ere && c == '?') { mn = 0; mx = 1; ps->s++; }
            else if (!ps->ere && c == '\\' && ps->s[1] == '+') { mn = 1; mx = -1; ps->s += 2; }
            else if (!ps->ere && c == '\\' && ps->s[1] == '?') { mn = 0; mx = 1; ps->s += 2; }
            else if (ps->ere && c == '{' && isdigit((unsigned char)ps->s[1])) {
                int e;
                ps->s++;
                if ((e = parse_bound(ps, &mn, &mx))) { ps->err = e; return -1; }
            } else if (!ps->ere && c == '\\' && ps->s[1] == '{') {
                int e;
                ps->s += 2;
                if ((e = parse_bound(ps, &mn, &mx))) { ps->err = e; return -1; }
            } else break;
            if (ps->p->n[atom].type == N_BOL || ps->p->n[atom].type == N_EOL) {
                if (ps->ere) { ps->err = REG_BADRPT; return -1; }
            }
            rep = new_node(ps->p, N_REP);
            if (rep < 0) { ps->err = REG_ESPACE; return -1; }
            ps->p->n[rep].a = mn;
            ps->p->n[rep].b = mx;
            ps->p->n[rep].child = atom;
            atom = rep;
            at_start = 0;
        }
        if (add_kid(ps->p, cat, atom) < 0) { ps->err = REG_ESPACE; return -1; }
    }
    return cat;
}

static int parse_alt(struct parser *ps)
{
    int first = parse_seq(ps), alt;
    if (ps->err) return -1;
    if (!((ps->ere && *ps->s == '|') || (!ps->ere && ps->s[0] == '\\' && ps->s[1] == '|'))) return first;
    alt = new_node(ps->p, N_ALT);
    if (alt < 0 || add_kid(ps->p, alt, first) < 0) { ps->err = REG_ESPACE; return -1; }
    while ((ps->ere && *ps->s == '|') || (!ps->ere && ps->s[0] == '\\' && ps->s[1] == '|')) {
        int k;
        ps->s += ps->ere ? 1 : 2;
        k = parse_seq(ps);
        if (ps->err) return -1;
        if (add_kid(ps->p, alt, k) < 0) { ps->err = REG_ESPACE; return -1; }
    }
    return alt;
}

static void prog_free(struct prog *p)
{
    int i;
    if (!p) return;
    for (i = 0; i < p->count; i++) {
        free(p->n[i].kids);
        free(p->n[i].set);
    }
    free(p->n);
    free(p);
}

int regcomp(regex_t *re, const char *pattern, int flags)
{
    struct parser ps;
    struct prog *p = (struct prog *)calloc(1, sizeof(*p));
    if (!p) return REG_ESPACE;
    p->flags = flags;
    memset(&ps, 0, sizeof(ps));
    ps.p = p;
    ps.s = pattern;
    ps.ere = (flags & REG_EXTENDED) != 0;
    ps.icase = (flags & REG_ICASE) != 0;
    p->root = parse_alt(&ps);
    if (!ps.err && *ps.s) ps.err = REG_EPAREN;                        /* বাড়তি ')' */
    if (ps.err) {
        prog_free(p);
        return ps.err;
    }
    re->re_nsub = (size_t)p->nsub;
    re->__prog = p;
    re->__flags = flags;
    return 0;
}

void regfree(regex_t *re)
{
    prog_free((struct prog *)re->__prog);
    re->__prog = NULL;
}

/* ---------- মিল খোঁজা ---------- */

#define MAX_GROUPS 32
#define STEP_LIMIT 400000L

struct kont {
    int node;
    int idx;                     /* CAT: পরের সন্তান; REP: এ পর্যন্ত কয়বার; GROUP-শেষ: -1 */
    const char *mark;            /* REP: এই পুনরাবৃত্তি কোথায় শুরু */
    const struct kont *next;
};

struct mctx {
    const struct prog *p;
    const char *base, *start, *end;
    int eflags, longest;
    long steps;
    long so[MAX_GROUPS + 1], eo[MAX_GROUPS + 1];
    long bso[MAX_GROUPS + 1], beo[MAX_GROUPS + 1];
    const char *best;
};

static int is_word(int c) { return isalnum(c) || c == '_'; }

static int run(struct mctx *c, int ni, const char *s, const struct kont *k);

static int next(struct mctx *c, const char *s, const struct kont *k)
{
    const struct node *n;
    if (!k) {
        if (!c->best || s > c->best) {
            c->best = s;
            memcpy(c->bso, c->so, sizeof(c->so));
            memcpy(c->beo, c->eo, sizeof(c->eo));
        }
        return 1;
    }
    n = &c->p->n[k->node];
    if (n->type == N_CAT) {
        struct kont kk;
        if (k->idx + 1 < n->nkids) {
            kk.node = k->node;
            kk.idx = k->idx + 1;
            kk.mark = NULL;
            kk.next = k->next;
            return run(c, n->kids[k->idx], s, &kk);
        }
        return run(c, n->kids[k->idx], s, k->next);
    }
    if (n->type == N_GROUP) {
        long old = c->eo[n->a];
        int r;
        c->eo[n->a] = (long)(s - c->base);
        r = next(c, s, k->next);
        c->eo[n->a] = old;
        return r;
    }
    if (n->type == N_REP) {
        int count = k->idx;
        if (s == k->mark) {
            /* খালি পুনরাবৃত্তি: আর ঘোরা নয় (অসীম লুপ এড়াতে) — বাকি ন্যূনতমগুলোও খালি
             * ধরে নেওয়া যায়, তাই সোজা পরের অংশে */
            return next(c, s, k->next);
        }
        {
            /* আরও পুনরাবৃত্তি (লোভী), তারপর এখানে থামা */
            int r = 0;
            if (n->b < 0 || count < n->b) {
                struct kont kk;
                kk.node = k->node;
                kk.idx = count + 1;
                kk.mark = s;
                kk.next = k->next;
                r = run(c, n->child, s, &kk);
                if (r && !c->longest) return 1;
            }
            if (count >= n->a) r |= next(c, s, k->next);
            return r;
        }
    }
    return 0;
}

static int single_match(const struct mctx *c, const struct node *n, int ch)
{
    if (n->type == N_CHAR) return ch == n->a;
    if (n->type == N_ANY) return ch != '\0' && !(ch == '\n' && (c->p->flags & REG_NEWLINE));
    if (n->type == N_SET) return SET_HAS(n->set, ch) != 0;
    return 0;
}

static int run(struct mctx *c, int ni, const char *s, const struct kont *k)
{
    const struct node *n;
    if (++c->steps > STEP_LIMIT) c->longest = 0;      /* বেশি সময় লাগছে: প্রথম মিলেই থামা */
    if (ni < 0) return next(c, s, k);
    n = &c->p->n[ni];
    switch (n->type) {
    case N_EMPTY:
        return next(c, s, k);
    case N_CHAR:
    case N_ANY:
    case N_SET:
        if (s < c->end && single_match(c, n, (unsigned char)*s)) return next(c, s + 1, k);
        return 0;
    case N_BOL:
        if ((s == c->start && !(c->eflags & REG_NOTBOL)) ||
            ((c->p->flags & REG_NEWLINE) && s > c->base && s[-1] == '\n')) return next(c, s, k);
        return 0;
    case N_EOL:
        if ((s == c->end && !(c->eflags & REG_NOTEOL)) ||
            ((c->p->flags & REG_NEWLINE) && s < c->end && *s == '\n')) return next(c, s, k);
        return 0;
    case N_BUFBEG:
        return s == c->start ? next(c, s, k) : 0;
    case N_BUFEND:
        return s == c->end ? next(c, s, k) : 0;
    case N_WORDB:
    case N_NWORDB:
    case N_WBEG:
    case N_WEND: {
        int before = s > c->base && is_word((unsigned char)s[-1]);
        int after = s < c->end && is_word((unsigned char)*s);
        int ok = n->type == N_WORDB ? before != after : n->type == N_NWORDB ? before == after :
                 n->type == N_WBEG ? (!before && after) : (before && !after);
        return ok ? next(c, s, k) : 0;
    }
    case N_BREF: {
        long so = c->so[n->a], eo = c->eo[n->a], len;
        if (so < 0 || eo < 0) return 0;
        len = eo - so;
        if (c->end - s < len) return 0;
        if (c->p->flags & REG_ICASE) {
            long i;
            for (i = 0; i < len; i++)
                if (tolower((unsigned char)s[i]) != tolower((unsigned char)c->base[so + i])) return 0;
        } else if (memcmp(s, c->base + so, (size_t)len)) return 0;
        return next(c, s + len, k);
    }
    case N_GROUP: {
        long oso = c->so[n->a], oeo = c->eo[n->a];
        struct kont kk;
        int r;
        c->so[n->a] = (long)(s - c->base);
        kk.node = ni;
        kk.idx = -1;
        kk.mark = NULL;
        kk.next = k;
        r = run(c, n->child, s, &kk);
        c->so[n->a] = oso;
        c->eo[n->a] = oeo;
        return r;
    }
    case N_CAT: {
        struct kont kk;
        if (n->nkids == 0) return next(c, s, k);
        if (n->nkids == 1) return run(c, n->kids[0], s, k);
        kk.node = ni;
        kk.idx = 1;
        kk.mark = NULL;
        kk.next = k;
        return run(c, n->kids[0], s, &kk);
    }
    case N_ALT: {
        int i, any = 0;
        for (i = 0; i < n->nkids; i++) {
            if (run(c, n->kids[i], s, k)) {
                any = 1;
                if (!c->longest) return 1;
            }
        }
        return any;
    }
    case N_REP: {
        const struct node *ch = &c->p->n[n->child];
        if (ch->type == N_CHAR || ch->type == N_ANY || ch->type == N_SET) {
            /* এক-অক্ষরের পুনরাবৃত্তি: আগে সর্বোচ্চ কতদূর মেলে গুনে, পেছন থেকে চেষ্টা
             * (প্রতি অক্ষরে নতুন stack frame লাগে না) */
            long maxn = n->b < 0 ? (long)(c->end - s) : n->b, cnt = 0, i;
            int any = 0;
            while (cnt < maxn && s + cnt < c->end && single_match(c, ch, (unsigned char)s[cnt])) cnt++;
            if (cnt < n->a) return 0;
            for (i = cnt; i >= n->a; i--) {
                if (next(c, s + i, k)) {
                    any = 1;
                    if (!c->longest) return 1;
                }
                if (++c->steps > STEP_LIMIT) c->longest = 0;
            }
            return any;
        }
        {
            int r = 0;
            if (n->b != 0) {
                struct kont kk;
                kk.node = ni;
                kk.idx = 1;
                kk.mark = s;
                kk.next = k;
                r = run(c, n->child, s, &kk);
                if (r && !c->longest) return 1;
            }
            if (n->a == 0) r |= next(c, s, k);
            return r;
        }
    }
    }
    return 0;
}

int regexec(const regex_t *re, const char *str, size_t nmatch, regmatch_t m[], int eflags)
{
    const struct prog *p = (const struct prog *)re->__prog;
    struct mctx c;
    const char *s;
    size_t i;
    if (!p) return REG_BADPAT;
    if (p->flags & REG_NOSUB) nmatch = 0;
    memset(&c, 0, sizeof(c));
    c.p = p;
    c.base = str;
    c.eflags = eflags;
    if ((eflags & REG_STARTEND) && m) {
        c.start = str + m[0].rm_so;
        c.end = str + m[0].rm_eo;
    } else {
        c.start = str;
        c.end = str + strlen(str);
    }
    for (s = c.start; s <= c.end; s++) {
        int g;
        for (g = 0; g <= MAX_GROUPS; g++) c.so[g] = c.eo[g] = -1;
        c.best = NULL;
        c.longest = 1;
        c.steps = 0;
        run(&c, p->root, s, NULL);
        if (c.best) {
            if (nmatch) {
                m[0].rm_so = (regoff_t)(s - str);
                m[0].rm_eo = (regoff_t)(c.best - str);
                for (i = 1; i < nmatch; i++) {
                    if (i <= MAX_GROUPS && (int)i <= p->nsub) {
                        m[i].rm_so = c.bso[i];
                        m[i].rm_eo = c.bso[i] < 0 ? -1 : c.beo[i];
                    } else {
                        m[i].rm_so = m[i].rm_eo = -1;
                    }
                }
            }
            return 0;
        }
        if (s == c.end) break;
        /* প্যাটার্ন ^ দিয়ে শুরু আর REG_NEWLINE নেই: শুধু প্রথম জায়গাতেই মিলতে পারে */
    }
    return REG_NOMATCH;
}

size_t regerror(int err, const regex_t *re, char *buf, size_t size)
{
    static const char *const msgs[] = {
        "Success", "No match", "Invalid regular expression", "Invalid collation character",
        "Invalid character class name", "Trailing backslash", "Invalid back reference",
        "Unmatched [ or [^", "Unmatched ( or \\(", "Unmatched \\{", "Invalid content of \\{\\}",
        "Invalid range end", "Memory exhausted", "Invalid preceding regular expression",
    };
    const char *m = (err >= 0 && err < (int)(sizeof(msgs) / sizeof(msgs[0]))) ? msgs[err] : "Unknown error";
    size_t n = strlen(m) + 1;
    (void)re;
    if (size) {
        size_t k = n < size ? n : size;
        memcpy(buf, m, k - 1);
        buf[k - 1] = '\0';
    }
    return n;
}
