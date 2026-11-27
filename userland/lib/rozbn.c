/* rozOS — rozbn: অভ্র Classic Phonetic (ধাপ ২৩গ) — rozbn.h দেখুন
 *
 * রূপান্তরের যুক্তি libavrophonetic-এর rulebasedconv/converter.go-র হুবহু অনুবাদ
 * (MIT, (c) 2021 Mehdi Hasan Khan): নিয়ম লম্বা থেকে ছোট ক্রমে; প্রতি অবস্থানে প্রথম যে
 * নিয়মের match মেলে, তার ব্যতিক্রম (আগে/পরে স্বরবর্ণ, ব্যঞ্জন, চিহ্ন, নির্দিষ্ট লেখা)
 * দেখে replace বা thenReplace। নিয়মের টেবিল rozbn_rules.h (toolchain/gen-avro-rules.py)। */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rozbn.h"
#include "rozgfx.h"

struct av_cond { int suffix, kind, neg; const char *value; };
struct av_exc  { int first, count; const char *then; };
struct av_rule { const char *match, *replace; int first_exc, n_exc; };
#include "rozbn_rules.h"

#define MODE_FILE "/tmp/.rozbn-mode"

static int order[AV_NRULES];
static int mlen[AV_NRULES];
static int sorted;

static int cmp_len(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    if (mlen[x] != mlen[y]) return mlen[y] - mlen[x];        /* লম্বা আগে */
    return x - y;
}

static void sort_rules(void)
{
    int i;
    for (i = 0; i < AV_NRULES; i++) { order[i] = i; mlen[i] = (int)strlen(av_rules[i].match); }
    qsort(order, AV_NRULES, sizeof(order[0]), cmp_len);
    sorted = 1;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int is_vowel(int c) { c = lower(c); return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u'; }
static int is_cons(int c)
{
    c = lower(c);
    return c >= 'a' && c <= 'z' && !is_vowel(c);
}
static int is_punct(int c) { return !is_vowel(c) && !is_cons(c); }

/* বড়/ছোট হাতের তফাত শুধু এই অক্ষরগুলোয়; বাকিগুলো ছোট হাতের করা হয় */
static int case_sensitive(int c)
{
    return strchr("oiudgjnrstyz", lower(c)) != NULL;
}

static int exact(const char *needle, const char *in, int len, int s, int e, int neg)
{
    int n = (int)strlen(needle);
    int ok = s >= 0 && e < len && e - s == n && !memcmp(in + s, needle, (size_t)n);
    return ok != neg;
}

static int cond_ok(const struct av_cond *c, const char *in, int len, int start, int right)
{
    int cur = c->suffix ? right : start - 1, inr;
    switch (c->kind) {
    case AV_PUNCT:
        inr = (cur < 0 && !c->suffix) || (cur >= len && c->suffix) || (cur >= 0 && cur < len && is_punct(in[cur]));
        return inr != c->neg;
    case AV_VOWEL:
        inr = ((cur >= 0 && !c->suffix) || (cur < len && c->suffix)) && cur >= 0 && cur < len && is_vowel(in[cur]);
        return inr != c->neg;
    case AV_CONSONANT:
        inr = ((cur >= 0 && !c->suffix) || (cur < len && c->suffix)) && cur >= 0 && cur < len && is_cons(in[cur]);
        return inr != c->neg;
    case AV_EXACT: {
        int n = (int)strlen(c->value);
        if (c->suffix) return exact(c->value, in, len, right, right + n, c->neg);
        return exact(c->value, in, len, start - n, start, c->neg);
    }
    }
    return 1;
}

static void put(char *out, size_t size, size_t *o, const char *s)
{
    size_t n = strlen(s);
    if (*o + n + 1 > size) n = size > *o + 1 ? size - *o - 1 : 0;
    memcpy(out + *o, s, n);
    *o += n;
    out[*o] = '\0';
}

int rozbn_convert(const char *roman, char *out, size_t size)
{
    char in[512];
    int len, i, k;
    size_t o = 0;
    if (!size) return 0;
    out[0] = '\0';
    if (!sorted) sort_rules();
    for (len = 0; roman[len] && len < (int)sizeof(in) - 1; len++)
        in[len] = case_sensitive(roman[len]) ? roman[len] : (char)lower((unsigned char)roman[len]);
    in[len] = '\0';
    for (i = 0; i < len; i++) {
        int done = 0;
        for (k = 0; k < AV_NRULES && !done; k++) {
            const struct av_rule *r = &av_rules[order[k]];
            int right = i + mlen[order[k]], e;
            if (right > len || memcmp(in + i, r->match, (size_t)mlen[order[k]])) continue;
            for (e = 0; e < r->n_exc; e++) {
                const struct av_exc *x = &av_excs[r->first_exc + e];
                int c, all = 1;
                for (c = 0; c < x->count && all; c++)
                    all = cond_ok(&av_conds[x->first + c], in, len, i, right);
                if (all) { put(out, size, &o, x->then); done = 1; break; }
            }
            if (!done) { put(out, size, &o, r->replace); done = 1; }
            i = right - 1;
        }
        if (!done) {
            char one[2] = { in[i], 0 };
            put(out, size, &o, one);
        }
    }
    return (int)o;
}

/* ---------- মোড (সব প্রোগ্রামে একসাথে) ---------- */

int rozbn_mode(void)
{
    FILE *f = fopen(MODE_FILE, "r");
    int c = '0';
    if (f) { c = fgetc(f); fclose(f); }
    return c == '1';
}

void rozbn_set_mode(int on)
{
    FILE *f = fopen(MODE_FILE, "w");
    if (f) { fputc(on ? '1' : '0', f); fclose(f); }
}

int rozbn_toggle(void)
{
    int m = !rozbn_mode();
    rozbn_set_mode(m);
    return m;
}

int rozbn_is_toggle_key(int key, int mods)
{
    return (key == ' ' && (mods & GFX_MOD_CTRL)) || key == 0;
}

/* ---------- টাইপ করার সময় ---------- */

void rozbn_reset(rozbn_comp *c) { c->rlen = 0; c->roman[0] = '\0'; c->out_len = 0; }

/* অভ্রের নিয়মে যে অক্ষরগুলো আছে (অক্ষর, সংখ্যা আর কয়েকটা চিহ্ন) */
static int roman_key(int k)
{
    return (k >= 'a' && k <= 'z') || (k >= 'A' && k <= 'Z') || (k >= '0' && k <= '9') ||
           k == '.' || k == '^' || k == ':' || k == '$' || k == '`';
}

int rozbn_key(rozbn_comp *c, int key, int *del, char *ins, size_t size)
{
    *del = 0;
    if (size) ins[0] = '\0';
    if (key == '\b' && c->rlen > 0) {
        c->roman[--c->rlen] = '\0';
    } else if (roman_key(key) && c->rlen < (int)sizeof(c->roman) - 1) {
        c->roman[c->rlen++] = (char)key;
        c->roman[c->rlen] = '\0';
    } else {
        rozbn_reset(c);                      /* শব্দ শেষ — key সাধারণভাবে বসবে */
        return 0;
    }
    *del = c->out_len;
    c->out_len = rozbn_convert(c->roman, ins, size);
    if (c->rlen == 0) c->out_len = 0;
    return 1;
}
