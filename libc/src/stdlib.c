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

void __roz_tls_init(void);

void __libc_start(int argc, char **argv, char **envp)
{
    __roz_tls_init();                    /* ধাপ ১৬খ-২: মূল থ্রেডের TLS (FS base) — errno, pthread_self */
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

/* ধাপ ১৬খ-১: POSIX-এর মতো — SIGABRT খুলে পাঠানো; handler ফিরে এলে ডিফল্টে বসিয়ে আবার */
void abort(void)
{
    sigset_t s;
    struct sigaction sa;
    __stdio_flush_all();
    sigemptyset(&s);
    sigaddset(&s, SIGABRT);
    sigprocmask(SIG_UNBLOCK, &s, NULL);
    raise(SIGABRT);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sigaction(SIGABRT, &sa, NULL);
    raise(SIGABRT);
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

/* ধাপ ১৬ক: qsort = introsort (আগে shell sort ছিল — খারাপ ক্ষেত্রে O(n²)-এর কাছাকাছি):
 * median-of-3 quicksort, ছোট অংশে insertion sort, গভীরতা বেশি হলে heapsort — সবসময়
 * O(n log n)। তুলনা-ফাংশন দুই ধরনের (qsort আর qsort_r), তাই একটা ছোট মোড়ক। */
struct qcmp {
    int (*c2)(const void *, const void *);
    int (*c3)(const void *, const void *, void *);
    void *arg;
};

static int qc(const struct qcmp *q, const void *a, const void *b)
{
    return q->c2 ? q->c2(a, b) : q->c3(a, b, q->arg);
}

static void q_insertion(char *b, size_t n, size_t size, const struct qcmp *q)
{
    size_t i, j;
    for (i = 1; i < n; i++)
        for (j = i; j > 0 && qc(q, b + (j - 1) * size, b + j * size) > 0; j--)
            swap_bytes(b + (j - 1) * size, b + j * size, size);
}

static void q_sift(char *b, size_t start, size_t n, size_t size, const struct qcmp *q)
{
    size_t root = start;
    for (;;) {
        size_t child = 2 * root + 1, sw = root;
        if (child >= n) return;
        if (qc(q, b + sw * size, b + child * size) < 0) sw = child;
        if (child + 1 < n && qc(q, b + sw * size, b + (child + 1) * size) < 0) sw = child + 1;
        if (sw == root) return;
        swap_bytes(b + root * size, b + sw * size, size);
        root = sw;
    }
}

static void q_heapsort(char *b, size_t n, size_t size, const struct qcmp *q)
{
    size_t i;
    if (n < 2) return;
    for (i = n / 2; i-- > 0;) q_sift(b, i, n, size, q);
    for (i = n - 1; i > 0; i--) {
        swap_bytes(b, b + i * size, size);
        q_sift(b, 0, i, size, q);
    }
}

static void q_intro(char *b, size_t n, size_t size, const struct qcmp *q, int depth)
{
    while (n > 16) {
        size_t mid = n / 2, i, j;
        char *pv;
        if (depth-- == 0) {
            q_heapsort(b, n, size, q);
            return;
        }
        /* median-of-3 → প্রথম ঘরে pivot */
        if (qc(q, b + mid * size, b) < 0) swap_bytes(b + mid * size, b, size);
        if (qc(q, b + (n - 1) * size, b) < 0) swap_bytes(b + (n - 1) * size, b, size);
        if (qc(q, b + (n - 1) * size, b + mid * size) < 0) swap_bytes(b + (n - 1) * size, b + mid * size, size);
        swap_bytes(b, b + mid * size, size);
        pv = b;
        i = 1;
        j = n - 1;
        for (;;) {
            while (i <= j && qc(q, b + i * size, pv) < 0) i++;
            while (j >= i && qc(q, b + j * size, pv) > 0) j--;
            if (i >= j) break;
            swap_bytes(b + i * size, b + j * size, size);
            i++;
            j--;
        }
        swap_bytes(b, b + j * size, size);
        /* ছোট দিকে recursion, বড় দিকে লুপ (stack কম লাগে) */
        if (j < n - j - 1) {
            q_intro(b, j, size, q, depth);
            b += (j + 1) * size;
            n -= j + 1;
        } else {
            q_intro(b + (j + 1) * size, n - j - 1, size, q, depth);
            n = j;
        }
    }
    q_insertion(b, n, size, q);
}

static void q_sort(void *base, size_t n, size_t size, const struct qcmp *q)
{
    int depth = 0;
    size_t t = n;
    if (n < 2 || size == 0) return;
    while (t) { depth++; t >>= 1; }
    q_intro((char *)base, n, size, q, depth * 2);
}

void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    struct qcmp q;
    q.c2 = cmp;
    q.c3 = NULL;
    q.arg = NULL;
    q_sort(base, n, size, &q);
}

void qsort_r(void *base, size_t n, size_t size,
             int (*cmp)(const void *, const void *, void *), void *arg)
{
    struct qcmp q;
    q.c2 = NULL;
    q.c3 = cmp;
    q.arg = arg;
    q_sort(base, n, size, &q);
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
    return execvpe(file, argv, environ);
}

/* ধাপ ১৬ক: execvpe (GNU) — execvp-এর পুরনো ভেতরটা, নিজের envp দিয়ে */
int execvpe(const char *file, char *const argv[], char *const envp[])
{
    const char *path, *p;
    char buf[PATH_MAX];
    if (strchr(file, '/')) return execve(file, argv, envp);
    path = getenv("PATH");
    if (!path) path = "/bin:/usr/bin:/usr/local/bin";   /* ধাপ ১৭ক: /usr/bin (binutils/GCC প্যাকেজ) */
    for (p = path; ; ) {
        const char *colon = strchr(p, ':');
        size_t n = colon ? (size_t)(colon - p) : strlen(p);
        if (n + 1 + strlen(file) + 1 <= sizeof(buf)) {
            memcpy(buf, p, n);
            buf[n] = '/';
            strcpy(buf + n + 1, file);
            execve(buf, argv, envp);
            if (errno != ENOENT) return -1;
        }
        if (!colon) break;
        p = colon + 1;
    }
    errno = ENOENT;
    return -1;
}

/* ---------- getopt ধাপ ১৬ক থেকে libc/src/getopt.c-তে (getopt_long সহ) ---------- */
