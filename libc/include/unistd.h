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

extern char *optarg;
extern int optind, opterr, optopt;
int     getopt(int argc, char *const argv[], const char *opts);

#endif
