/* rozlibc — শুরু/শেষ, পরিবেশ, সংখ্যা রূপান্তর, qsort ইত্যাদি */

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>

char **environ;

extern int main(int argc, char **argv, char **envp);
void __stdio_init(void);
void __stdio_flush_all(void);

/* লিংকার যে সীমা-চিহ্ন দেয় (gcc-র ld ও TCC দুটোই); না থাকলে weak = NULL */
extern void (*__preinit_array_start[])(void) __attribute__((weak));
extern void (*__preinit_array_end[])(void) __attribute__((weak));
extern void (*__init_array_start[])(void) __attribute__((weak));
extern void (*__init_array_end[])(void) __attribute__((weak));

/* glibc-র মতো constructor-রা (argc, argv, envp) পায় — না চাইলে উপেক্ষা করে */
typedef void (*init_fn)(int, char **, char **);

static void run_array(void (**a)(void), void (**e)(void), int argc, char **argv, char **envp)
{
    if (a && e) {
        for (; a < e; a++) {
            ((init_fn)*a)(argc, argv, envp);
        }
    }
}

void __libc_start(int argc, char **argv, char **envp) __attribute__((noreturn));

/* ধাপ ১৪: প্রোগ্রামের নাম (argv[0]-এর শেষ অংশ) — উইন্ডোর শিরোনাম, বার্তার জন্য */
char *__progname = (char *)"";
char *program_invocation_name = (char *)"";
char *program_invocation_short_name = (char *)"";

void __libc_start(int argc, char **argv, char **envp)
{
    environ = envp;
    if (argc > 0 && argv[0]) {
        char *p = argv[0], *q;
        for (q = p; *q; q++) if (*q == '/') p = q + 1;
        __progname = program_invocation_short_name = p;
        program_invocation_name = argv[0];
    }
    __stdio_init();
    run_array(__preinit_array_start, __preinit_array_end, argc, argv, envp);
    run_array(__init_array_start, __init_array_end, argc, argv, envp);   /* C/C++ constructor */
    exit(main(argc, argv, envp));
}

void _Exit(int code)
{
    _exit(code);
}

void abort(void)
{
    __stdio_flush_all();
    kill(getpid(), SIGABRT);
    _exit(127);
}

void __assert_fail(const char *expr, const char *file, int line, const char *func)
{
    fprintf(stderr, "%s:%d: %s: assertion '%s' failed\n", file, line, func, expr);
    abort();
}

/* ---------- পরিবেশ ---------- */

static int env_owned;            /* environ আমাদের malloc-করা কিনা */

char *getenv(const char *name)
{
    size_t n = strlen(name);
    char **e;
    if (!environ) return NULL;
    for (e = environ; *e; e++) {
        if (!strncmp(*e, name, n) && (*e)[n] == '=') return *e + n + 1;
    }
    return NULL;
}

static int env_count(void)
{
    int n = 0;
    if (environ) while (environ[n]) n++;
    return n;
}

static int env_put(char *entry, size_t name_len)
{
    int i, n = env_count();
    char **ne;
    for (i = 0; i < n; i++) {
        if (!strncmp(environ[i], entry, name_len) && environ[i][name_len] == '=') {
            environ[i] = entry;
            return 0;
        }
    }
    ne = (char **)malloc(sizeof(char *) * (size_t)(n + 2));
    if (!ne) return -1;
    if (n) memcpy(ne, environ, sizeof(char *) * (size_t)n);
    ne[n] = entry;
    ne[n + 1] = NULL;
    if (env_owned) free(environ);
    environ = ne;
    env_owned = 1;
    return 0;
}

int setenv(const char *name, const char *value, int overwrite)
{
    size_t a, b;
    char *e;
    if (!name || !*name || strchr(name, '=')) { errno = EINVAL; return -1; }
    if (!overwrite && getenv(name)) return 0;
    a = strlen(name);
    b = strlen(value);
    e = (char *)malloc(a + b + 2);
    if (!e) return -1;
    memcpy(e, name, a);
    e[a] = '=';
    memcpy(e + a + 1, value, b + 1);
    return env_put(e, a);
}

int putenv(char *s)
{
    char *eq = strchr(s, '=');
    if (!eq) return unsetenv(s);
    return env_put(s, (size_t)(eq - s));
}

int unsetenv(const char *name)
{
    size_t n = strlen(name);
    int i, j;
    if (!environ) return 0;
    for (i = j = 0; environ[i]; i++) {
        if (!(strncmp(environ[i], name, n) == 0 && environ[i][n] == '=')) {
            environ[j++] = environ[i];
        }
    }
    environ[j] = NULL;
    return 0;
}

/* ---------- সংখ্যা ---------- */

unsigned long long strtoull(const char *s, char **end, int base)
{
    const char *p = s;
    unsigned long long v = 0;
    int neg = 0, any = 0, over = 0;

    while (isspace((unsigned char)*p)) p++;
    if (*p == '+' || *p == '-') neg = (*p++ == '-');
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
        isxdigit((unsigned char)p[2])) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = (*p == '0') ? 8 : 10;
    }
    for (;; p++) {
        int d;
        if (isdigit((unsigned char)*p)) d = *p - '0';
        else if (isalpha((unsigned char)*p)) d = tolower((unsigned char)*p) - 'a' + 10;
        else break;
        if (d >= base) break;
        if (v > (ULLONG_MAX - (unsigned)d) / (unsigned)base) over = 1;
        v = v * (unsigned)base + (unsigned)d;
        any = 1;
    }
    if (end) *end = (char *)(any ? p : s);
    if (over) { errno = ERANGE; return ULLONG_MAX; }
    return neg ? -v : v;
}

long long strtoll(const char *s, char **end, int base)
{
    const char *p = s;
    unsigned long long v;
    int neg;
    while (isspace((unsigned char)*p)) p++;
    neg = (*p == '-');
    v = strtoull(s, end, base);
    if (neg) {
        v = -v;                                   /* মান ফেরত পজিটিভ করা */
        if (v > (unsigned long long)LLONG_MAX + 1) { errno = ERANGE; return LLONG_MIN; }
        return -(long long)v;
    }
    if (v > (unsigned long long)LLONG_MAX) { errno = ERANGE; return LLONG_MAX; }
    return (long long)v;
}

long strtol(const char *s, char **end, int base) { return (long)strtoll(s, end, base); }
unsigned long strtoul(const char *s, char **end, int base) { return (unsigned long)strtoull(s, end, base); }
int atoi(const char *s) { return (int)strtol(s, NULL, 10); }
long atol(const char *s) { return strtol(s, NULL, 10); }
long long atoll(const char *s) { return strtoll(s, NULL, 10); }

int abs(int v) { return v < 0 ? -v : v; }
long labs(long v) { return v < 0 ? -v : v; }
long long llabs(long long v) { return v < 0 ? -v : v; }

static unsigned long rand_state = 1;
int rand(void)
{
    rand_state = rand_state * 6364136223846793005UL + 1442695040888963407UL;
    return (int)((rand_state >> 33) & RAND_MAX);
}
void srand(unsigned seed) { rand_state = seed; }

/* ---------- qsort (insertion + shell sort — ছোট ও স্থিতিশীল কোড) ---------- */

static void swap_bytes(char *a, char *b, size_t n)
{
    while (n--) { char t = *a; *a++ = *b; *b++ = t; }
}

void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    char *b = (char *)base;
    size_t gap, i, j;
    for (gap = n / 2; gap > 0; gap /= 2) {
        for (i = gap; i < n; i++) {
            for (j = i; j >= gap && cmp(b + (j - gap) * size, b + j * size) > 0; j -= gap) {
                swap_bytes(b + (j - gap) * size, b + j * size, size);
            }
        }
    }
}

void *bsearch(const void *key, const void *base, size_t n, size_t size,
              int (*cmp)(const void *, const void *))
{
    const char *b = (const char *)base;
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int r = cmp(key, b + mid * size);
        if (r == 0) return (void *)(b + mid * size);
        if (r < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}

/* ---------- execvp ---------- */

int execv(const char *path, char *const argv[])
{
    return execve(path, argv, environ);
}

int execvp(const char *file, char *const argv[])
{
    const char *path, *p;
    char buf[PATH_MAX];
    if (strchr(file, '/')) return execve(file, argv, environ);
    path = getenv("PATH");
    if (!path) path = "/bin";
    for (p = path; ; ) {
        const char *colon = strchr(p, ':');
        size_t n = colon ? (size_t)(colon - p) : strlen(p);
        if (n + 1 + strlen(file) + 1 <= sizeof(buf)) {
            memcpy(buf, p, n);
            buf[n] = '/';
            strcpy(buf + n + 1, file);
            execve(buf, argv, environ);
            if (errno != ENOENT) return -1;
        }
        if (!colon) break;
        p = colon + 1;
    }
    errno = ENOENT;
    return -1;
}

/* ---------- getopt ---------- */

char *optarg;
int optind = 1, opterr = 1, optopt;

int getopt(int argc, char *const argv[], const char *opts)
{
    static int pos = 1;
    const char *o;
    char c;
    if (optind >= argc || argv[optind][0] != '-' || argv[optind][1] == '\0') return -1;
    if (!strcmp(argv[optind], "--")) { optind++; return -1; }
    c = argv[optind][pos];
    o = strchr(opts, c);
    if (!o || c == ':') {
        optopt = c;
        if (opterr) fprintf(stderr, "%s: unknown option -%c\n", argv[0], c);
        if (!argv[optind][++pos]) { optind++; pos = 1; }
        return '?';
    }
    if (o[1] == ':') {
        if (argv[optind][pos + 1]) {
            optarg = &argv[optind][pos + 1];
        } else if (optind + 1 < argc) {
            optarg = argv[++optind];
        } else {
            optopt = c;
            if (opterr) fprintf(stderr, "%s: option -%c needs a value\n", argv[0], c);
            optind++; pos = 1;
            return '?';
        }
        optind++; pos = 1;
    } else if (!argv[optind][++pos]) {
        optind++; pos = 1;
    }
    return c;
}
