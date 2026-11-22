/* rozlibc — unistd.h (POSIX-এর যা rozOS-এ আছে) */
#ifndef _UNISTD_H
#define _UNISTD_H

#include <_rozlibc.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
int     close(int fd);
off_t   lseek(int fd, off_t off, int whence);
int     dup(int fd);
int     dup2(int oldfd, int newfd);
int     pipe(int fds[2]);                 /* ধাপ ১৩: fds[0] পড়া, fds[1] লেখা */
int     pipe2(int fds[2], int flags);
int     unlink(const char *path);
int     rmdir(const char *path);
int     chdir(const char *path);
char   *getcwd(char *buf, size_t size);
int     truncate(const char *path, off_t len);
int     ftruncate(int fd, off_t len);         /* ধাপ ১৩ */
int     access(const char *path, int mode);
int     isatty(int fd);

pid_t   fork(void);
int     execve(const char *path, char *const argv[], char *const envp[]);
int     execv(const char *path, char *const argv[]);
int     execvp(const char *file, char *const argv[]);
int     execl(const char *path, const char *arg0, ...);    /* ধাপ ১৪: শেষে (char *)NULL */
int     execlp(const char *file, const char *arg0, ...);
void    _exit(int code) __attribute__((noreturn));
pid_t   getpid(void);
pid_t   getppid(void);
/* ধাপ ১৪: process group আর session (Ctrl+C পুরো কাজকে থামায়) */
int     setpgid(pid_t pid, pid_t pgid);
pid_t   getpgid(pid_t pid);
pid_t   getpgrp(void);
pid_t   setsid(void);
pid_t   getsid(pid_t pid);
pid_t   tcgetpgrp(int fd);
int     tcsetpgrp(int fd, pid_t pgrp);
uid_t   getuid(void);
uid_t   geteuid(void);
gid_t   getgid(void);
gid_t   getegid(void);

unsigned sleep(unsigned seconds);
int     usleep(unsigned long usec);
void   *sbrk(long inc);
int     brk(void *addr);
int     gethostname(char *name, size_t len);

/* ---------- ধাপ ১৬ক ---------- */
#define _POSIX_VERSION 200809L
#define _SC_ARG_MAX          0
#define _SC_CHILD_MAX        1
#define _SC_CLK_TCK          2
#define _SC_NGROUPS_MAX      3
#define _SC_OPEN_MAX         4
#define _SC_PAGESIZE        30
#define _SC_PAGE_SIZE       30
#define _SC_NPROCESSORS_CONF 83
#define _SC_NPROCESSORS_ONLN 84
#define _SC_PHYS_PAGES      85
#define _SC_LINE_MAX        43
#define _SC_HOST_NAME_MAX  180
#define _SC_LOGIN_NAME_MAX  71
#define _SC_GETPW_R_SIZE_MAX 70
#define _SC_GETGR_R_SIZE_MAX 69
#define _SC_TTY_NAME_MAX    72
#define _PC_NAME_MAX         3
#define _PC_PATH_MAX         4
#define _PC_PIPE_BUF         5
#define _PC_LINK_MAX         0
ssize_t pread(int fd, void *buf, size_t n, off_t off);
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off);
int     fsync(int fd);
int     fdatasync(int fd);
void    sync(void);
int     getpagesize(void);
long    sysconf(int name);
long    pathconf(const char *path, int name);
long    fpathconf(int fd, int name);
int     execle(const char *path, const char *arg0, ...);   /* শেষে NULL, তারপর envp */
int     execvpe(const char *file, char *const argv[], char *const envp[]);
int     link(const char *oldpath, const char *newpath);
int     symlink(const char *target, const char *linkpath);
ssize_t readlink(const char *path, char *buf, size_t size);
int     chown(const char *path, uid_t uid, gid_t gid);
int     fchown(int fd, uid_t uid, gid_t gid);
int     lchown(const char *path, uid_t uid, gid_t gid);
int     fchdir(int fd);
int     setuid(uid_t uid);
int     setgid(gid_t gid);
int     seteuid(uid_t uid);
int     setegid(gid_t gid);
int     getgroups(int n, gid_t list[]);
char   *getlogin(void);
int     getlogin_r(char *buf, size_t n);
char   *ttyname(int fd);
int     ttyname_r(int fd, char *buf, size_t n);
int     nice(int inc);
unsigned alarm(unsigned seconds);      /* ধাপ ১৬খ-তে আসল SIGALRM; এখন ০ ফেরত */
int     pause(void);
char   *getpass(const char *prompt);
int     sethostname(const char *name, size_t len);
int     getdtablesize(void);
long    gethostid(void);
int     lockf(int fd, int cmd, off_t len);
#define F_ULOCK 0
#define F_LOCK  1
#define F_TLOCK 2
#define F_TEST  3
pid_t   gettid(void);                 /* ধাপ ১৬খ-২: থ্রেডের id */
extern char **environ;

extern char *optarg;
extern int optind, opterr, optopt;
int     getopt(int argc, char *const argv[], const char *opts);

#endif
