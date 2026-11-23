/* rozlibc — syscall-এর মূল ও POSIX wrapper (Linux x86-64 নম্বর) */

#include <sys/sysinfo.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <poll.h>
#include <termios.h>
#include <pty.h>
#include <sys/time.h>
#include <signal.h>
#include <time.h>
#include <rozos.h>
#include <stdlib.h>
#include <stdio.h>

void __rozlibc_dirfd_set(int fd, const char *path);    /* ধাপ ১৯জ (নিচে) */
#include <sys/mount.h>

#define SYS_read            0
#define SYS_write           1
#define SYS_open            2
#define SYS_close           3
#define SYS_stat            4
#define SYS_fstat           5
#define SYS_lseek           8
#define SYS_mmap            9
#define SYS_mprotect        10
#define SYS_munmap          11
#define SYS_brk             12
#define SYS_ioctl           16
#define SYS_dup             32
#define SYS_dup2            33
#define SYS_pipe            22      /* ধাপ ১৩ */
#define SYS_pipe2           293
#define SYS_poll            7
#define SYS_nanosleep       35
#define SYS_getpid          39
#define SYS_fork            57
#define SYS_execve          59
#define SYS_exit            60
#define SYS_wait4           61
#define SYS_kill            62
#define SYS_rt_sigaction    13      /* ধাপ ১৪ */
#define SYS_setpgid         109
#define SYS_getpgrp         111
#define SYS_setsid          112
#define SYS_getpgid         121
#define SYS_getsid          124
#define SYS_rozos_openpty   505
#define SYS_init_module     175
#define SYS_delete_module   176
#define SYS_rozos_modlist   506
#define SYS_mount           165
#define SYS_umount2         166
#define SYS_rozos_mounts    507
#define SYS_rozos_usblist   508
#define SYS_rozos_proclist  509     /* ধাপ ১৯চ */
#define SYS_sysinfo         99
#define SYS_statfs          137
#define SYS_uname           63
#define SYS_truncate        76
#define SYS_ftruncate       77      /* ধাপ ১৩ */
#define SYS_rozos_shm_open  503
#define SYS_rozos_shm_unlink 504
#define SYS_getcwd          79
#define SYS_chdir           80
#define SYS_mkdir           83
#define SYS_rmdir           84
#define SYS_unlink          87
#define SYS_rename          82
#define SYS_getppid         110
#define SYS_clock_gettime   228

/* ধাপ ১৬খ-২: `errno` চলকটা pthread.c-তে (এখানে errno = থ্রেডের নিজের, ম্যাক্রো দিয়ে) */

long __syscall6(long n, long a, long b, long c, long d, long e, long f)
{
    long ret;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    __asm__ __volatile__("syscall"
                         : "=a"(ret)
                         : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                         : "rcx", "r11", "memory");
    return ret;
}

long __syscall_int80(long n, long a, long b, long c)
{
    long ret;
    __asm__ __volatile__("int $0x80" : "=a"(ret) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory");
    return ret;
}

long __syscall_ret(long r)
{
    if (r < 0 && r > -4096) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

#define SC3(n, a, b, c) __syscall_ret(__syscall3((n), (a), (b), (c)))

ssize_t read(int fd, void *buf, size_t n)        { return SC3(SYS_read, fd, buf, n); }
ssize_t write(int fd, const void *buf, size_t n) { return SC3(SYS_write, fd, buf, n); }
int     close(int fd)                            { __rozlibc_dirfd_set(fd, NULL); return (int)SC3(SYS_close, fd, 0, 0); }
off_t   lseek(int fd, off_t off, int whence)     { return SC3(SYS_lseek, fd, off, whence); }
int     dup(int fd)                              { return (int)SC3(SYS_dup, fd, 0, 0); }
int     dup2(int a, int b)                       { return (int)SC3(SYS_dup2, a, b, 0); }
int     pipe(int fds[2])                         { return (int)SC3(SYS_pipe, fds, 0, 0); }
int     pipe2(int fds[2], int flags)             { return (int)SC3(SYS_pipe2, fds, flags, 0); }
int     poll(struct pollfd *fds, nfds_t n, int timeout) { return (int)SC3(SYS_poll, fds, n, timeout); }
int     unlink(const char *p)                    { return (int)SC3(SYS_unlink, p, 0, 0); }
int     rename(const char *a, const char *b)     { return (int)SC3(SYS_rename, a, b, 0); }   /* ধাপ ১৫ক */
int     rmdir(const char *p)                     { return (int)SC3(SYS_rmdir, p, 0, 0); }
int     chdir(const char *p)                     { return (int)SC3(SYS_chdir, p, 0, 0); }
int     truncate(const char *p, off_t len)       { return (int)SC3(SYS_truncate, p, len, 0); }
int     ftruncate(int fd, off_t len)             { return (int)SC3(SYS_ftruncate, fd, len, 0); }
int     shm_open(const char *name, int oflag, mode_t mode) { return (int)SC3(SYS_rozos_shm_open, name, oflag, mode); }
int     shm_unlink(const char *name)             { return (int)SC3(SYS_rozos_shm_unlink, name, 0, 0); }
int     mkdir(const char *p, mode_t m)           { return (int)SC3(SYS_mkdir, p, m, 0); }
int     stat(const char *p, struct stat *st)     { return (int)SC3(SYS_stat, p, st, 0); }
int     lstat(const char *p, struct stat *st)    { return stat(p, st); }
int     fstat(int fd, struct stat *st)           { return (int)SC3(SYS_fstat, fd, st, 0); }
/* ধাপ ১৬খ-২: pthread_atfork-এর handler আর সন্তানের নতুন tid */
void __roz_atfork(int phase);
void __roz_after_fork_child(void);
pid_t fork(void)
{
    pid_t r;
    __roz_atfork(0);
    r = (pid_t)SC3(SYS_fork, 0, 0, 0);
    if (r == 0) {
        __roz_after_fork_child();
        __roz_atfork(2);
    } else {
        __roz_atfork(1);
    }
    return r;
}
pid_t   getpid(void)                             { return (pid_t)SC3(SYS_getpid, 0, 0, 0); }
pid_t   getppid(void)                            { return (pid_t)SC3(SYS_getppid, 0, 0, 0); }
int     kill(pid_t pid, int sig)                 { return (int)SC3(SYS_kill, pid, sig, 0); }
/* ধাপ ১৪: process group, session, PTY, termios */
int     setpgid(pid_t pid, pid_t pg)             { return (int)SC3(SYS_setpgid, pid, pg, 0); }
pid_t   getpgid(pid_t pid)                       { return (pid_t)SC3(SYS_getpgid, pid, 0, 0); }
pid_t   getpgrp(void)                            { return (pid_t)SC3(SYS_getpgrp, 0, 0, 0); }
pid_t   setsid(void)                             { return (pid_t)SC3(SYS_setsid, 0, 0, 0); }
pid_t   getsid(pid_t pid)                        { return (pid_t)SC3(SYS_getsid, pid, 0, 0); }
int     killpg(pid_t pg, int sig)                { return kill(-pg, sig); }
pid_t   tcgetpgrp(int fd)
{
    int pg;
    if (ioctl(fd, 0x540F, &pg) < 0) return -1;
    return (pid_t)pg;
}
int     tcsetpgrp(int fd, pid_t pg)
{
    int v = (int)pg;
    return ioctl(fd, 0x5410, &v);
}
int     tcgetattr(int fd, struct termios *t)     { return ioctl(fd, 0x5401, t); }
int     tcsetattr(int fd, int act, const struct termios *t)
{
    (void)act;
    return ioctl(fd, 0x5402, (void *)t);
}
void    cfmakeraw(struct termios *t)
{
    t->c_iflag = 0;
    t->c_oflag = 0;
    t->c_lflag &= ~(ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHONL | IEXTEN);
}
/* ধাপ ১৫ক: কার্নেল মডিউল */
int  rozos_init_module(const void *image, unsigned long len, const char *name) { return (int)SC3(SYS_init_module, image, len, name); }
int  rozos_delete_module(const char *name) { return (int)SC3(SYS_delete_module, name, 0, 0); }
long rozos_module_list(char *buf, unsigned long len, int ks) { return SC3(SYS_rozos_modlist, buf, len, ks); }
long rozos_mounts(char *buf, unsigned long len) { return SC3(SYS_rozos_mounts, buf, len, 0); }
long rozos_usb_list(char *buf, unsigned long len) { return SC3(SYS_rozos_usblist, buf, len, 0); }

/* ধাপ ১৯চ: প্রসেস-তালিকা, sysinfo, statfs/statvfs (Settings-এর System, Processes, Disks) */
long rozos_proc_list(char *buf, unsigned long len) { return SC3(SYS_rozos_proclist, buf, len, 0); }
int  sysinfo(struct sysinfo *info) { return (int)SC3(SYS_sysinfo, info, 0, 0); }
int  statfs(const char *path, struct statfs *buf) { return (int)SC3(SYS_statfs, path, buf, 0); }

int statvfs(const char *path, struct statvfs *v)
{
    struct statfs s;
    if (statfs(path, &s) < 0) return -1;
    v->f_bsize = (unsigned long)s.f_bsize;
    v->f_frsize = (unsigned long)(s.f_frsize ? s.f_frsize : s.f_bsize);
    v->f_blocks = s.f_blocks;
    v->f_bfree = s.f_bfree;
    v->f_bavail = s.f_bavail;
    v->f_files = s.f_files;
    v->f_ffree = v->f_favail = s.f_ffree;
    v->f_fsid = (unsigned long)(unsigned)s.f_fsid.__val[0];
    v->f_flag = (unsigned long)(s.f_flags & 1 ? ST_RDONLY : 0);
    v->f_namemax = (unsigned long)s.f_namelen;
    return 0;
}
int  mount(const char *src, const char *tgt, const char *type, unsigned long flags, const void *data)
{
    return (int)__syscall_ret(__syscall6(SYS_mount, (long)src, (long)tgt, (long)type, (long)flags, (long)data, 0));
}
int  umount2(const char *tgt, int flags) { return (int)SC3(SYS_umount2, tgt, flags, 0); }
int  umount(const char *tgt) { return umount2(tgt, 0); }

int     openpty(int *master, int *slave, char *name, const struct termios *tp, const struct winsize *wp)
{
    int fds[2];
    if ((int)SC3(SYS_rozos_openpty, fds, 0, 0) < 0) return -1;
    *master = fds[0];
    *slave = fds[1];
    if (name) name[0] = '\0';
    if (tp) tcsetattr(fds[1], 0, tp);
    if (wp) ioctl(fds[0], TIOCSWINSZ, (void *)wp);
    return 0;
}
int     uname(struct utsname *u)                 { return (int)SC3(SYS_uname, u, 0, 0); }
int     munmap(void *a, size_t len)              { return (int)SC3(SYS_munmap, a, len, 0); }
int     mprotect(void *a, size_t len, int prot)  { return (int)SC3(SYS_mprotect, a, len, prot); }
int     clock_gettime(clockid_t c, struct timespec *ts) { return (int)SC3(SYS_clock_gettime, c, ts, 0); }
int     nanosleep(const struct timespec *r, struct timespec *m) { return (int)SC3(SYS_nanosleep, r, m, 0); }

/* rozOS-এ এখনো ইউজার/অনুমতি নেই: সবাই root */
uid_t getuid(void)  { return 0; }
uid_t geteuid(void) { return 0; }
gid_t getgid(void)  { return 0; }
gid_t getegid(void) { return 0; }
mode_t umask(mode_t m) { (void)m; return 022; }
int chmod(const char *p, mode_t m) { struct stat st; (void)m; return stat(p, &st); }

/* ধাপ ১৯জ: O_DIRECTORY দিয়ে খোলা ফোল্ডারের পুরো পথ মনে রাখা — openat/fstatat/unlinkat ...
 * আসল dirfd + আপেক্ষিক পথ পেলে "পথ/নাম" বানিয়ে চলে (কার্নেলে *at syscall নেই)।
 * sbase-এর tar, du, chmod -R এভাবে ফোল্ডারের ভেতরে ঘোরে। */
static char *dirfd_path[64];

void __rozlibc_dirfd_set(int fd, const char *path)
{
    if (fd < 0 || fd >= 64) return;
    free(dirfd_path[fd]);
    dirfd_path[fd] = NULL;
    if (!path) return;
    if (path[0] == '/') {
        dirfd_path[fd] = strdup(path);
    } else {
        char cwd[1024];
        size_t n;
        if (!getcwd(cwd, sizeof(cwd))) return;
        n = strlen(cwd) + strlen(path) + 2;
        if ((dirfd_path[fd] = (char *)malloc(n)))
            snprintf(dirfd_path[fd], n, "%s%s%s", cwd, strcmp(cwd, "/") ? "/" : "", path);
    }
}

const char *__rozlibc_dirfd_get(int fd)
{
    return fd >= 0 && fd < 64 ? dirfd_path[fd] : NULL;
}

int open(const char *path, int flags, ...)
{
    va_list ap;
    int mode = 0, fd;
    va_start(ap, flags);
    if (flags & O_CREAT) {
        mode = va_arg(ap, int);
    }
    va_end(ap);
    fd = (int)SC3(SYS_open, path, flags, mode);
    if (fd >= 0 && (flags & O_DIRECTORY)) __rozlibc_dirfd_set(fd, path);
    return fd;
}

int creat(const char *path, mode_t mode)
{
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

int execve(const char *path, char *const argv[], char *const envp[])
{
    return (int)SC3(SYS_execve, path, argv, envp);
}

/* ধাপ ১৬খ-২: exit_group (231) — সব থ্রেডসহ প্রসেস শেষ (SYS_exit এখন শুধু একটা থ্রেড) */
void _exit(int code)
{
    for (;;) {
        __syscall3(231, code, 0, 0);
        __syscall3(SYS_exit, code, 0, 0);
    }
}

pid_t waitpid(pid_t pid, int *status, int options)
{
    return (pid_t)__syscall_ret(__syscall6(SYS_wait4, pid, (long)status, options, 0, 0, 0));
}

pid_t wait(int *status)
{
    return waitpid(-1, status, 0);
}

char *getcwd(char *buf, size_t size)
{
    long r = __syscall3(SYS_getcwd, buf, size, 0);
    if (r < 0) {
        errno = (int)-r;
        return NULL;
    }
    return buf;
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
    long r = __syscall6(SYS_mmap, (long)addr, (long)len, prot, flags, fd, off);
    if (r < 0 && r > -4096) {
        errno = (int)-r;
        return MAP_FAILED;
    }
    return (void *)r;
}

int ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    void *arg;
    va_start(ap, req);
    arg = va_arg(ap, void *);
    va_end(ap);
    return (int)SC3(SYS_ioctl, fd, req, arg);
}

int isatty(int fd)
{
    struct winsize ws;
    return ioctl(fd, TIOCGWINSZ, &ws) == 0;
}

int access(const char *path, int mode)
{
    struct stat st;
    (void)mode;
    return stat(path, &st);
}

/* ---------- brk/sbrk ---------- */

static char *cur_brk;

int brk(void *addr)
{
    char *r = (char *)__syscall3(SYS_brk, addr, 0, 0);
    cur_brk = r;
    if (r != (char *)addr) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

void *sbrk(long inc)
{
    char *old;
    if (!cur_brk) {
        cur_brk = (char *)__syscall3(SYS_brk, 0, 0, 0);
    }
    old = cur_brk;
    if (inc == 0) {
        return old;
    }
    if (brk(old + inc) < 0) {
        return (void *)-1;
    }
    return old;
}

/* ---------- সময় ---------- */

unsigned sleep(unsigned seconds)
{
    struct timespec ts;
    ts.tv_sec = seconds;
    ts.tv_nsec = 0;
    nanosleep(&ts, NULL);
    return 0;
}

int usleep(unsigned long usec)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(usec / 1000000);
    ts.tv_nsec = (long)(usec % 1000000) * 1000;
    return nanosleep(&ts, NULL);
}

time_t time(time_t *t)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    if (t) {
        *t = ts.tv_sec;
    }
    return ts.tv_sec;
}

int gettimeofday(struct timeval *tv, void *tz)
{
    struct timespec ts;
    (void)tz;
    clock_gettime(CLOCK_REALTIME, &ts);
    tv->tv_sec = ts.tv_sec;
    tv->tv_usec = ts.tv_nsec / 1000;
    return 0;
}

static long start_ms = -1;

clock_t clock(void)
{
    /* প্রসেসের CPU সময়ের হিসাব নেই — শুরুর পর থেকে কেটে যাওয়া সময় */
    long now = rozos_now_ms();
    if (start_ms < 0) {
        start_ms = now;
    }
    return (clock_t)((now - start_ms) * 1000);
}

long rozos_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int rozos_kmon(void)
{
    return (int)SC3(ROZOS_SYS_KMON, 0, 0, 0);
}

int gethostname(char *name, size_t len)
{
    struct utsname u;
    if (uname(&u) < 0) {
        return -1;
    }
    strncpy(name, u.nodename, len);
    if (len) {
        name[len - 1] = '\0';
    }
    return 0;
}

/* ---------- signal() ধাপ ১৬খ-১ থেকে libc/src/signal.c-তে (আসল handler) ---------- */

int raise(int sig)
{
    return kill(getpid(), sig);
}
