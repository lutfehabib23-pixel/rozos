/* rozlibc — getopt, getopt_long, getopt_long_only (ধাপ ১৬ক; আগের getopt stdlib.c-তে ছিল)
 *
 * POSIX: "ab:c::" (:: = ঐচ্ছিক মান), শুরুতে ':' হলে ভুলে বার্তা নয়, ':' ফেরত মান না
 * থাকলে। GNU: শুরুতে '+' = প্রথম অ-অপশনে থামা (POSIXLY_CORRECT-ও তাই), '-' = অ-অপশন
 * আর্গুমেন্ট 1 হিসেবে ফেরত; ডিফল্টে অ-অপশনগুলো শেষে সরানো (permute), যেমন
 * `ls dir -l`। লম্বা অপশন: --name, --name=value, --name value, অনন্য হলে সংক্ষেপ (--verb)।
 * optind = 0 বসালে (বা optreset = 1) আবার শুরু থেকে। */

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *optarg;
int optind = 1, opterr = 1, optopt;
int optreset;

static int nextchar;                     /* বর্তমান "-abc" গুচ্ছে কোন অক্ষর */
static int first_nonopt = 1, last_nonopt = 1;   /* permute: সরানো অ-অপশনগুলোর সীমা */

/* [first_nonopt, last_nonopt) আর [last_nonopt, optind) অদলবদল (অ-অপশনগুলো পরে যায়) */
static void permute(char **argv)
{
    int a = first_nonopt, b = last_nonopt, c = optind, i;
    int n1 = b - a, n2 = c - b;
    char *tmp;
    /* ছোট তালিকা: ঘুরিয়ে ঘুরিয়ে (rotation), অতিরিক্ত মেমরি ছাড়া */
    for (i = 0; i < n2; i++) {
        int j;
        tmp = argv[b + i];
        for (j = b + i; j > a + i; j--) argv[j] = argv[j - 1];
        argv[a + i] = tmp;
    }
    (void)n1;
    first_nonopt += n2;
    last_nonopt = optind;
}

static int is_opt(const char *s) { return s[0] == '-' && s[1] != '\0'; }

static int msg(char *const argv[], const char *fmt, const char *what, int c)
{
    if (opterr) {
        fprintf(stderr, "%s: ", argv[0]);
        if (what) fprintf(stderr, fmt, what);
        else fprintf(stderr, fmt, c);
        fputc('\n', stderr);
    }
    return 0;
}

static int do_long(int argc, char *const argv[], const char *opts, const struct option *lo,
                   int *idx, const char *name, int colon_mode)
{
    const char *eq = strchr(name, '=');
    size_t len = eq ? (size_t)(eq - name) : strlen(name);
    int i, found = -1, ambig = 0;
    (void)opts;
    for (i = 0; lo[i].name; i++) {
        if (strncmp(lo[i].name, name, len)) continue;
        if (strlen(lo[i].name) == len) { found = i; ambig = 0; break; }      /* ঠিক মিল */
        if (found < 0) found = i;
        else if (lo[found].has_arg != lo[i].has_arg || lo[found].flag != lo[i].flag ||
                 lo[found].val != lo[i].val) ambig = 1;
    }
    optind++;
    if (ambig) {
        msg(argv, "option '--%s' is ambiguous", name, 0);
        optopt = 0;
        return '?';
    }
    if (found < 0) {
        msg(argv, "unrecognized option '--%s'", name, 0);
        optopt = 0;
        return '?';
    }
    if (idx) *idx = found;
    optarg = NULL;
    if (lo[found].has_arg == no_argument) {
        if (eq) {
            msg(argv, "option '--%s' doesn't allow an argument", lo[found].name, 0);
            optopt = lo[found].val;
            return '?';
        }
    } else if (eq) {
        optarg = (char *)eq + 1;
    } else if (lo[found].has_arg == required_argument) {
        if (optind < argc) optarg = argv[optind++];
        else {
            msg(argv, "option '--%s' requires an argument", lo[found].name, 0);
            optopt = lo[found].val;
            return colon_mode ? ':' : '?';
        }
    }
    if (lo[found].flag) {
        *lo[found].flag = lo[found].val;
        return 0;
    }
    return lo[found].val;
}

static int getopt_core(int argc, char *const argv[], const char *opts,
                       const struct option *lo, int *idx, int long_only)
{
    char **av = (char **)argv;
    int mode = 0, colon_mode, c;
    const char *o, *cur;

    if (optind == 0 || optreset) {
        optind = 1;
        optreset = 0;
        nextchar = 0;
        first_nonopt = last_nonopt = 1;
    }
    if (*opts == '+' || *opts == '-') mode = *opts++;
    else if (getenv("POSIXLY_CORRECT")) mode = '+';
    colon_mode = *opts == ':';
    if (colon_mode) opts++;
    optarg = NULL;

    if (!nextchar) {
        /* আগের ডাকের পর optind বাইরে থেকে বদলালে সীমা ঠিক করা */
        if (last_nonopt > optind) last_nonopt = optind;
        if (first_nonopt > optind) first_nonopt = optind;
        if (mode == 0) {
            if (first_nonopt != last_nonopt && last_nonopt != optind) permute(av);
            else if (last_nonopt != optind) first_nonopt = optind;
            while (optind < argc && !is_opt(argv[optind])) optind++;
            last_nonopt = optind;
        }
        if (optind < argc && !strcmp(argv[optind], "--")) {
            optind++;
            if (first_nonopt != last_nonopt && last_nonopt != optind) permute(av);
            else if (first_nonopt == last_nonopt) first_nonopt = optind;
            last_nonopt = argc;
            optind = argc;
        }
        if (optind >= argc) {
            if (first_nonopt != last_nonopt) optind = first_nonopt;
            return -1;
        }
        if (!is_opt(argv[optind])) {
            if (mode == '-') {
                optarg = argv[optind++];
                return 1;
            }
            return -1;                          /* '+' (POSIX): প্রথম অ-অপশনে থামা */
        }
        /* লম্বা অপশন */
        if (lo && argv[optind][1] == '-' && argv[optind][2]) {
            return do_long(argc, argv, opts, lo, idx, argv[optind] + 2, colon_mode);
        }
        if (lo && long_only && argv[optind][1] != '-') {
            /* -name: আগে লম্বা নামে মেলে কিনা দেখা, না মিললে ছোট অপশন হিসেবে */
            const char *nm = argv[optind] + 1;
            size_t len = strcspn(nm, "=");
            int i;
            for (i = 0; lo[i].name; i++) {
                if (!strncmp(lo[i].name, nm, len)) {
                    if (len > 1 || !strchr(opts, nm[0])) return do_long(argc, argv, opts, lo, idx, nm, colon_mode);
                }
            }
        }
        nextchar = 1;
    }

    cur = argv[optind];
    c = (unsigned char)cur[nextchar++];
    o = strchr(opts, c);
    if (!o || c == ':') {
        optopt = c;
        if (!colon_mode) msg(argv, "invalid option -- '%c'", NULL, c);
        if (!cur[nextchar]) { optind++; nextchar = 0; }
        return '?';
    }
    if (o[1] == ':') {
        if (cur[nextchar]) {
            optarg = (char *)&cur[nextchar];
            optind++;
        } else if (o[2] == ':') {
            optarg = NULL;                      /* ঐচ্ছিক মান: শুধু লেগে থাকলে */
            optind++;
        } else if (optind + 1 < argc) {
            optarg = argv[optind + 1];
            optind += 2;
        } else {
            optopt = c;
            optind++;
            nextchar = 0;
            if (colon_mode) return ':';
            msg(argv, "option requires an argument -- '%c'", NULL, c);
            return '?';
        }
        nextchar = 0;
    } else if (!cur[nextchar]) {
        optind++;
        nextchar = 0;
    }
    return c;
}

int getopt(int argc, char *const argv[], const char *opts)
{
    /* POSIX getopt: permute নয় (আগের rozlibc-র আচরণ) — '+' ধরে নেওয়া */
    if (*opts != '+' && *opts != '-') {
        char buf[256];
        size_t n = strlen(opts);
        if (n + 2 <= sizeof(buf)) {
            buf[0] = '+';
            memcpy(buf + 1, opts, n + 1);
            return getopt_core(argc, argv, buf, NULL, NULL, 0);
        }
    }
    return getopt_core(argc, argv, opts, NULL, NULL, 0);
}

int getopt_long(int argc, char *const argv[], const char *opts,
                const struct option *longopts, int *longindex)
{
    return getopt_core(argc, argv, opts, longopts, longindex, 0);
}

int getopt_long_only(int argc, char *const argv[], const char *opts,
                     const struct option *longopts, int *longindex)
{
    return getopt_core(argc, argv, opts, longopts, longindex, 1);
}
