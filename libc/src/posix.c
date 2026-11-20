/* rozlibc — ধাপ ১১: POSIX-এর কিছু অংশ যা pdpmake (make) চায়
 *   string.h  stpcpy
 *   libgen.h  basename, dirname
 *   fnmatch.h fnmatch
 *   glob.h    glob, globfree  (wildcard শুধু শেষ অংশে)
 *   signal.h  sigaction ও sigset ফাংশন (signal()-এর মতো শুধু রেকর্ড)
 *   stdlib.h  system()
 *   stdio.h   popen/pclose — কার্নেলে pipe নেই, তাই /tmp-এর অস্থায়ী ফাইল দিয়ে
 *   sys/stat.h utimensat (কার্নেলে নেই — ENOSYS)
 *   sys/reboot.h reboot() (ধাপ ১১খ, syscall 169) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <signal.h>
#include <stdarg.h>
#include <libgen.h>
#include <fnmatch.h>
#include <glob.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/reboot.h>
#include <rozos.h>

/* ---------- string.h: stpcpy ---------- */

char *stpcpy(char *dst, const char *src)
{
    while ((*dst = *src++)) dst++;
    return dst;
}

/* ---------- libgen ---------- */

char *basename(char *path)
{
    char *e, *s;
    if (!path || !*path) return (char *)".";
    e = path + strlen(path) - 1;
    while (e > path && *e == '/') *e-- = '\0';
    if (e == path && *e == '/') return path;
    s = strrchr(path, '/');
    return s ? s + 1 : path;
}

char *dirname(char *path)
{
    char *e;
    if (!path || !*path) return (char *)".";
    e = path + strlen(path) - 1;
    while (e > path && *e == '/') e--;          /* শেষের '/' */
    while (e > path && *e != '/') e--;          /* শেষ অংশ */
    if (*e != '/') return (char *)".";
    while (e > path && *e == '/') e--;          /* মাঝের '/' */
    e[1] = '\0';
    return path;
}

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

/* ---------- glob ---------- */

static int has_magic(const char *s)
{
    return strpbrk(s, "*?[") != NULL;
}

static int glob_add(glob_t *g, const char *name)
{
    char **nv = (char **)realloc(g->gl_pathv, (g->gl_pathc + 2) * sizeof(char *));
    if (!nv) return -1;
    g->gl_pathv = nv;
    if (!(nv[g->gl_pathc] = strdup(name))) return -1;
    nv[++g->gl_pathc] = NULL;
    return 0;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int glob(const char *pattern, int flags, int (*errfunc)(const char *path, int err), glob_t *g)
{
    char dir[1024], full[1280];
    const char *base, *slash;
    DIR *d;
    struct dirent *e;

    g->gl_pathc = 0;
    g->gl_pathv = NULL;
    g->gl_offs = 0;
    slash = strrchr(pattern, '/');
    if (slash) {
        size_t n = (size_t)(slash - pattern);
        if (n >= sizeof(dir)) n = sizeof(dir) - 1;
        memcpy(dir, pattern, n);
        dir[n] = '\0';
        if (n == 0) strcpy(dir, "/");
        base = slash + 1;
    } else {
        strcpy(dir, ".");
        base = pattern;
    }
    if (!has_magic(base)) {
        struct stat st;
        if (stat(pattern, &st) == 0 || (flags & GLOB_NOCHECK)) {
            if (glob_add(g, pattern) < 0) return GLOB_NOSPACE;
            return 0;
        }
        return GLOB_NOMATCH;
    }
    d = opendir(dir);
    if (!d) {
        if (errfunc && errfunc(dir, errno)) return GLOB_ABORTED;
        if (flags & GLOB_ERR) return GLOB_ABORTED;
    } else {
        while ((e = readdir(d))) {
            if (fnmatch(base, e->d_name, FNM_PERIOD | (flags & GLOB_NOESCAPE ? FNM_NOESCAPE : 0))) continue;
            if (slash) snprintf(full, sizeof(full), "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", e->d_name);
            else snprintf(full, sizeof(full), "%s", e->d_name);
            if (flags & GLOB_MARK) {
                struct stat st;
                if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) strncat(full, "/", sizeof(full) - strlen(full) - 1);
            }
            if (glob_add(g, full) < 0) { closedir(d); return GLOB_NOSPACE; }
        }
        closedir(d);
    }
    if (g->gl_pathc == 0) {
        if (flags & GLOB_NOCHECK) return glob_add(g, pattern) < 0 ? GLOB_NOSPACE : 0;
        return GLOB_NOMATCH;
    }
    if (!(flags & GLOB_NOSORT)) qsort(g->gl_pathv, g->gl_pathc, sizeof(char *), cmp_str);
    return 0;
}

void globfree(glob_t *g)
{
    size_t i;
    for (i = 0; i < g->gl_pathc; i++) free(g->gl_pathv[i]);
    free(g->gl_pathv);
    g->gl_pathv = NULL;
    g->gl_pathc = 0;
}

/* ---------- sigaction ---------- */

int sigemptyset(sigset_t *set) { *set = 0; return 0; }
int sigfillset(sigset_t *set) { *set = ~0ul; return 0; }

int sigaddset(sigset_t *set, int sig)
{
    if (sig <= 0 || sig >= 64) { errno = EINVAL; return -1; }
    *set |= 1ul << sig;
    return 0;
}

int sigdelset(sigset_t *set, int sig)
{
    if (sig <= 0 || sig >= 64) { errno = EINVAL; return -1; }
    *set &= ~(1ul << sig);
    return 0;
}

int sigismember(const sigset_t *set, int sig)
{
    if (sig <= 0 || sig >= 64) { errno = EINVAL; return -1; }
    return (*set >> sig) & 1;
}

int sigaction(int sig, const struct sigaction *act, struct sigaction *old)
{
    sighandler_t prev;
    if (act) {
        prev = signal(sig, act->sa_handler);
    } else {
        prev = signal(sig, SIG_DFL);           /* পড়া: বসিয়ে আবার ফিরিয়ে দেওয়া */
        if (prev != SIG_ERR) signal(sig, prev);
    }
    if (prev == SIG_ERR) return -1;
    if (old) {
        memset(old, 0, sizeof(*old));
        old->sa_handler = prev;
    }
    return 0;
}

/* ---------- system ---------- */

int system(const char *command)
{
    pid_t pid;
    int status;
    sighandler_t old_int, old_quit;
    if (!command) return access("/bin/sh", X_OK) == 0;
    /* ধাপ ১৪ (POSIX-এর মতো): অপেক্ষার সময় ডাকা প্রোগ্রাম Ctrl+C-তে মরে না, কমান্ড মরে */
    old_int = signal(SIGINT, SIG_IGN);
    old_quit = signal(SIGQUIT, SIG_IGN);
    pid = fork();
    if (pid < 0) {
        signal(SIGINT, old_int);
        signal(SIGQUIT, old_quit);
        return -1;
    }
    if (pid == 0) {
        char *argv[4];
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        argv[0] = (char *)"sh";
        argv[1] = (char *)"-c";
        argv[2] = (char *)command;
        argv[3] = NULL;
        execv("/bin/sh", argv);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) { status = -1; break; }
    }
    signal(SIGINT, old_int);
    signal(SIGQUIT, old_quit);
    return status;
}

/* ---------- popen / pclose ---------- *
 * "r": কমান্ড পুরোটা চালিয়ে আউটপুট /tmp-এর ফাইলে, তারপর সেটা পড়তে দেওয়া।
 * "w": লেখা ফাইলে জমে, pclose-এ কমান্ড সেটা stdin হিসেবে পায়। */

struct popen_ent {
    FILE *f;
    char path[40];
    char *cmd;              /* "w"-এর জন্য, pclose-এ চলে */
    int status;
};
static struct popen_ent popen_tab[8];

static int run_sh(const char *command, int in_fd, int out_fd)
{
    pid_t pid = fork();
    int status;
    if (pid < 0) return -1;
    if (pid == 0) {
        char *argv[4];
        if (in_fd >= 0) { dup2(in_fd, 0); close(in_fd); }
        if (out_fd >= 0) { dup2(out_fd, 1); close(out_fd); }
        argv[0] = (char *)"sh";
        argv[1] = (char *)"-c";
        argv[2] = (char *)command;
        argv[3] = NULL;
        execv("/bin/sh", argv);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return status;
}

FILE *popen(const char *command, const char *mode)
{
    static unsigned seq;
    struct popen_ent *e = NULL;
    int i, fd;
    for (i = 0; i < 8; i++) if (!popen_tab[i].f) { e = &popen_tab[i]; break; }
    if (!e || !mode || (mode[0] != 'r' && mode[0] != 'w')) { errno = e ? EINVAL : EMFILE; return NULL; }
    snprintf(e->path, sizeof(e->path), "/tmp/.popen-%d-%u", (int)getpid(), seq++);
    if (mode[0] == 'r') {
        fd = open(e->path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) return NULL;
        fflush(stdout);
        e->status = run_sh(command, -1, fd);
        close(fd);
        e->cmd = NULL;
        e->f = fopen(e->path, "r");
    } else {
        e->cmd = strdup(command);
        e->status = 0;
        e->f = fopen(e->path, "w");
    }
    if (!e->f) { unlink(e->path); free(e->cmd); e->cmd = NULL; }
    return e->f;
}

int pclose(FILE *f)
{
    int i, st;
    for (i = 0; i < 8; i++) {
        struct popen_ent *e = &popen_tab[i];
        if (e->f != f) continue;
        fclose(f);
        e->f = NULL;
        st = e->status;
        if (e->cmd) {
            int fd = open(e->path, O_RDONLY);
            st = run_sh(e->cmd, fd, -1);
            if (fd >= 0) close(fd);
            free(e->cmd);
            e->cmd = NULL;
        }
        unlink(e->path);
        return st;
    }
    errno = EINVAL;
    return -1;
}

/* ---------- utimensat ---------- */

int utimensat(int dirfd, const char *path, const struct timespec times[2], int flags)
{
    (void)dirfd; (void)path; (void)times; (void)flags;
    errno = ENOSYS;
    return -1;
}

/* ---------- reboot (ধাপ ১১খ) ---------- */

int reboot(int cmd)
{
    return (int)__syscall_ret(__syscall6(169, (long)0xfee1deadL, 672274793L, (long)(unsigned)cmd, 0, 0, 0));
}

/* ---------- ধাপ ১৪: execl / execlp ---------- */

static int exec_list(const char *path, const char *arg0, va_list ap, int use_path)
{
    char *argv[64];
    int n = 0;
    argv[n++] = (char *)arg0;
    while (n < 63) {
        char *a = va_arg(ap, char *);
        argv[n++] = a;
        if (!a) break;
    }
    argv[63] = NULL;
    return use_path ? execvp(path, argv) : execv(path, argv);
}

int execl(const char *path, const char *arg0, ...)
{
    va_list ap;
    int r;
    va_start(ap, arg0);
    r = exec_list(path, arg0, ap, 0);
    va_end(ap);
    return r;
}

int execlp(const char *file, const char *arg0, ...)
{
    va_list ap;
    int r;
    va_start(ap, arg0);
    r = exec_list(file, arg0, ap, 1);
    va_end(ap);
    return r;
}
