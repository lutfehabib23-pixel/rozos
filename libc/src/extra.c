/* rozlibc — ধাপ ১৬ক-এর ছোট ছোট সংযোজন
 *
 * string.h/strings.h-এর বাকি ফাংশন, stdlib-এর div/random/drand48/mkstemp/…,
 * unistd-এর pread/pwrite/fsync/sysconf/…, fcntl, select/readv/writev (কার্নেলে নতুন
 * syscall), syslog, err.h, dlfcn (stub), sys/resource, times, flock, utime, search.h।
 * যেখানে rozOS-এ জিনিসটাই নেই (symlink, chown, একাধিক ব্যবহারকারী) সেখানে POSIX-সম্মত
 * ব্যর্থতা (ENOSYS/EPERM) বা "কিছু করার নেই" সাফল্য — যেটা প্রোগ্রামের জন্য নিরাপদ। */

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <termios.h>
#include <signal.h>
#include <inttypes.h>
#include <syslog.h>
#include <err.h>
#include <dlfcn.h>
#include <search.h>
#include <utime.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/times.h>
#include <sys/file.h>
#include <sys/utsname.h>
#include <rozos.h>

#define SYS_pread64   17
#define SYS_pwrite64  18
#define SYS_readv     19
#define SYS_writev    20
#define SYS_select    23
#define SYS_fcntl     72
#define SYS_fsync     74
#define SYS_fdatasync 75

#define SC(n, a, b, c, d, e) __syscall_ret(__syscall6((n), (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), 0))

extern char *__progname;

/* ================= string.h ================= */

void *memrchr(const void *s, int c, size_t n)
{
    const unsigned char *p = (const unsigned char *)s + n;
    while (n--) if (*--p == (unsigned char)c) return (void *)p;
    return NULL;
}

void *memmem(const void *hay, size_t hn, const void *needle, size_t nn)
{
    const char *h = (const char *)hay;
    size_t i;
    if (!nn) return (void *)hay;
    if (hn < nn) return NULL;
    for (i = 0; i + nn <= hn; i++) {
        if (h[i] == *(const char *)needle && !memcmp(h + i, needle, nn)) return (void *)(h + i);
    }
    return NULL;
}

void *mempcpy(void *dst, const void *src, size_t n) { return (char *)memcpy(dst, src, n) + n; }

void *memccpy(void *dst, const void *src, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) {
        if ((*d++ = *s++) == (unsigned char)c) return d;
    }
    return NULL;
}

char *strchrnul(const char *s, int c)
{
    while (*s && *s != (char)c) s++;
    return (char *)s;
}

char *strcasestr(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (!n) return (char *)hay;
    for (; *hay; hay++) if (!strncasecmp(hay, needle, n)) return (char *)hay;
    return NULL;
}

char *stpncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    {
        char *end = dst + i;
        for (; i < n; i++) dst[i] = '\0';
        return end;
    }
}

char *strsep(char **sp, const char *delim)
{
    char *s = *sp, *e;
    if (!s) return NULL;
    e = s + strcspn(s, delim);
    if (*e) { *e = '\0'; *sp = e + 1; }
    else *sp = NULL;
    return s;
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t n = strlen(src);
    if (size) {
        size_t k = n < size - 1 ? n : size - 1;
        memcpy(dst, src, k);
        dst[k] = '\0';
    }
    return n;
}

size_t strlcat(char *dst, const char *src, size_t size)
{
    size_t d = strnlen(dst, size);
    if (d == size) return size + strlen(src);
    return d + strlcpy(dst + d, src, size - d);
}

int strerror_r(int err, char *buf, size_t len)
{
    const char *m = strerror(err);
    if (strlen(m) + 1 > len) {
        if (len) { memcpy(buf, m, len - 1); buf[len - 1] = '\0'; }
        return ERANGE;
    }
    strcpy(buf, m);
    return 0;
}

char *strsignal(int sig)
{
    static char buf[32];
    static const char *const names[] = {
        "Unknown signal 0", "Hangup", "Interrupt", "Quit", "Illegal instruction", "Trace/breakpoint trap",
        "Aborted", "Bus error", "Floating point exception", "Killed", "User defined signal 1",
        "Segmentation fault", "User defined signal 2", "Broken pipe", "Alarm clock", "Terminated",
        "Stack fault", "Child exited", "Continued", "Stopped (signal)", "Stopped",
        "Stopped (tty input)", "Stopped (tty output)", "Urgent I/O condition", "CPU time limit exceeded",
        "File size limit exceeded", "Virtual timer expired", "Profiling timer expired",
        "Window changed", "I/O possible", "Power failure", "Bad system call",
    };
    if (sig >= 0 && sig < (int)(sizeof(names) / sizeof(names[0]))) return (char *)names[sig];
    snprintf(buf, sizeof(buf), "Unknown signal %d", sig);
    return buf;
}

size_t strxfrm(char *dst, const char *src, size_t n)
{
    size_t l = strlen(src);
    if (l < n) strcpy(dst, src);
    return l;
}

static int isdigit_ascii(int c) { return c >= '0' && c <= '9'; }

/* "file9" < "file10": সংখ্যার অংশ সংখ্যা হিসেবে তুলনা (GNU) */
int strverscmp(const char *a, const char *b)
{
    while (*a && *a == *b && !isdigit_ascii(*a)) { a++; b++; }
    for (;;) {
        if (isdigit_ascii(*a) && isdigit_ascii(*b)) {
            const char *pa = a, *pb = b;
            size_t la, lb;
            int lz = (*a == '0' || *b == '0');
            while (isdigit_ascii(*pa)) pa++;
            while (isdigit_ascii(*pb)) pb++;
            la = (size_t)(pa - a);
            lb = (size_t)(pb - b);
            if (!lz && la != lb) return la < lb ? -1 : 1;
            {
                int c = strncmp(a, b, la < lb ? la : lb);
                if (c) return c;
                if (la != lb) return la < lb ? (lz ? 1 : -1) : (lz ? -1 : 1);
            }
            a = pa;
            b = pb;
            continue;
        }
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        if (!*a) return 0;
        a++;
        b++;
    }
}

void explicit_bzero(void *s, size_t n)
{
    volatile unsigned char *p = (volatile unsigned char *)s;
    while (n--) *p++ = 0;
}

/* ================= strings.h ================= */

void bzero(void *s, size_t n) { memset(s, 0, n); }
void bcopy(const void *src, void *dst, size_t n) { memmove(dst, src, n); }
int bcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n) != 0; }
char *index(const char *s, int c) { return strchr(s, c); }
char *rindex(const char *s, int c) { return strrchr(s, c); }
int ffs(int v) { int i; if (!v) return 0; for (i = 1; !(v & 1); i++) v = (int)((unsigned)v >> 1); return i; }
int ffsl(long v) { int i; if (!v) return 0; for (i = 1; !(v & 1); i++) v = (long)((unsigned long)v >> 1); return i; }
int ffsll(long long v) { int i; if (!v) return 0; for (i = 1; !(v & 1); i++) v = (long long)((unsigned long long)v >> 1); return i; }

/* ================= stdlib.h ================= */

div_t div(int a, int b) { div_t r; r.quot = a / b; r.rem = a % b; return r; }
ldiv_t ldiv(long a, long b) { ldiv_t r; r.quot = a / b; r.rem = a % b; return r; }
lldiv_t lldiv(long long a, long long b) { lldiv_t r; r.quot = a / b; r.rem = a % b; return r; }
intmax_t imaxabs(intmax_t v) { return v < 0 ? -v : v; }
imaxdiv_t imaxdiv(intmax_t a, intmax_t b) { imaxdiv_t r; r.quot = a / b; r.rem = a % b; return r; }

/* random(): xorshift64* (rand()-এর থেকে আলাদা অবস্থা) */
static uint64_t rnd_state = 0x853c49e6748fea9bULL;

long random(void)
{
    rnd_state ^= rnd_state >> 12;
    rnd_state ^= rnd_state << 25;
    rnd_state ^= rnd_state >> 27;
    return (long)((rnd_state * 0x2545F4914F6CDD1DULL) >> 33);
}

void srandom(unsigned seed)
{
    rnd_state = (uint64_t)seed * 0x9E3779B97F4A7C15ULL + 1;
    if (!rnd_state) rnd_state = 1;
}

char *initstate(unsigned seed, char *state, size_t n)
{
    static char dummy[8];
    (void)state;
    (void)n;
    srandom(seed);
    return dummy;
}

char *setstate(char *state)
{
    static char dummy[8];
    (void)state;
    return dummy;
}

int rand_r(unsigned *seed)
{
    unsigned x = *seed;
    x = x * 1103515245u + 12345u;
    *seed = x;
    return (int)((x >> 1) & 0x7fffffff);
}

/* drand48 পরিবার: POSIX-এর ৪৮-বিট LCG (a = 0x5DEECE66D, c = 0xB) */
static unsigned short d48_x[3] = { 0x330e, 0xabcd, 0x1234 };
static unsigned short d48_a[3] = { 0xe66d, 0xdeec, 0x0005 };
static unsigned short d48_c = 0xb;

static uint64_t d48_step(unsigned short x[3])
{
    uint64_t v = (uint64_t)x[0] | (uint64_t)x[1] << 16 | (uint64_t)x[2] << 32;
    uint64_t a = (uint64_t)d48_a[0] | (uint64_t)d48_a[1] << 16 | (uint64_t)d48_a[2] << 32;
    v = (v * a + d48_c) & 0xffffffffffffULL;
    x[0] = (unsigned short)v;
    x[1] = (unsigned short)(v >> 16);
    x[2] = (unsigned short)(v >> 32);
    return v;
}

double erand48(unsigned short x[3]) { return (double)d48_step(x) / 281474976710656.0; }
double drand48(void) { return erand48(d48_x); }
long nrand48(unsigned short x[3]) { return (long)(d48_step(x) >> 17); }
long lrand48(void) { return nrand48(d48_x); }
long jrand48(unsigned short x[3]) { return (long)(int32_t)(uint32_t)(d48_step(x) >> 16); }
long mrand48(void) { return jrand48(d48_x); }

void srand48(long seed)
{
    d48_x[0] = 0x330e;
    d48_x[1] = (unsigned short)seed;
    d48_x[2] = (unsigned short)((unsigned long)seed >> 16);
}

unsigned short *seed48(unsigned short x[3])
{
    static unsigned short old[3];
    memcpy(old, d48_x, sizeof(old));
    memcpy(d48_x, x, sizeof(d48_x));
    return old;
}

/* mkstemp ইত্যাদি: শেষের ৬টা X বদলে নতুন নাম, O_EXCL দিয়ে তৈরি */
static int fill_template(char *tmpl, int suffixlen)
{
    static const char cs[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    size_t l = strlen(tmpl);
    char *x;
    int i;
    if (suffixlen < 0 || l < (size_t)suffixlen + 6) { errno = EINVAL; return -1; }
    x = tmpl + l - suffixlen - 6;
    if (memcmp(x, "XXXXXX", 6)) { errno = EINVAL; return -1; }
    {
        uint64_t r = (uint64_t)random() ^ ((uint64_t)rozos_now_ms() << 20) ^ (uint64_t)getpid();
        for (i = 0; i < 6; i++) {
            x[i] = cs[r % (sizeof(cs) - 1)];
            r /= (sizeof(cs) - 1);
            if (!r) r = (uint64_t)random();
        }
    }
    return 0;
}

int mkostemps(char *tmpl, int suffixlen, int flags)
{
    int tries;
    size_t l = strlen(tmpl);
    char save[6];
    if (l < (size_t)suffixlen + 6) { errno = EINVAL; return -1; }
    memcpy(save, tmpl + l - suffixlen - 6, 6);
    for (tries = 0; tries < 100; tries++) {
        int fd;
        memcpy(tmpl + l - suffixlen - 6, save, 6);
        if (fill_template(tmpl, suffixlen) < 0) return -1;
        fd = open(tmpl, O_RDWR | O_CREAT | O_EXCL | (flags & ~O_ACCMODE), 0600);
        if (fd >= 0) return fd;
        if (errno != EEXIST) return -1;
    }
    errno = EEXIST;
    return -1;
}

int mkstemp(char *tmpl) { return mkostemps(tmpl, 0, 0); }
int mkostemp(char *tmpl, int flags) { return mkostemps(tmpl, 0, flags); }
int mkstemps(char *tmpl, int suffixlen) { return mkostemps(tmpl, suffixlen, 0); }

char *mkdtemp(char *tmpl)
{
    int tries;
    size_t l = strlen(tmpl);
    if (l < 6) { errno = EINVAL; return NULL; }
    for (tries = 0; tries < 100; tries++) {
        memcpy(tmpl + l - 6, "XXXXXX", 6);
        if (fill_template(tmpl, 0) < 0) return NULL;
        if (mkdir(tmpl, 0700) == 0) return tmpl;
        if (errno != EEXIST) return NULL;
    }
    return NULL;
}

char *mktemp(char *tmpl)
{
    int tries;
    size_t l = strlen(tmpl);
    if (l < 6) { errno = EINVAL; tmpl[0] = '\0'; return tmpl; }
    for (tries = 0; tries < 100; tries++) {
        memcpy(tmpl + l - 6, "XXXXXX", 6);
        if (fill_template(tmpl, 0) < 0) { tmpl[0] = '\0'; return tmpl; }
        if (access(tmpl, F_OK) < 0) return tmpl;
    }
    tmpl[0] = '\0';
    return tmpl;
}

int clearenv(void)
{
    static char *empty[1];
    environ = empty;
    return 0;
}

char *secure_getenv(const char *name) { return getenv(name); }

static void (*qexit_fns[32])(void);
static int qexit_n;

int at_quick_exit(void (*fn)(void))
{
    if (qexit_n >= 32) return -1;
    qexit_fns[qexit_n++] = fn;
    return 0;
}

void quick_exit(int code)
{
    while (qexit_n > 0) qexit_fns[--qexit_n]();
    _Exit(code);
}

int getsubopt(char **opt, char *const *tokens, char **value)
{
    char *s = *opt, *end, *eq;
    int i;
    *value = NULL;
    if (!*s) return -1;
    end = strchr(s, ',');
    if (end) { *end = '\0'; *opt = end + 1; }
    else *opt = s + strlen(s);
    eq = strchr(s, '=');
    if (eq) { *eq = '\0'; *value = eq + 1; }
    for (i = 0; tokens[i]; i++) if (!strcmp(s, tokens[i])) return i;
    *value = s;
    if (eq) *eq = '=';
    return -1;
}

static const char b64c[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

char *l64a(long v)
{
    static char buf[8];
    unsigned long u = (unsigned long)v & 0xffffffffUL;
    int i = 0;
    while (u && i < 6) { buf[i++] = b64c[u & 63]; u >>= 6; }
    buf[i] = '\0';
    return buf;
}

long a64l(const char *s)
{
    long v = 0;
    int i;
    for (i = 0; i < 6 && s[i]; i++) {
        const char *p = strchr(b64c, s[i]);
        if (!p) break;
        v |= (long)(p - b64c) << (6 * i);
    }
    return (long)(int32_t)v;
}

/* ecvt/fcvt/gcvt (পুরনো) — নিখুঁত printf দিয়ে */
static char *cvt(double v, int ndig, int *dec, int *sign, int fixed)
{
    static char out[400];
    char buf[420], *p, *e;
    int ex;
    *sign = v < 0;
    if (v < 0) v = -v;
    if (ndig < 0) ndig = 0;
    if (ndig > 340) ndig = 340;
    if (fixed) snprintf(buf, sizeof(buf), "%.*f", ndig, v);
    else snprintf(buf, sizeof(buf), "%.*e", ndig > 0 ? ndig - 1 : 0, v);
    if (fixed) {
        p = strchr(buf, '.');
        *dec = p ? (int)(p - buf) : (int)strlen(buf);
        if (p) memmove(p, p + 1, strlen(p));
        p = buf;
        while (*p == '0' && p[1]) { p++; (*dec)--; }
        strcpy(out, p);
        return out;
    }
    e = strchr(buf, 'e');
    ex = e ? atoi(e + 1) : 0;
    if (e) *e = '\0';
    p = strchr(buf, '.');
    if (p) memmove(p, p + 1, strlen(p));
    *dec = v == 0 ? 0 : ex + 1;
    strcpy(out, buf);
    return out;
}

char *ecvt(double v, int ndig, int *dec, int *sign) { return cvt(v, ndig, dec, sign, 0); }
char *fcvt(double v, int ndig, int *dec, int *sign) { return cvt(v, ndig, dec, sign, 1); }

char *gcvt(double v, int ndig, char *buf)
{
    sprintf(buf, "%.*g", ndig, v);
    return buf;
}

int getloadavg(double loads[], int n)
{
    int i;
    for (i = 0; i < n && i < 3; i++) loads[i] = 0.0;
    return i;
}

/* ================= unistd.h ================= */

ssize_t pread(int fd, void *buf, size_t n, off_t off) { return SC(SYS_pread64, fd, buf, n, off, 0); }
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off) { return SC(SYS_pwrite64, fd, buf, n, off, 0); }
int fsync(int fd) { return (int)SC(SYS_fsync, fd, 0, 0, 0, 0); }
int fdatasync(int fd) { return (int)SC(SYS_fdatasync, fd, 0, 0, 0, 0); }
void sync(void) { }
int getpagesize(void) { return 4096; }
int getdtablesize(void) { return 256; }
long gethostid(void) { return 0x7f0101; }

long sysconf(int name)
{
    switch (name) {
    case _SC_ARG_MAX: return ARG_MAX;
    case _SC_CHILD_MAX: return CHILD_MAX;
    case _SC_CLK_TCK: return 100;
    case _SC_NGROUPS_MAX: return NGROUPS_MAX;
    case _SC_OPEN_MAX: return OPEN_MAX;
    case _SC_PAGESIZE: return 4096;
    case _SC_NPROCESSORS_CONF:
    case _SC_NPROCESSORS_ONLN: return 1;
    case _SC_PHYS_PAGES: return 131072;
    case _SC_LINE_MAX: return LINE_MAX;
    case _SC_HOST_NAME_MAX: return HOST_NAME_MAX;
    case _SC_LOGIN_NAME_MAX: return LOGIN_NAME_MAX;
    case _SC_GETPW_R_SIZE_MAX:
    case _SC_GETGR_R_SIZE_MAX: return 1024;
    case _SC_TTY_NAME_MAX: return TTY_NAME_MAX;
    }
    errno = EINVAL;
    return -1;
}

static long pconf(int name)
{
    switch (name) {
    case _PC_NAME_MAX: return NAME_MAX;
    case _PC_PATH_MAX: return PATH_MAX;
    case _PC_PIPE_BUF: return PIPE_BUF;
    case _PC_LINK_MAX: return 1;
    }
    errno = EINVAL;
    return -1;
}

long pathconf(const char *path, int name) { (void)path; return pconf(name); }
long fpathconf(int fd, int name) { (void)fd; return pconf(name); }

/* rozOS-এ symlink/hard link নেই */
int link(const char *o, const char *n) { (void)o; (void)n; errno = EPERM; return -1; }
int symlink(const char *t, const char *l) { (void)t; (void)l; errno = EPERM; return -1; }
ssize_t readlink(const char *path, char *buf, size_t size)
{
    struct stat st;
    (void)buf; (void)size;
    if (stat(path, &st) < 0) return -1;
    errno = EINVAL;                            /* আছে, কিন্তু symlink নয় */
    return -1;
}

/* একজনই ব্যবহারকারী (root): মালিক বদলের কিছু নেই */
int chown(const char *p, uid_t u, gid_t g) { (void)u; (void)g; return access(p, F_OK); }
int fchown(int fd, uid_t u, gid_t g) { (void)u; (void)g; return fcntl(fd, F_GETFD) < 0 ? -1 : 0; }
int lchown(const char *p, uid_t u, gid_t g) { return chown(p, u, g); }
int setuid(uid_t u) { if (u) { errno = EPERM; return -1; } return 0; }
int setgid(gid_t g) { if (g) { errno = EPERM; return -1; } return 0; }
int seteuid(uid_t u) { return setuid(u); }
int setegid(gid_t g) { return setgid(g); }

int getgroups(int n, gid_t list[])
{
    if (n == 0) return 1;
    if (n < 1) { errno = EINVAL; return -1; }
    list[0] = 0;
    return 1;
}

int fchdir(int fd) { (void)fd; errno = ENOSYS; return -1; }

char *ttyname(int fd)
{
    static char name[TTY_NAME_MAX];
    if (ttyname_r(fd, name, sizeof(name))) return NULL;
    return name;
}

int ttyname_r(int fd, char *buf, size_t n)
{
    if (!isatty(fd)) return errno ? errno : ENOTTY;
    if (n < 9) return ERANGE;
    strcpy(buf, "/dev/tty");
    return 0;
}

int nice(int inc) { (void)inc; return 0; }
/* alarm ও pause ধাপ ১৬খ-১ থেকে signal.c-তে (আসল SIGALRM) */

char *getpass(const char *prompt)
{
    static char buf[128];
    struct termios t, old;
    int have = tcgetattr(0, &t) == 0, n = 0;
    char c;
    fputs(prompt, stderr);
    fflush(stderr);
    if (have) {
        old = t;
        t.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(0, TCSAFLUSH, &t);
    }
    while (read(0, &c, 1) == 1 && c != '\n' && c != '\r') {
        if (n + 1 < (int)sizeof(buf)) buf[n++] = c;
    }
    buf[n] = '\0';
    if (have) tcsetattr(0, TCSAFLUSH, &old);
    fputc('\n', stderr);
    return buf;
}

int sethostname(const char *name, size_t len) { (void)name; (void)len; errno = EPERM; return -1; }
int lockf(int fd, int cmd, off_t len) { (void)cmd; (void)len; return fcntl(fd, F_GETFD) < 0 ? -1 : 0; }

/* ================= fcntl.h ================= */

int fcntl(int fd, int cmd, ...)
{
    va_list ap;
    long arg;
    va_start(ap, cmd);
    arg = va_arg(ap, long);
    va_end(ap);
    return (int)SC(SYS_fcntl, fd, cmd, arg, 0, 0);
}

int openat(int dirfd, const char *path, int flags, ...)
{
    va_list ap;
    int mode;
    va_start(ap, flags);
    mode = va_arg(ap, int);
    va_end(ap);
    if (dirfd != AT_FDCWD && path[0] != '/') { errno = ENOSYS; return -1; }
    return open(path, flags, mode);
}

int posix_fadvise(int fd, off_t off, off_t len, int advice)
{
    (void)fd; (void)off; (void)len; (void)advice;
    return 0;
}

int fstatat(int dirfd, const char *path, struct stat *st, int flags)
{
    if (dirfd != AT_FDCWD && path[0] != '/') { errno = ENOSYS; return -1; }
    return (flags & 0x100) ? lstat(path, st) : stat(path, st);
}

int mkdirat(int dirfd, const char *path, mode_t mode)
{
    if (dirfd != AT_FDCWD && path[0] != '/') { errno = ENOSYS; return -1; }
    return mkdir(path, mode);
}

int fchmod(int fd, mode_t mode) { (void)mode; return fcntl(fd, F_GETFD) < 0 ? -1 : 0; }
int mkfifo(const char *path, mode_t mode) { (void)path; (void)mode; errno = ENOSYS; return -1; }
int mknod(const char *path, mode_t mode, dev_t dev) { (void)path; (void)mode; (void)dev; errno = EPERM; return -1; }
int futimens(int fd, const struct timespec t[2]) { (void)fd; (void)t; errno = ENOSYS; return -1; }

/* ================= sys/uio.h, sys/select.h ================= */

ssize_t readv(int fd, const struct iovec *iov, int cnt) { return SC(SYS_readv, fd, iov, cnt, 0, 0); }
ssize_t writev(int fd, const struct iovec *iov, int cnt) { return SC(SYS_writev, fd, iov, cnt, 0, 0); }

ssize_t preadv(int fd, const struct iovec *iov, int cnt, off_t off)
{
    ssize_t total = 0;
    int i;
    for (i = 0; i < cnt; i++) {
        ssize_t r = pread(fd, iov[i].iov_base, iov[i].iov_len, off + total);
        if (r < 0) return total ? total : -1;
        total += r;
        if ((size_t)r < iov[i].iov_len) break;
    }
    return total;
}

ssize_t pwritev(int fd, const struct iovec *iov, int cnt, off_t off)
{
    ssize_t total = 0;
    int i;
    for (i = 0; i < cnt; i++) {
        ssize_t r = pwrite(fd, iov[i].iov_base, iov[i].iov_len, off + total);
        if (r < 0) return total ? total : -1;
        total += r;
        if ((size_t)r < iov[i].iov_len) break;
    }
    return total;
}

int select(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *tv)
{
    return (int)SC(SYS_select, nfds, rd, wr, ex, tv);
}

int pselect(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, const struct timespec *ts,
            const sigset_t *mask)
{
    struct timeval tv, *tp = NULL;
    (void)mask;                                /* signal mask ধাপ ১৬খ-তে */
    if (ts) {
        tv.tv_sec = ts->tv_sec;
        tv.tv_usec = (ts->tv_nsec + 999) / 1000;
        tp = &tv;
    }
    return select(nfds, rd, wr, ex, tp);
}

/* ================= sys/time.h, utime.h ================= */

int settimeofday(const struct timeval *tv, const void *tz) { (void)tv; (void)tz; errno = EPERM; return -1; }

int utimes(const char *path, const struct timeval tv[2])
{
    struct timespec ts[2];
    if (!tv) return utimensat(AT_FDCWD, path, NULL, 0);
    ts[0].tv_sec = tv[0].tv_sec; ts[0].tv_nsec = tv[0].tv_usec * 1000;
    ts[1].tv_sec = tv[1].tv_sec; ts[1].tv_nsec = tv[1].tv_usec * 1000;
    return utimensat(AT_FDCWD, path, ts, 0);
}

int utime(const char *path, const struct utimbuf *t)
{
    struct timespec ts[2];
    if (!t) return utimensat(AT_FDCWD, path, NULL, 0);
    ts[0].tv_sec = t->actime; ts[0].tv_nsec = 0;
    ts[1].tv_sec = t->modtime; ts[1].tv_nsec = 0;
    return utimensat(AT_FDCWD, path, ts, 0);
}

/* ================= sys/resource.h, sys/times.h, sys/file.h ================= */

int getrlimit(int res, struct rlimit *r)
{
    if (res < 0 || res >= RLIM_NLIMITS) { errno = EINVAL; return -1; }
    r->rlim_cur = r->rlim_max = res == RLIMIT_NOFILE ? 256 : res == RLIMIT_STACK ? 8UL << 20 : RLIM_INFINITY;
    return 0;
}

int setrlimit(int res, const struct rlimit *r)
{
    (void)r;
    if (res < 0 || res >= RLIM_NLIMITS) { errno = EINVAL; return -1; }
    return 0;
}

int getrusage(int who, struct rusage *u)
{
    clock_t c = clock();
    (void)who;
    memset(u, 0, sizeof(*u));
    u->ru_utime.tv_sec = c / CLOCKS_PER_SEC;
    u->ru_utime.tv_usec = c % CLOCKS_PER_SEC;
    return 0;
}

int getpriority(int which, int who) { (void)which; (void)who; errno = 0; return 0; }
int setpriority(int which, int who, int prio) { (void)which; (void)who; (void)prio; return 0; }

clock_t times(struct tms *t)
{
    if (t) {
        t->tms_utime = clock() / (CLOCKS_PER_SEC / 100);
        t->tms_stime = t->tms_cutime = t->tms_cstime = 0;
    }
    return (clock_t)(rozos_now_ms() / 10);
}

int flock(int fd, int op) { (void)op; return fcntl(fd, F_GETFD) < 0 ? -1 : 0; }

/* ================= syslog ================= */

static const char *log_ident;
static int log_opt, log_fac = LOG_USER, log_mask = 0xff;

void openlog(const char *ident, int option, int facility)
{
    log_ident = ident;
    log_opt = option;
    if (facility) log_fac = facility;
}

void closelog(void) { log_ident = NULL; }

int setlogmask(int mask)
{
    int old = log_mask;
    if (mask) log_mask = mask;
    return old;
}

void vsyslog(int pri, const char *fmt, va_list ap)
{
    char msg[1024], line[1200], ts[32], host[64];
    time_t now = time(NULL);
    struct tm tm;
    int n, fd;
    if (!(log_mask & LOG_MASK(LOG_PRI(pri)))) return;
    vsnprintf(msg, sizeof(msg), fmt, ap);
    localtime_r(&now, &tm);
    strftime(ts, sizeof(ts), "%b %e %H:%M:%S", &tm);
    if (gethostname(host, sizeof(host)) < 0) strcpy(host, "rozos");
    if (log_opt & LOG_PID)
        n = snprintf(line, sizeof(line), "%s %s %s[%d]: %s\n", ts, host,
                     log_ident ? log_ident : __progname, (int)getpid(), msg);
    else
        n = snprintf(line, sizeof(line), "%s %s %s: %s\n", ts, host, log_ident ? log_ident : __progname, msg);
    if (n < 0) return;
    if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;
    fd = open("/var/log/messages", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) fd = open("/tmp/messages", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0 && (log_opt & LOG_CONS)) fd = open("/dev/console", O_WRONLY, 0);
    if (fd >= 0) {
        write(fd, line, (size_t)n);
        close(fd);
    }
    if (log_opt & LOG_PERROR) {
        char *colon = strchr(line + strlen(ts) + 1, ' ');
        const char *p = colon ? colon + 1 : line;
        write(2, p, strlen(p));
    }
}

void syslog(int pri, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsyslog(pri, fmt, ap);
    va_end(ap);
}

/* ================= err.h ================= */

void vwarn(const char *fmt, va_list ap)
{
    int e = errno;
    fprintf(stderr, "%s: ", __progname);
    if (fmt) { vfprintf(stderr, fmt, ap); fputs(": ", stderr); }
    fprintf(stderr, "%s\n", strerror(e));
}

void vwarnx(const char *fmt, va_list ap)
{
    fprintf(stderr, "%s: ", __progname);
    if (fmt) vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

void verr(int code, const char *fmt, va_list ap) { vwarn(fmt, ap); exit(code); }
void verrx(int code, const char *fmt, va_list ap) { vwarnx(fmt, ap); exit(code); }
void warn(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vwarn(fmt, ap); va_end(ap); }
void warnx(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vwarnx(fmt, ap); va_end(ap); }
void err(int code, const char *fmt, ...) { va_list ap; va_start(ap, fmt); vwarn(fmt, ap); va_end(ap); exit(code); }
void errx(int code, const char *fmt, ...) { va_list ap; va_start(ap, fmt); vwarnx(fmt, ap); va_end(ap); exit(code); }

/* ================= dlfcn.h (stub) ================= */

static const char *dl_msg;

void *dlopen(const char *file, int mode)
{
    (void)mode;
    if (!file) { dl_msg = NULL; return (void *)&dl_msg; }   /* নিজের প্রোগ্রাম: চিহ্ন হিসেবে */
    dl_msg = "dlopen: rozOS does not support shared libraries yet (link statically)";
    return NULL;
}

void *dlsym(void *handle, const char *name)
{
    (void)handle;
    (void)name;
    dl_msg = "dlsym: symbol lookup is not available (static binary)";
    return NULL;
}

int dlclose(void *handle) { (void)handle; return 0; }

char *dlerror(void)
{
    const char *m = dl_msg;
    dl_msg = NULL;
    return (char *)m;
}

int dladdr(const void *addr, Dl_info *info) { (void)addr; (void)info; return 0; }

/* ================= search.h ================= */

struct tnode {
    const void *key;             /* প্রথম সদস্য: POSIX অনুযায়ী node = key-এর ঠিকানা */
    struct tnode *l, *r;
    int h;
};

static int th(const struct tnode *n) { return n ? n->h : 0; }
static void tfix(struct tnode *n) { int a = th(n->l), b = th(n->r); n->h = (a > b ? a : b) + 1; }

static struct tnode *trot(struct tnode *n, int right)
{
    struct tnode *x = right ? n->l : n->r;
    if (right) { n->l = x->r; x->r = n; }
    else { n->r = x->l; x->l = n; }
    tfix(n);
    tfix(x);
    return x;
}

static struct tnode *tbal(struct tnode *n)
{
    int d;
    tfix(n);
    d = th(n->l) - th(n->r);
    if (d > 1) {
        if (th(n->l->l) < th(n->l->r)) n->l = trot(n->l, 0);
        return trot(n, 1);
    }
    if (d < -1) {
        if (th(n->r->r) < th(n->r->l)) n->r = trot(n->r, 1);
        return trot(n, 0);
    }
    return n;
}

static struct tnode *tins(struct tnode *n, const void *key, int (*cmp)(const void *, const void *),
                          struct tnode **found)
{
    int c;
    if (!n) {
        n = (struct tnode *)calloc(1, sizeof(*n));
        if (!n) { *found = NULL; return NULL; }
        n->key = key;
        n->h = 1;
        *found = n;
        return n;
    }
    c = cmp(key, n->key);
    if (c == 0) { *found = n; return n; }
    if (c < 0) {
        struct tnode *k = tins(n->l, key, cmp, found);
        if (!k) return n;
        n->l = k;
    } else {
        struct tnode *k = tins(n->r, key, cmp, found);
        if (!k) return n;
        n->r = k;
    }
    return tbal(n);
}

void *tsearch(const void *key, void **root, int (*cmp)(const void *, const void *))
{
    struct tnode *found = NULL;
    struct tnode *nr;
    if (!root) return NULL;
    nr = tins((struct tnode *)*root, key, cmp, &found);
    if (nr) *root = nr;
    return found;
}

void *tfind(const void *key, void *const *root, int (*cmp)(const void *, const void *))
{
    struct tnode *n;
    if (!root) return NULL;
    n = (struct tnode *)*root;
    while (n) {
        int c = cmp(key, n->key);
        if (!c) return n;
        n = c < 0 ? n->l : n->r;
    }
    return NULL;
}

static struct tnode *tdel(struct tnode *n, const void *key, int (*cmp)(const void *, const void *),
                          int *done)
{
    int c;
    if (!n) return NULL;
    c = cmp(key, n->key);
    if (c < 0) n->l = tdel(n->l, key, cmp, done);
    else if (c > 0) n->r = tdel(n->r, key, cmp, done);
    else {
        *done = 1;
        if (!n->l || !n->r) {
            struct tnode *k = n->l ? n->l : n->r;
            free(n);
            return k;
        } else {
            struct tnode *m = n->r;
            int d2 = 0;
            while (m->l) m = m->l;
            n->key = m->key;
            n->r = tdel(n->r, m->key, cmp, &d2);
        }
    }
    return tbal(n);
}

void *tdelete(const void *key, void **root, int (*cmp)(const void *, const void *))
{
    struct tnode *n, *parent = NULL;
    int done = 0;
    if (!root || !*root) return NULL;
    n = (struct tnode *)*root;
    while (n) {
        int c = cmp(key, n->key);
        if (!c) break;
        parent = n;
        n = c < 0 ? n->l : n->r;
    }
    if (!n) return NULL;
    *root = tdel((struct tnode *)*root, key, cmp, &done);
    /* POSIX: মোছা node-এর মা-বাবা; root মুছলে অশূন্য কিছু */
    return parent ? (void *)parent : (*root ? *root : (void *)root);
}

static void twalk_r(const struct tnode *n, void (*fn)(const void *, VISIT, int), int depth)
{
    if (!n) return;
    if (!n->l && !n->r) { fn(n, leaf, depth); return; }
    fn(n, preorder, depth);
    twalk_r(n->l, fn, depth + 1);
    fn(n, postorder, depth);
    twalk_r(n->r, fn, depth + 1);
    fn(n, endorder, depth);
}

void twalk(const void *root, void (*fn)(const void *node, VISIT v, int depth))
{
    twalk_r((const struct tnode *)root, fn, 0);
}

void tdestroy(void *root, void (*freefn)(void *))
{
    struct tnode *n = (struct tnode *)root;
    if (!n) return;
    tdestroy(n->l, freefn);
    tdestroy(n->r, freefn);
    if (freefn) freefn((void *)n->key);
    free(n);
}

void *lfind(const void *key, const void *base, size_t *n, size_t size,
            int (*cmp)(const void *, const void *))
{
    const char *b = (const char *)base;
    size_t i;
    for (i = 0; i < *n; i++) if (!cmp(key, b + i * size)) return (void *)(b + i * size);
    return NULL;
}

void *lsearch(const void *key, void *base, size_t *n, size_t size,
              int (*cmp)(const void *, const void *))
{
    void *r = lfind(key, base, n, size, cmp);
    if (r) return r;
    r = (char *)base + *n * size;
    memcpy(r, key, size);
    (*n)++;
    return r;
}

static ENTRY *htab;
static size_t hsize, hused;

int hcreate(size_t n)
{
    size_t s = 16;
    if (htab) return 0;
    while (s < n * 2) s <<= 1;
    htab = (ENTRY *)calloc(s, sizeof(ENTRY));
    if (!htab) return 0;
    hsize = s;
    hused = 0;
    return 1;
}

void hdestroy(void)
{
    free(htab);
    htab = NULL;
    hsize = hused = 0;
}

ENTRY *hsearch(ENTRY e, ACTION a)
{
    size_t h = 5381, i;
    const unsigned char *p;
    if (!htab) { errno = ESRCH; return NULL; }
    for (p = (const unsigned char *)e.key; *p; p++) h = h * 33 + *p;
    for (i = 0; i < hsize; i++) {
        ENTRY *slot = &htab[(h + i) & (hsize - 1)];
        if (!slot->key) {
            if (a == FIND) { errno = ESRCH; return NULL; }
            if (hused + 1 >= hsize) { errno = ENOMEM; return NULL; }
            *slot = e;
            hused++;
            return slot;
        }
        if (!strcmp(slot->key, e.key)) return slot;
    }
    errno = ENOMEM;
    return NULL;
}

struct qelem { struct qelem *next, *prev; };

void insque(void *elem, void *prev)
{
    struct qelem *e = (struct qelem *)elem, *p = (struct qelem *)prev;
    if (!p) { e->next = e->prev = NULL; return; }
    e->next = p->next;
    e->prev = p;
    p->next = e;
    if (e->next) e->next->prev = e;
}

void remque(void *elem)
{
    struct qelem *e = (struct qelem *)elem;
    if (e->next) e->next->prev = e->prev;
    if (e->prev) e->prev->next = e->next;
}
