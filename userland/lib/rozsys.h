/* rozOS — ইউজার প্রোগ্রামের জন্য ছোট syscall লাইব্রেরি (ধাপ ৬)
 *
 * ধাপ ৭-এ পূর্ণ libc (rozlibc) আসবে; এখন শুধু টেস্ট প্রোগ্রামগুলোর জন্য।
 * নম্বর ও ABI Linux x86-64-এর মতো (কার্নেলের rozos/syscall.h)। */
#ifndef ROZSYS_H
#define ROZSYS_H

typedef unsigned long size_t;
typedef long ssize_t;
typedef unsigned long uint64_t;
typedef long int64_t;
typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;

#define NULL ((void *)0)

#define SYS_read 0
#define SYS_write 1
#define SYS_open 2
#define SYS_close 3
#define SYS_stat 4
#define SYS_fstat 5
#define SYS_lseek 8
#define SYS_mmap 9
#define SYS_munmap 11
#define SYS_brk 12
#define SYS_sched_yield 24
#define SYS_dup2 33
#define SYS_nanosleep 35
#define SYS_getpid 39
#define SYS_fork 57
#define SYS_execve 59
#define SYS_exit 60
#define SYS_wait4 61
#define SYS_kill 62
#define SYS_uname 63
#define SYS_getcwd 79
#define SYS_chdir 80
#define SYS_mkdir 83
#define SYS_unlink 87
#define SYS_getppid 110
#define SYS_arch_prctl 158
#define SYS_getdents64 217
#define SYS_clock_gettime 228

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0x40
#define O_TRUNC 0x200
#define O_DIRECTORY 0x10000

#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20

#define WIFEXITED(s)   (((s) & 0x7F) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xFF)
#define WTERMSIG(s)    ((s) & 0x7F)

struct timespec { long tv_sec; long tv_nsec; };

static inline long syscall6(long n, long a, long b, long c, long d, long e, long f)
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

/* একই syscall, কিন্তু int 0x80 পথে (ব্যাকআপ পথ পরীক্ষার জন্য) */
static inline long syscall_int80(long n, long a, long b, long c)
{
    long ret;
    __asm__ __volatile__("int $0x80"
                         : "=a"(ret)
                         : "a"(n), "D"(a), "S"(b), "d"(c)
                         : "memory");
    return ret;
}

#define syscall3(n, a, b, c) syscall6((n), (long)(a), (long)(b), (long)(c), 0, 0, 0)

static inline ssize_t read(int fd, void *b, size_t n) { return syscall3(SYS_read, fd, b, n); }
static inline ssize_t write(int fd, const void *b, size_t n) { return syscall3(SYS_write, fd, b, n); }
static inline int open(const char *p, int fl) { return (int)syscall3(SYS_open, p, fl, 0644); }
static inline int close(int fd) { return (int)syscall3(SYS_close, fd, 0, 0); }
static inline long lseek(int fd, long off, int wh) { return syscall3(SYS_lseek, fd, off, wh); }
static inline int getpid(void) { return (int)syscall3(SYS_getpid, 0, 0, 0); }
static inline int getppid(void) { return (int)syscall3(SYS_getppid, 0, 0, 0); }
static inline int fork(void) { return (int)syscall3(SYS_fork, 0, 0, 0); }
static inline int execve(const char *p, char *const *av, char *const *ev)
{ return (int)syscall3(SYS_execve, p, av, ev); }
static inline void _exit(int c) { syscall3(SYS_exit, c, 0, 0); for (;;) {} }
static inline int waitpid(int pid, int *st, int opt) { return (int)syscall6(SYS_wait4, pid, (long)st, opt, 0, 0, 0); }
static inline int kill(int pid, int sig) { return (int)syscall3(SYS_kill, pid, sig, 0); }
static inline void *sys_brk(void *a) { return (void *)syscall3(SYS_brk, a, 0, 0); }
static inline void *mmap(void *a, size_t len, int prot, int fl, int fd, long off)
{ return (void *)syscall6(SYS_mmap, (long)a, (long)len, prot, fl, fd, off); }
static inline int munmap(void *a, size_t len) { return (int)syscall3(SYS_munmap, a, len, 0); }
static inline int mkdir(const char *p) { return (int)syscall3(SYS_mkdir, p, 0755, 0); }
static inline int unlink(const char *p) { return (int)syscall3(SYS_unlink, p, 0, 0); }
static inline int chdir(const char *p) { return (int)syscall3(SYS_chdir, p, 0, 0); }
static inline long getcwd(char *b, size_t n) { return syscall3(SYS_getcwd, b, n, 0); }
static inline long getdents64(int fd, void *b, size_t n) { return syscall3(SYS_getdents64, fd, b, n); }
static inline int nanosleep(const struct timespec *r, struct timespec *m) { return (int)syscall3(SYS_nanosleep, r, m, 0); }
static inline int clock_gettime(int clk, struct timespec *ts) { return (int)syscall3(SYS_clock_gettime, clk, ts, 0); }
static inline int sched_yield(void) { return (int)syscall3(SYS_sched_yield, 0, 0, 0); }

/* ulib.c */
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
void  *memset(void *d, int c, size_t n);
void  *memcpy(void *d, const void *s, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
int    printf(const char *fmt, ...);
int    puts(const char *s);
long   now_ms(void);                 /* CLOCK_MONOTONIC মিলিসেকেন্ডে */
void   sleep_ms(long ms);

#endif
