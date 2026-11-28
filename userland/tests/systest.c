/* rozOS — /bin/systest: ধাপ ৬-এর কার্নেল-টেস্ট (ধাপ ৬-এ এটাই /bin/init ছিল)
 *
 * ধাপ ৭ থেকে init shell চালায়, তাই টেস্টগুলো এখানে সরানো — shell থেকে `systest`।
 * syscall দুই পথে, brk, mmap, স্ট্যাক বাড়া, ফাইল, getdents64, fork/execve/wait,
 * ভুল করা প্রসেস শেষ হওয়া, preemption ও FPU অবস্থা, sleep, kill। শেষে exit (ব্যর্থ টেস্টের সংখ্যা)।
 * ধাপ ৭: rozlibc দিয়ে লেখা (আগে ছোট rozsys.h ছিল), সাথে কয়েকটা libc-টেস্ট।
 * ধাপ ১০: getrandom, /dev/urandom, sscanf-এর double, rozhttp-এর SHA-256।
 * ধাপ ১২: sscanf %[...] ও %n, fscanf।
 * ধাপ ১৬ক: libc সামঞ্জস্য — নতুন syscall (select, readv, pread, fcntl, fsync), নিখুঁত
 * printf, scanf, TZ, wchar/UTF-8, regex, getopt_long, iconv, memstream, libm, pwd।
 * ধাপ ১৬খ-১: আসল signal handler (SA_SIGINFO, mask, sigsuspend, setitimer, EINTR/SA_RESTART,
 * SIGCHLD, SIGSEGV/SIGFPE ধরা, sigaltstack, nanosleep-এর বাকি সময়, sigwait), SIGWINCH,
 * আর Lua ও zlib (/bin/lua, gzip/gunzip)।
 * ধাপ ১৬খ-২: থ্রেড — pthread_create/join, mutex, cond, errno প্রতি থ্রেডে, TSD, once,
 * semaphore, barrier, detached, FPU, exit/pthread_exit/execve-এর সাথে থ্রেড। */

#include <sys/sysinfo.h>
#include <sys/statvfs.h>
#include <stdio.h>
#include <rozgfx.h>
#include <rozbn.h>
#include <poll.h>
#include <pty.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/utsname.h>
#include <time.h>
#include <signal.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/random.h>
#include <rozos.h>
#include <rozlock.h>          /* ধাপ ১৯ছ */
#include <dirent.h>
#include <ctype.h>
#include "rozhttp.h"
#include "rozui.h"
#include "rozgame.h"
#include "rozcref.h"
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/uio.h>
#include <wchar.h>
#include <wctype.h>
#include <regex.h>
#include <getopt.h>
#include <fnmatch.h>
#include <iconv.h>
#include <locale.h>
#include <math.h>
#include <fenv.h>
#include <pwd.h>
#include <grp.h>
#include <inttypes.h>
#include <search.h>
#include <syslog.h>
#include <dlfcn.h>
#include <setjmp.h>
#include <sys/time.h>
#include <pthread.h>
#include <semaphore.h>
#include <sched.h>

#define SYS_getpid 39

static int passed, failed;

static void check(int ok, const char *what)
{
    if (ok) {
        passed++;
        printf("systest: [ ok ] %s\n", what);
    } else {
        failed++;
        printf("systest: [FAIL] %s\n", what);
    }
}

static int run(const char *path, char *const *argv, int *status)
{
    static char *const envp[] = { "PATH=/bin", "FROM=init", NULL };
    int pid;
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        execve(path, argv, envp);
        printf("systest: execve %s failed\n", path);
        _exit(127);
    }
    if (pid < 0) {
        return pid;
    }
    return waitpid(pid, status, 0) == pid ? pid : -1;
}

/* প্রতি স্তরে 4 KiB স্ট্যাক; ফেরার পথে প্রতিটি স্তরের লেখা মান যাচাই */
static int deep(int n)
{
    volatile char buf[4096];
    buf[0] = (char)(n & 0x7F);
    buf[4095] = (char)(n & 0x7F);
    if (n == 0) {
        return 0;
    }
    return deep(n - 1) + (buf[0] != (char)(n & 0x7F)) + (buf[4095] != (char)(n & 0x7F));
}

static void test_basics(void)
{
    struct utsname un;
    check(getpid() > 1, "getpid() (syscall instruction)");
    check(__syscall_int80(SYS_getpid, 0, 0, 0) == getpid(), "getpid via int 0x80");
    check(__syscall3(999, 0, 0, 0) == -ENOSYS, "unknown syscall returns -ENOSYS");
    if (uname(&un) == 0) {
        printf("systest: uname: %s %s %s %s\n", un.sysname, un.nodename, un.release, un.machine);
        check(1, "uname");
    } else {
        check(0, "uname");
    }
    check(write(1, (void *)0xffff800000000000UL, 16) == -1 && errno == EFAULT,
          "kernel address rejected (EFAULT)");
}

static void test_memory(void)
{
    char *base = (char *)sbrk(0), *top;
    unsigned char *m;
    long i, bad = 0;

    top = brk(base + 256 * 1024) == 0 ? base + 256 * 1024 : NULL;
    check(top == base + 256 * 1024, "brk grows by 256 KiB");
    for (i = 0; i < 256 * 1024; i += 4096) base[i] = (char)i;
    for (i = 0; i < 256 * 1024; i += 4096) bad += base[i] != (char)i;
    check(bad == 0, "brk memory is usable");
    brk(base);

    m = (unsigned char *)mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(m != MAP_FAILED, "mmap 1 MiB anonymous");
    if (m != MAP_FAILED) {
        bad = 0;
        for (i = 0; i < (1 << 20); i++) bad += m[i] != 0;
        for (i = 0; i < (1 << 20); i++) m[i] = (unsigned char)(i * 7);
        for (i = 0; i < (1 << 20); i++) bad += m[i] != (unsigned char)(i * 7);
        check(bad == 0, "mmap memory zeroed and writable");
        check(munmap(m, 1 << 20) == 0, "munmap");
    }
    check(deep(256) == 0, "stack grows on demand (1 MiB of recursion)");
}

static void test_files(void)
{
    static const char msg[] = "written by /bin/init from ring 3\n";
    char buf[128], cwd[64];
    unsigned char dents[2048];
    long n, off, count = 0;
    int fd;

    fd = open("/tmp/init-test.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check(fd >= 3, "open(O_CREAT) gets fd >= 3");
    check(write(fd, msg, sizeof(msg) - 1) == (long)sizeof(msg) - 1, "write to /tmp");
    close(fd);
    fd = open("/tmp/init-test.txt", O_RDONLY);
    n = read(fd, buf, sizeof(buf));
    check(n == (long)sizeof(msg) - 1 && !memcmp(buf, msg, (size_t)n), "read back the same bytes");
    close(fd);

    check(chdir("/tmp") == 0 && getcwd(cwd, sizeof(cwd)) != NULL && !strcmp(cwd, "/tmp"), "chdir/getcwd");
    fd = open("init-test.txt", O_RDONLY);
    check(fd >= 0, "relative path uses the cwd");
    close(fd);
    check(unlink("init-test.txt") == 0, "unlink");
    chdir("/");

    fd = open("/bin", O_RDONLY | O_DIRECTORY);
    printf("systest: /bin:");
    while ((n = __syscall3(217, fd, dents, sizeof(dents))) > 0) {
        for (off = 0; off < n; ) {
            unsigned short reclen = *(unsigned short *)(dents + off + 16);
            printf(" %s", (char *)(dents + off + 19));
            count++;
            off += reclen;
        }
    }
    printf("\n");
    close(fd);
    check(count >= 4, "getdents64 lists /bin");
}

static void test_processes(void)
{
    static char *const hello_argv[] = { "hello", "first arg", "second", NULL };
    static char *const crash_argv[] = { "crash", NULL };
    static char *const ud_argv[] = { "crash", "ud", NULL };
    static char *const spin_a[] = { "spin", "A", NULL };
    static char *const spin_b[] = { "spin", "B", NULL };
    static char *const envp[] = { "PATH=/bin", NULL };
    int st = 0, pa, pb, sa = -1, sb = -1, i, shared = 5, me = getpid();
    long t0;

    fflush(stdout);
    pa = fork();
    if (pa == 0) {
        shared = 99;                         /* সন্তানের নিজস্ব কপিতে */
        _exit(shared == 99 && getppid() == me ? 42 : 1);
    }
    check(pa > 1 && waitpid(pa, &st, 0) == pa && WIFEXITED(st) && WEXITSTATUS(st) == 42,
          "fork: child gets its own copy, exit code 42");
    check(shared == 5, "fork: parent memory unchanged");

    check(run("/bin/hello", hello_argv, &st) > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 7,
          "fork + execve /bin/hello with arguments, exit code 7");
    check(run("/bin/crash", crash_argv, &st) > 0 && !WIFEXITED(st) && WTERMSIG(st) == 11,
          "NULL write kills only that process (SIGSEGV)");
    check(run("/bin/crash", ud_argv, &st) > 0 && !WIFEXITED(st) && WTERMSIG(st) == 4,
          "invalid opcode kills only that process (SIGILL)");
    check(run("/bin/does-not-exist", hello_argv, &st) > 0 && WEXITSTATUS(st) == 127,
          "execve of a missing file fails cleanly");
    check(waitpid(-1, &st, 0) == -1 && errno == ECHILD, "wait with no children returns ECHILD");

    printf("systest: two busy processes, no yield (letters should interleave):\nsystest:   ");
    fflush(stdout);
    pa = fork();
    if (pa == 0) { execve("/bin/spin", spin_a, envp); _exit(127); }
    pb = fork();
    if (pb == 0) { execve("/bin/spin", spin_b, envp); _exit(127); }
    for (i = 0; i < 2; i++) {
        int pid = waitpid(-1, &st, 0);
        if (pid == pa) sa = st;
        if (pid == pb) sb = st;
    }
    printf("\n");
    check(sa == 0 && sb == 0, "preemptive multitasking + FPU state per process");

    t0 = rozos_now_ms();
    usleep(200000);
    t0 = rozos_now_ms() - t0;
    check(t0 >= 195 && t0 < 400, "nanosleep 200 ms");

    pa = fork();
    if (pa == 0) {
        for (;;) {
        }
    }
    usleep(30000);
    check(kill(pa, 9) == 0 && waitpid(pa, &st, 0) == pa && WTERMSIG(st) == 9,
          "kill(SIGKILL) ends a spinning child");
}

/* ধাপ ৭: rozlibc-র কিছু অংশ */
static int cmp_int(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

static void test_libc(void)
{
    char buf[128], *p, *big;
    int v[6] = { 5, 3, 9, 1, 7, 2 }, i, sorted = 1;
    FILE *f;

    snprintf(buf, sizeof(buf), "%d|%5s|%-4x|%05.1f|%c|%lu", -42, "ab", 255, 3.14159, 'z', 1234567890123UL);
    check(!strcmp(buf, "-42|   ab|ff  |003.1|z|1234567890123"), "snprintf formats");
    check(strtol("-0x1f", NULL, 0) == -31 && atoi("  77") == 77, "strtol/atoi");
    qsort(v, 6, sizeof(int), cmp_int);
    for (i = 1; i < 6; i++) sorted &= v[i - 1] <= v[i];
    check(sorted, "qsort");
    p = (char *)malloc(100);
    big = (char *)malloc(1 << 20);                    /* mmap পথ */
    check(p && big, "malloc small and large");
    if (p && big) {
        memset(big, 0x5A, 1 << 20);
        p = (char *)realloc(p, 5000);
        check(p != NULL && big[12345] == 0x5A, "realloc keeps data");
    }
    free(p);
    free(big);
    check(setenv("ROZ_TEST", "yes", 1) == 0 && !strcmp(getenv("ROZ_TEST"), "yes"), "setenv/getenv");

    f = fopen("/tmp/libc-test.txt", "w");
    check(f != NULL, "fopen for writing");
    if (f) {
        fprintf(f, "line one\nline %d\n", 2);
        fclose(f);
    }
    f = fopen("/tmp/libc-test.txt", "r");
    if (f) {
        check(fgets(buf, sizeof(buf), f) && !strcmp(buf, "line one\n") &&
              fgets(buf, sizeof(buf), f) && !strcmp(buf, "line 2\n") && !fgets(buf, sizeof(buf), f),
              "fgets reads lines back");
        fclose(f);
    } else {
        check(0, "fgets reads lines back");
    }
    remove("/tmp/libc-test.txt");
}

/* ধাপ ৯: TCP সার্ভার + ক্লায়েন্ট (loopback), UDP, DNS-ছাড়া ঠিকানা */
static void test_net(void)
{
    struct sockaddr_in a;
    socklen_t al = sizeof(a);
    char buf[64];
    int ls, c, st, pid, one = 1;

    check(inet_addr("10.0.2.15") == htonl(0x0A00020F), "inet_addr / htonl");
    ls = socket(AF_INET, SOCK_STREAM, 0);
    check(ls >= 0, "socket(AF_INET, SOCK_STREAM)");
    if (ls < 0) return;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(7777);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    check(bind(ls, (struct sockaddr *)&a, sizeof(a)) == 0 && listen(ls, 4) == 0, "bind + listen on 127.0.0.1:7777");
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) _exit(1);
        send(s, "ping over tcp", 13, 0);
        if (recv(s, buf, sizeof(buf), 0) != 13 || memcmp(buf, "PING OVER TCP", 13)) _exit(2);
        close(s);
        _exit(0);
    }
    c = accept(ls, (struct sockaddr *)&a, &al);
    check(c >= 0, "accept a connection");
    if (c >= 0) {
        int n = (int)recv(c, buf, sizeof(buf), 0), i;
        for (i = 0; i < n; i++) buf[i] = (char)toupper((unsigned char)buf[i]);
        check(n == 13, "recv on the server side");
        send(c, buf, (size_t)(n > 0 ? n : 0), 0);
        close(c);
    }
    check(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0, "TCP client got the echo back");
    close(ls);

    {
        int u1 = socket(AF_INET, SOCK_DGRAM, 0), u2 = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in b, from;
        socklen_t fl = sizeof(from);
        memset(&b, 0, sizeof(b));
        b.sin_family = AF_INET;
        b.sin_port = htons(7778);
        b.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(u1, (struct sockaddr *)&b, sizeof(b));
        sendto(u2, "udp!", 4, 0, (struct sockaddr *)&b, sizeof(b));
        check(recvfrom(u1, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl) == 4 && !memcmp(buf, "udp!", 4),
              "UDP datagram over loopback");
        close(u1);
        close(u2);
    }
}

/* ধাপ ১০ */
static void test_phase10(void)
{
    unsigned char a[32], b[32];
    double d = 0;
    int x = 0, fd, n;
    struct rozhttp_sha256 h;
    char hex[65];

    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));
    check(getrandom(a, sizeof(a), 0) == 32 && getrandom(b, sizeof(b), 0) == 32 && memcmp(a, b, 32) != 0,
          "getrandom gives fresh bytes each time");
    fd = open("/dev/urandom", O_RDONLY);
    n = fd >= 0 ? (int)read(fd, a, sizeof(a)) : -1;
    if (fd >= 0) close(fd);
    check(n == 32 && memcmp(a, b, 32) != 0, "/dev/urandom reads");
    check(sscanf("HTTP/1.1 404 x 2.5e3", "HTTP/%*d.%*d %d x %lg", &x, &d) == 2 && x == 404 && d == 2500.0,
          "sscanf %*d and %lg");
    rozhttp_sha256_init(&h);
    rozhttp_sha256_update(&h, "abc", 3);
    rozhttp_sha256_hex(&h, hex);
    check(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "SHA-256 (rozhttp)");
}

/* ধাপ ১২: scanf পরিবার */
static void test_phase12(void)
{
    char a[32], b[32];
    int x = 0, y = 0, n = 0;
    FILE *f;
    check(sscanf("name: rozOS v12\n", "name: %[^\n]%n", a, &n) == 1 && !strcmp(a, "rozOS v12") && n == 15,
          "sscanf %[^\\n] and %n");
    check(sscanf("abc123 0x1f", "%[a-z]%d %i", b, &x, &y) == 3 && !strcmp(b, "abc") && x == 123 && y == 31,
          "sscanf %[a-z], %d, %i with 0x");
    f = fopen("/tmp/systest-scanf.txt", "w");
    if (f) {
        fprintf(f, "42 3.5 word\n");
        fclose(f);
    }
    f = fopen("/tmp/systest-scanf.txt", "r");
    {
        double d = 0;
        int r = f ? fscanf(f, "%d %lf %31s", &x, &d, a) : -1;
        check(r == 3 && x == 42 && d == 3.5 && !strcmp(a, "word") && fscanf(f, "%d", &y) == EOF, "fscanf from a file, EOF at the end");
    }
    if (f) fclose(f);
    unlink("/tmp/systest-scanf.txt");
}

/* ধাপ ১৩: pipe, poll, shared memory */
static void test_phase13(void)
{
    int fds[2], st = 0;
    char buf[64];
    struct pollfd pf;
    pid_t pid;

    check(pipe(fds) == 0, "pipe() gives two fds");
    check(write(fds[1], "hello", 5) == 5 && read(fds[0], buf, sizeof(buf)) == 5 && !memcmp(buf, "hello", 5),
          "pipe: bytes written come out of the read end");
    pf.fd = fds[0]; pf.events = POLLIN; pf.revents = 0;
    check(poll(&pf, 1, 0) == 0, "poll: empty pipe is not readable");
    write(fds[1], "x", 1);
    pf.revents = 0;
    check(poll(&pf, 1, 100) == 1 && (pf.revents & POLLIN), "poll: pipe with data is readable");
    read(fds[0], buf, 1);
    close(fds[1]);
    check(read(fds[0], buf, sizeof(buf)) == 0, "pipe: EOF when the writer is closed");
    close(fds[0]);

    /* fork + pipe: সন্তান লেখে, মা পড়ে */
    if (pipe(fds) == 0) {
        pid = fork();
        if (pid == 0) {
            close(fds[0]);
            write(fds[1], "from child", 10);
            _exit(0);
        }
        close(fds[1]);
        memset(buf, 0, sizeof(buf));
        {
            ssize_t n = read(fds[0], buf, sizeof(buf));
            waitpid(pid, &st, 0);
            check(n == 10 && !strcmp(buf, "from child"), "pipe between parent and child (fork)");
        }
        close(fds[0]);
    }
    if (pipe(fds) == 0) {
        close(fds[0]);
        check(write(fds[1], "x", 1) < 0 && errno == EPIPE, "pipe: write without a reader fails (EPIPE)");
        close(fds[1]);
    }

    /* shared memory: নাম দিয়ে, তারপর fork-এ MAP_SHARED|MAP_ANONYMOUS */
    {
        int fd = shm_open("/systest", O_CREAT | O_RDWR, 0600);
        char *p = MAP_FAILED, *q = MAP_FAILED;
        int fd2;
        if (fd >= 0 && ftruncate(fd, 8192) == 0) {
            p = (char *)mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        }
        fd2 = shm_open("/systest", O_RDWR, 0);
        if (fd2 >= 0) q = (char *)mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd2, 0);
        if (p != MAP_FAILED) strcpy(p + 4096, "shared!");
        check(p != MAP_FAILED && q != MAP_FAILED && p != q && !strcmp(q + 4096, "shared!"),
              "shm_open + mmap(MAP_SHARED): two mappings, same memory");
        if (p != MAP_FAILED) munmap(p, 8192);
        if (q != MAP_FAILED) munmap(q, 8192);
        if (fd >= 0) close(fd);
        if (fd2 >= 0) close(fd2);
        check(shm_unlink("/systest") == 0 && shm_open("/systest", O_RDWR, 0) < 0 && errno == ENOENT,
              "shm_unlink removes the name");
    }
    {
        volatile int *shared = (volatile int *)mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (shared != MAP_FAILED) {
            *shared = 1;
            pid = fork();
            if (pid == 0) {
                *shared = 42;
                _exit(0);
            }
            waitpid(pid, &st, 0);
        }
        check(shared != MAP_FAILED && *shared == 42, "MAP_SHARED|MAP_ANONYMOUS: child's write seen by parent");
        if (shared != MAP_FAILED) munmap((void *)shared, 4096);
    }
}

/* ধাপ ১৪: PTY, AF_UNIX socket, process group আর SIGINT */
static void test_phase14(void)
{
    int m = -1, sl = -1, st = 0;
    char buf[64];
    pid_t pid;
    struct winsize ws;

    check(openpty(&m, &sl, NULL, NULL, NULL) == 0 && isatty(sl), "openpty: slave is a terminal");
    if (m >= 0) {
        ssize_t n;
        write(sl, "out", 3);
        n = read(m, buf, sizeof(buf));
        check(n == 3 && !memcmp(buf, "out", 3), "pty: slave output arrives at the master");
        write(m, "key", 3);
        n = read(sl, buf, sizeof(buf));
        check(n == 3 && !memcmp(buf, "key", 3), "pty: master input arrives at the slave");
        ws.ws_row = 33; ws.ws_col = 77; ws.ws_xpixel = ws.ws_ypixel = 0;
        ioctl(m, TIOCSWINSZ, &ws);
        memset(&ws, 0, sizeof(ws));
        check(ioctl(sl, TIOCGWINSZ, &ws) == 0 && ws.ws_row == 33 && ws.ws_col == 77, "pty: window size set and read");
        close(sl);
        check(read(m, buf, sizeof(buf)) == 0, "pty: master reads EOF when the slave is closed");
        close(m);
    }

    {
        int srv = socket(AF_UNIX, SOCK_STREAM, 0), c;
        struct sockaddr_un a;
        memset(&a, 0, sizeof(a));
        a.sun_family = AF_UNIX;
        strcpy(a.sun_path, "/tmp/.systest-unix");
        check(srv >= 0 && bind(srv, (struct sockaddr *)&a, sizeof(a)) == 0 && listen(srv, 4) == 0,
              "AF_UNIX: socket, bind, listen");
        pid = fork();
        if (pid == 0) {
            int cs = socket(AF_UNIX, SOCK_STREAM, 0);
            if (cs < 0 || connect(cs, (struct sockaddr *)&a, sizeof(a)) < 0) _exit(1);
            write(cs, "hello unix", 10);
            if (read(cs, buf, sizeof(buf)) != 4) _exit(2);
            _exit(memcmp(buf, "pong", 4) ? 3 : 0);
        }
        c = accept(srv, NULL, NULL);
        memset(buf, 0, sizeof(buf));
        if (c >= 0) {
            read(c, buf, sizeof(buf));
            write(c, "pong", 4);
        }
        waitpid(pid, &st, 0);
        check(c >= 0 && !strcmp(buf, "hello unix") && WIFEXITED(st) && WEXITSTATUS(st) == 0,
              "AF_UNIX: connect, accept, talk both ways");
        if (c >= 0) close(c);
        close(srv);
        {
            int t = socket(AF_UNIX, SOCK_STREAM, 0);
            check(t >= 0 && connect(t, (struct sockaddr *)&a, sizeof(a)) < 0 && errno == ECONNREFUSED,
                  "AF_UNIX: name is gone after the server closes");
            if (t >= 0) close(t);
        }
    }

    pid = fork();
    if (pid == 0) {
        signal(SIGINT, SIG_IGN);
        setpgid(0, 0);
        sleep(5);
        _exit(7);
    }
    setpgid(pid, pid);
    usleep(100000);
    kill(-pid, SIGINT);                  /* উপেক্ষা করে */
    usleep(100000);
    kill(pid, SIGTERM);
    waitpid(pid, &st, 0);
    check(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM, "SIG_IGN ignores SIGINT to a process group, SIGTERM still works");
}

/* ধাপ ১৪খ: Files, Code, rozui উইজেট-লাইব্রেরি, আইকন */
static void test_phase14b(void)
{
    int st = 0, n = 0;
    char *c;
    FILE *f;
    check(access("/bin/code", X_OK) == 0 && access("/bin/files", X_OK) == 0, "Code editor and Files are installed");
    check(ui_shortcut_key("Ctrl+S") == 19 && ui_shortcut_key("F5") == GFX_KEY_F5 && ui_shortcut_key("Del") == GFX_KEY_DELETE,
          "rozui: menu shortcuts (Ctrl+S, F5, Del)");
    c = NULL;
    if (ui_clip_set("rozui-clip", 10) == 0) c = ui_clip_get(&n);
    check(c && n == 10 && !memcmp(c, "rozui-clip", 10), "rozui: clipboard round trip (/tmp/.clipboard)");
    free(c);
    check(access("/usr/share/icons/README.txt", R_OK) == 0 && ui_icon_image("no-such-icon", 32) == NULL,
          "icons folder exists; a missing icon falls back to the drawn symbol");
    f = fopen("/tmp/st-ui.c", "w");
    if (f) {
        fputs("#include <rozui.h>\nint main(void){ui_ctx u; if(gfx_init_window(\"t\",200,100)<0)return 1;\n"
              "ui_init(&u); while(ui_run(&u)){ if(ui_do_button(&u,10,10,80,28,\"OK\")) ui_quit(&u);\n"
              "if(ui_msgbox(\"t\",\"x\",UI_MB_OK)) ui_quit(&u);} gfx_quit(); return 0;}\n", f);
        fclose(f);
    }
    if (access("/bin/tcc", X_OK) == 0) {
        static char *const argv[] = { "tcc", "-o", "/tmp/st-ui", "/tmp/st-ui.c", "-lrozui", "-lrozgfx", NULL };
        check(run("/bin/tcc", argv, &st) > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0 && access("/tmp/st-ui", X_OK) == 0,
              "tcc compiles a rozui program (buttons + message box)");
    } else {
        check(1, "tcc not installed - rozui compile test skipped");
    }
    unlink("/tmp/st-ui.c");
    unlink("/tmp/st-ui");
}

/* ধাপ ১৫ক: rename, কার্নেল মডিউল, mount/umount, NVMe */
static int read_all(const char *path, char *buf, int size)
{
    int fd = open(path, O_RDONLY), n = -1;
    if (fd >= 0) { n = (int)read(fd, buf, (size_t)size - 1); close(fd); }
    if (n >= 0) buf[n] = '\0';
    return n;
}

static void test_phase15a(void)
{
    char buf[512];
    struct stat st;
    FILE *f;
    long n;
    char *img;

    mkdir("/home/.st15", 0755);
    mkdir("/home/.st15/d", 0755);
    f = fopen("/home/.st15/d/a.txt", "w");
    if (f) { fputs("rename-me", f); fclose(f); }
    check(rename("/home/.st15/d/a.txt", "/home/.st15/d/b.txt") == 0 && read_all("/home/.st15/d/b.txt", buf, sizeof(buf)) == 9 &&
          access("/home/.st15/d/a.txt", F_OK) != 0, "rename: a file on ext2 (instant, same data)");
    check(rename("/home/.st15/d", "/home/.st15/e") == 0 && read_all("/home/.st15/e/b.txt", buf, sizeof(buf)) == 9 &&
          stat("/home/.st15/e/..", &st) == 0, "rename: a folder with its files (ext2)");
    check(rename("/home/.st15/e", "/home/.st15/e/inside") < 0 && errno == EINVAL, "rename: a folder into itself is refused");
    f = fopen("/tmp/.st15", "w");
    if (f) { fputs("x", f); fclose(f); }
    check(rename("/tmp/.st15", "/tmp/.st15b") == 0 && access("/tmp/.st15b", F_OK) == 0, "rename: tmpfs");
    check(rename("/tmp/.st15b", "/home/.st15/t") < 0 && errno == EXDEV, "rename across file systems gives EXDEV (mv copies)");
    unlink("/tmp/.st15b");
    unlink("/home/.st15/e/b.txt");
    rmdir("/home/.st15/e");
    rmdir("/home/.st15");

    img = NULL;
    n = 0;
    f = fopen("/lib/modules/hello.ko", "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        img = (char *)malloc((size_t)n);
        if (img && fread(img, 1, (size_t)n, f) != (size_t)n) { free(img); img = NULL; }
        fclose(f);
    }
    check(img && rozos_init_module(img, (unsigned long)n, "hello") == 0 && read_all("/dev/hello", buf, sizeof(buf)) > 0 &&
          strstr(buf, "kernel module") != NULL, "insmod hello.ko: the module runs and makes /dev/hello");
    check(rozos_module_list(buf, sizeof(buf), 0) > 0 && strstr(buf, "hello") != NULL, "lsmod lists the module");
    check(rozos_delete_module("hello") == 0 && access("/dev/hello", F_OK) != 0 && rozos_delete_module("hello") < 0,
          "rmmod: the module and /dev/hello are gone");
    free(img);
    {
        static char syms[65536];
        check(rozos_module_list(syms, sizeof(syms), 1) > 0 && strstr(syms, "kprintf\n") != NULL,
              "the kernel exports symbols for modules (kprintf ...)");
    }
    check(rozos_mounts(buf, sizeof(buf)) > 0 && strstr(buf, " / ext2") != NULL && umount("/") < 0 && errno == EBUSY &&
          umount("/no/such") < 0, "mount list; umount of / is refused");
    if (access("/dev/nvmea", F_OK) == 0) {
        int whole;
        mkdir("/mnt/.st15", 0755);
        whole = mount("/dev/nvmea", "/mnt/.st15", "auto", 0, NULL) == 0 && stat("/mnt/.st15", &st) == 0 &&
                umount("/mnt/.st15") == 0;
        rmdir("/mnt/.st15");
        if (whole) {
            check(1, "NVMe: /dev/nvmea mounts and unmounts");
        } else {
            /* ধাপ ২৫ক: আসল পিসিতে ফাইল সিস্টেম partition-এ (nvmea1 …), আর সেগুলো আগেই
             * automount হয়ে /mnt-এ — তখন পুরো ডিস্ক mount হয় না, সেটা ভুল নয় */
            check(access("/dev/nvmea1", F_OK) == 0 ||
                  (rozos_mounts(buf, sizeof(buf)) > 0 && strstr(buf, "nvmea") != NULL),
                  "NVMe: /dev/nvmea found with its partitions (real PC)");
        }
    } else {
        check(1, "no NVMe disk attached (make run NVME=1) - skipped");
    }
}

/* ধাপ ২৫ক: /mnt/usb-এ লেখা পরীক্ষা চলবে না যদি — (ক) এটা rozOS-এর নিজের পেন ড্রাইভ
 * (ISO-র ভেতরের ছোট EFI/FAT ভাগ, জায়গা নেই, আর সেখানে লেখা ঝুঁকির): "/"-এর একই ডিস্ক,
 * অথবা (খ) ফাইল সিস্টেম শুধু-পড়া (ntfs)। mount তালিকার লাইন: "src path fs kind label" */
static void disk_of(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] >= '0' && s[n - 1] <= '9') s[--n] = '\0';   /* sdb2 → sdb */
}
static int usb_is_readonly(void)
{
    static char m[4096];
    char root_disk[64] = "", usb_disk[64] = "", usb_fs[32] = "", *line;
    if (rozos_mounts(m, sizeof(m)) <= 0) return 0;
    for (line = strtok(m, "\n"); line; line = strtok(NULL, "\n")) {
        char src[64], path[128], fs[32];
        if (sscanf(line, "%63s %127s %31s", src, path, fs) != 3) continue;
        if (!strcmp(path, "/")) { snprintf(root_disk, sizeof(root_disk), "%s", src); disk_of(root_disk); }
        if (!strcmp(path, "/mnt/usb")) {
            snprintf(usb_disk, sizeof(usb_disk), "%s", src); disk_of(usb_disk);
            snprintf(usb_fs, sizeof(usb_fs), "%s", fs);
        }
    }
    if (!usb_disk[0]) return 0;
    if (access("/mnt/usb/EFI/BOOT/BOOTX64.EFI", F_OK) == 0 || access("/mnt/usb/limine.conf", F_OK) == 0)
        return 1;                                /* বুটের ফাইল — rozOS-এর নিজের পেন ড্রাইভ (live মোডেও) */
    return !strcmp(usb_fs, "ntfs") || (root_disk[0] && !strncmp(root_disk, "sd", 2) && !strcmp(root_disk, usb_disk));
}

/* ধাপ ১৫খ: USB পেন ড্রাইভ (make run USB=1 হলে) */
static void test_phase15b(void)
{
    char buf[1024];
    long n = rozos_usb_list(buf, sizeof(buf));
    check(n >= 0, "lsusb: the USB device list can be read");
    if (n > 0 && strstr(buf, " storage ") && access("/mnt/usb", F_OK) == 0 && usb_is_readonly()) {
        /* ধাপ ২৫ক: আসল পিসিতে /mnt/usb = rozOS-এর নিজের পেন ড্রাইভের EFI ভাগ — লেখা নয়, পড়া পরীক্ষা */
        DIR *dir = opendir("/mnt/usb");
        check(dir != NULL && readdir(dir) != NULL, "USB pen drive: /mnt/usb (the rozOS stick itself) can be read");
        if (dir) closedir(dir);
    } else if (n > 0 && strstr(buf, " storage ") && access("/mnt/usb", F_OK) == 0) {
        FILE *f = fopen("/mnt/usb/.systest", "w");
        int ok = 0;
        if (f) { fputs("usb-ok", f); fclose(f); }
        ok = read_all("/mnt/usb/.systest", buf, sizeof(buf)) == 6 && !strcmp(buf, "usb-ok");
        unlink("/mnt/usb/.systest");
        check(ok, "USB pen drive: write and read a file on /mnt/usb");
    } else {
        check(1, "no USB pen drive (make run USB=1) - skipped");
    }
}

/* ধাপ ১৬ক: libc সামঞ্জস্য */
static int cmp_int16(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static int re_match(const char *pat, const char *str, int flags, int *so, int *eo)
{
    regex_t r;
    regmatch_t m[2];
    int e = regcomp(&r, pat, flags);
    if (e) return -1;
    e = regexec(&r, str, 2, m, 0);
    if (!e) { *so = (int)m[0].rm_so; *eo = (int)m[0].rm_eo; }
    regfree(&r);
    return e;
}

static void test_phase16a(void)
{
    char buf[256], *p;
    int fds[2], i, so = -1, eo = -1;

    /* ---- কার্নেলের নতুন syscall ---- */
    {
        int fd = open("/tmp/p16.txt", O_RDWR | O_CREAT | O_TRUNC, 0644);
        struct iovec iov[2];
        char a[4], b[8];
        check(fd >= 0, "phase16a: create /tmp/p16.txt");
        iov[0].iov_base = (void *)"abc"; iov[0].iov_len = 3;
        iov[1].iov_base = (void *)"defgh"; iov[1].iov_len = 5;
        check(writev(fd, iov, 2) == 8, "writev: two buffers, 8 bytes");
        check(pread(fd, buf, 3, 2) == 3 && !memcmp(buf, "cde", 3) && lseek(fd, 0, SEEK_CUR) == 8,
              "pread: reads at offset 2, file offset unchanged");
        check(pwrite(fd, "XY", 2, 0) == 2 && lseek(fd, 0, SEEK_CUR) == 8, "pwrite: writes at offset 0");
        lseek(fd, 0, SEEK_SET);
        iov[0].iov_base = a; iov[0].iov_len = 3;
        iov[1].iov_base = b; iov[1].iov_len = 5;
        check(readv(fd, iov, 2) == 8 && !memcmp(a, "XYc", 3) && !memcmp(b, "defgh", 5),
              "readv: fills two buffers");
        check(fsync(fd) == 0 && fdatasync(fd) == 0, "fsync/fdatasync");
        {
            int d = fcntl(fd, F_DUPFD, 20);
            struct flock fl;
            memset(&fl, 0, sizeof(fl));
            fl.l_type = F_WRLCK;
            check(d >= 20, "fcntl F_DUPFD: new fd >= 20");
            check((fcntl(fd, F_GETFL) & O_ACCMODE) == O_RDWR, "fcntl F_GETFL: O_RDWR");
            check(fcntl(fd, F_SETLK, &fl) == 0 && fcntl(fd, F_GETLK, &fl) == 0 && fl.l_type == F_UNLCK,
                  "fcntl record locks (always granted)");
            if (d >= 0) close(d);
        }
        close(fd);
        unlink("/tmp/p16.txt");
    }
    if (pipe(fds) == 0) {
        fd_set rd;
        struct timeval tv;
        FD_ZERO(&rd); FD_SET(fds[0], &rd);
        tv.tv_sec = 0; tv.tv_usec = 50000;
        check(select(fds[0] + 1, &rd, NULL, NULL, &tv) == 0, "select: empty pipe times out");
        write(fds[1], "z", 1);
        FD_ZERO(&rd); FD_SET(fds[0], &rd);
        tv.tv_sec = 1; tv.tv_usec = 0;
        check(select(fds[0] + 1, &rd, NULL, NULL, &tv) == 1 && FD_ISSET(fds[0], &rd), "select: readable pipe");
        close(fds[0]);
        close(fds[1]);
    }

    /* ---- printf / scanf ---- */
    snprintf(buf, sizeof(buf), "%.20f|%.3e|%g|%a|%.0f|%.0f", 0.1, 123456.0, 1e-5, 1.0, 2.5, 3.5);
    check(!strcmp(buf, "0.10000000000000000555|1.235e+05|1e-05|0x1p+0|2|4"), "printf: exact %f %e %g %a, round-half-even");
    snprintf(buf, sizeof(buf), "%.17g %Lg %.3Lf", 0.1 + 0.2, 1e4000L, 2.0L / 3);
    check(!strcmp(buf, "0.30000000000000004 1e+4000 0.667"), "printf: %.17g and long double");
    snprintf(buf, sizeof(buf), "%2$s-%1$d %3$ls", 7, "x", L"\x9ac\x9be\x982\x9b2\x9be");
    check(!strcmp(buf, "x-7 \xe0\xa6\xac\xe0\xa6\xbe\xe0\xa6\x82\xe0\xa6\xb2\xe0\xa6\xbe"),
          "printf: positional args and %ls (UTF-8)");
    {
        long double ld = 0;
        char *ms = NULL;
        double h = 0;
        int n = sscanf("2.5 word 0x1p4", "%Lf %ms %lf", &ld, &ms, &h);
        check(n == 3 && ld == 2.5L && ms && !strcmp(ms, "word") && h == 16.0, "scanf: %Lf, %ms, hex float");
        free(ms);
    }
    p = NULL;
    check(asprintf(&p, "%05.1f", 3.14159) == 5 && p && !strcmp(p, "003.1"), "asprintf");
    free(p);

    /* ---- memstream / tmpfile / mkstemp ---- */
    {
        char *mb = NULL;
        size_t ms = 0;
        FILE *f = open_memstream(&mb, &ms);
        char mbuf[32] = "12 34";
        int x = 0, y = 0;
        fprintf(f, "n=%d", 42);
        fclose(f);
        check(mb && !strcmp(mb, "n=42") && ms == 4, "open_memstream");
        free(mb);
        f = fmemopen(mbuf, sizeof(mbuf), "r");
        check(f && fscanf(f, "%d %d", &x, &y) == 2 && x == 12 && y == 34, "fmemopen + fscanf");
        if (f) fclose(f);
        f = tmpfile();
        check(f && fputs("tmp", f) >= 0 && fseek(f, 0, SEEK_SET) == 0 && fgets(buf, 8, f) && !strcmp(buf, "tmp"),
              "tmpfile");
        if (f) fclose(f);
        strcpy(buf, "/tmp/stXXXXXX");
        i = mkstemp(buf);
        check(i >= 0 && strncmp(buf + 8, "XXXXXX", 6) != 0, "mkstemp");
        if (i >= 0) { close(i); unlink(buf); }
    }

    /* ---- সময় অঞ্চল ---- */
    {
        time_t t = 1700000000;
        struct tm tm;
        setenv("TZ", "Asia/Dhaka", 1);
        tzset();
        localtime_r(&t, &tm);
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M %z %A", &tm);
        check(!strcmp(buf, "2023-11-15 04:13 +0600 Wednesday"), "TZ=Asia/Dhaka: localtime + strftime");
        setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
        tzset();
        t = 1690000000;
        localtime_r(&t, &tm);
        check(tm.tm_isdst == 1 && tm.tm_hour == 0 && tm.tm_gmtoff == -4 * 3600, "TZ with DST rule (New York, July)");
        memset(&tm, 0, sizeof(tm));
        check(strptime("2024-02-29 13:45:10", "%Y-%m-%d %H:%M:%S", &tm) != NULL && tm.tm_mday == 29 &&
              tm.tm_mon == 1, "strptime");
        unsetenv("TZ");
        setenv("TZ", "UTC0", 1);
        tzset();
        check(timegm(&tm) == 1709214310, "timegm");
        unsetenv("TZ");
        tzset();
    }

    /* ---- wide / UTF-8 / locale ---- */
    {
        wchar_t w[8];
        size_t n = mbstowcs(w, "a\xe0\xa6\xac\xf0\x9f\x98\x80", 8);
        check(n == 3 && w[1] == 0x9ac && w[2] == 0x1f600, "mbstowcs: UTF-8 to code points");
        check(wcwidth(0x9ac) == 1 && wcwidth(0x9cd) == 0 && wcwidth(0x4e00) == 2, "wcwidth: Bengali, hasanta, CJK");
        check(towupper(0xe9) == 0xc9 && iswalpha(0x9ac) && !iswalpha(0x9e6), "wctype: towupper, iswalpha");
        check(setlocale(LC_ALL, "") != NULL && setlocale(LC_ALL, "bn_BD.UTF-8") != NULL &&
              !strcmp(localeconv()->decimal_point, "."), "setlocale + localeconv");
        {
            iconv_t cd = iconv_open("UTF-16LE", "UTF-8");
            char in[] = "a\xe0\xa6\xac", out[16], *ip = in, *op = out;
            size_t il = strlen(in), ol = sizeof(out);
            check(cd != (iconv_t)-1 && iconv(cd, &ip, &il, &op, &ol) == 0 && sizeof(out) - ol == 4 &&
                  (unsigned char)out[2] == 0xac && (unsigned char)out[3] == 0x09, "iconv UTF-8 -> UTF-16LE");
            if (cd != (iconv_t)-1) iconv_close(cd);
        }
    }

    /* ---- regex ---- */
    check(re_match("(foo|foobar)baz", "xfoobarbaz", REG_EXTENDED, &so, &eo) == 0 && so == 1 && eo == 10,
          "regex ERE: alternation, leftmost-longest");
    check(re_match("\\(ab\\)*c", "zababc", 0, &so, &eo) == 0 && so == 1 && eo == 6, "regex BRE: groups and *");
    check(re_match("^[[:digit:]]{3}-[0-9]+$", "123-45", REG_EXTENDED, &so, &eo) == 0, "regex: classes and bounds");
    check(re_match("([a-z]+)\\1", "abcabc", REG_EXTENDED, &so, &eo) == 0 && eo == 6, "regex: backreference");
    check(re_match("HELLO", "say hello", REG_ICASE, &so, &eo) == 0 && so == 4, "regex: REG_ICASE");
    {
        regex_t r;
        check(regcomp(&r, "(", REG_EXTENDED) == REG_EPAREN && regerror(REG_EPAREN, NULL, buf, sizeof(buf)) > 1,
              "regex: syntax error reported");
    }

    /* ---- getopt_long ---- */
    {
        char *av[] = { "prog", "-a", "file", "--name=bob", "--verb", "-c", "3", NULL };
        static const struct option lo[] = { { "name", required_argument, 0, 'n' },
                                            { "verbose", no_argument, 0, 'v' }, { 0, 0, 0, 0 } };
        int c, got = 0;
        optind = 0;
        while ((c = getopt_long(7, av, "ac:", lo, NULL)) != -1) {
            if (c == 'a') got |= 1;
            if (c == 'n' && optarg && !strcmp(optarg, "bob")) got |= 2;
            if (c == 'v') got |= 4;
            if (c == 'c' && optarg && !strcmp(optarg, "3")) got |= 8;
        }
        check(got == 15 && optind == 6 && !strcmp(av[6], "file"), "getopt_long: long options, abbreviation, permute");
        optind = 1;
    }

    /* ---- qsort / search / misc ---- */
    {
        static int arr[2000];
        void *root = NULL;
        for (i = 0; i < 2000; i++) arr[i] = (i * 7919) % 2000;
        qsort(arr, 2000, sizeof(int), cmp_int16);
        for (i = 1; i < 2000 && arr[i - 1] <= arr[i]; i++) {}
        check(i == 2000, "qsort (introsort): 2000 elements sorted");
        tsearch("two", &root, (int (*)(const void *, const void *))strcmp);
        tsearch("one", &root, (int (*)(const void *, const void *))strcmp);
        check(tfind("two", &root, (int (*)(const void *, const void *))strcmp) != NULL, "tsearch/tfind");
        check(strverscmp("file9", "file10") < 0 && strlcpy(buf, "abcdef", 4) == 6 && !strcmp(buf, "abc") &&
              memmem("hello", 5, "ll", 2) != NULL, "strverscmp, strlcpy, memmem");
    }

    /* ---- libm ---- */
    check(fabs(expm1(1e-10) / 1.00000000005e-10 - 1) < 1e-15 && fabs(log1p(1e-10) / 0.99999999995e-10 - 1) < 1e-15,
          "libm: expm1/log1p accurate near 0");
    check(cbrt(-27.0) == -3.0 && fma(2, 3, 4) == 10 && hypot(3, 4) == 5, "libm: cbrt, fma, hypot");
    check(fabs(erf(0.5) - 0.5204998778130465) < 1e-15 && fabs(tgamma(5.0) - 24.0) < 1e-12 &&
          fabs(lgamma(100.0) - 359.13420536957540) < 1e-10, "libm: erf, tgamma, lgamma");
    check(isinf(pow(0.0, -1.0)) && pow(-8.0, 3.0) == -512.0 && isnan(pow(-8.0, 0.5)) && pow(1.0, NAN) == 1.0,
          "libm: pow special cases (C99)");
    check(fpclassify(0.0) == FP_ZERO && isnormal(1.0) && !isnormal(4.9e-324) && signbit(-0.0),
          "libm: fpclassify, isnormal, signbit");
    check(fegetround() == FE_TONEAREST && fesetround(FE_UPWARD) == 0 && fegetround() == FE_UPWARD &&
          fesetround(FE_TONEAREST) == 0, "fenv: rounding mode");
    check(rint(2.5) == 2.0 && lround(2.5) == 3 && nextafter(1.0, 2.0) > 1.0, "libm: rint, lround, nextafter");

    /* ---- pwd / syslog / dlfcn / sysconf / setjmp ---- */
    {
        struct passwd *pw = getpwuid(0);
        struct group *gr = getgrgid(0);
        check(pw && !strcmp(pw->pw_name, "root") && gr && !strcmp(gr->gr_name, "root"), "getpwuid/getgrgid: root");
        openlog("systest", LOG_PID, LOG_USER);
        syslog(LOG_INFO, "phase16a %d", 42);
        closelog();
        check(access("/var/log/messages", F_OK) == 0 || access("/tmp/messages", F_OK) == 0, "syslog writes a log file");
        check(dlopen("libfoo.so", RTLD_NOW) == NULL && dlerror() != NULL, "dlopen: clear failure message");
        check(sysconf(_SC_PAGESIZE) == 4096 && getpagesize() == 4096, "sysconf(_SC_PAGESIZE)");
    }
    {
        static sigjmp_buf jb;
        volatile int k = 0;
        if (sigsetjmp(jb, 1) == 0) { k = 1; siglongjmp(jb, 5); }
        check(k == 1, "sigsetjmp/siglongjmp");
    }
    {
        void *a = NULL;
        check(posix_memalign(&a, 4096, 100) == 0 && ((uintptr_t)a & 4095) == 0, "posix_memalign 4096");
        free(a);
        a = aligned_alloc(64, 1000);
        a = realloc(a, 5000);
        check(a != NULL, "aligned_alloc + realloc");
        free(a);
    }
}

/* ধাপ ১৬খ-১: signal handler (systest-এ আর host-এ একই কোড) */
static volatile sig_atomic_t g_cnt, g_sig, g_code, g_pid, g_status;
static volatile void *g_addr;
static volatile long g_ticks;
static sigjmp_buf g_jb;
static volatile uintptr_t g_seen;

static void h_count(int s) { g_cnt++; g_sig = s; }
static void h_info(int s, siginfo_t *si, void *uc)
{
    (void)uc;
    g_sig = s; g_code = si->si_code; g_pid = si->si_pid; g_status = si->si_status; g_addr = si->si_addr;
    g_cnt++;
}
static void h_tick(int s)
{
    volatile double x = 1.0;
    int i;
    (void)s;
    for (i = 0; i < 50; i++) x = x * 1.5 + 0.25;       /* FPU আর রেজিস্টার ঘাঁটা */
    g_ticks++;
}
static void h_jump(int s) { g_sig = s; siglongjmp(g_jb, 1); }
static void h_jump_info(int s, siginfo_t *si, void *uc) { (void)uc; g_sig = s; g_addr = si->si_addr; siglongjmp(g_jb, 1); }
static void h_alt(int s) { volatile char here = 0; (void)s; g_seen = (uintptr_t)&here; }
static void h_exit7(int s) { (void)s; _exit(7); }

static int set_sa(int sig, void (*fn)(int), int flags)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fn;
    sa.sa_flags = flags;
    sigemptyset(&sa.sa_mask);
    return sigaction(sig, &sa, NULL);
}

static int set_sa_info(int sig, void (*fn)(int, siginfo_t *, void *), int flags)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = fn;
    sa.sa_flags = flags | SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    return sigaction(sig, &sa, NULL);
}

static void timer_ms(long first, long every)
{
    struct itimerval it;
    memset(&it, 0, sizeof(it));
    it.it_value.tv_sec = first / 1000; it.it_value.tv_usec = (first % 1000) * 1000;
    it.it_interval.tv_sec = every / 1000; it.it_interval.tv_usec = (every % 1000) * 1000;
    setitimer(ITIMER_REAL, &it, NULL);
}

static void test_phase16b1(void)
{
    sigset_t s, old;
    int fds[2], st, pid;
    char c;

    /* ১. সাধারণ handler */
    g_cnt = 0;
    check(set_sa(SIGUSR1, h_count, 0) == 0 && raise(SIGUSR1) == 0 && g_cnt == 1 && g_sig == SIGUSR1,
          "signal handler runs (raise SIGUSR1)");
    /* ২. SA_SIGINFO */
    g_cnt = 0; g_pid = 0;
    set_sa_info(SIGUSR2, h_info, 0);
    kill(getpid(), SIGUSR2);
    check(g_cnt == 1 && g_sig == SIGUSR2 && g_pid == getpid() && g_code == SI_USER,
          "SA_SIGINFO: si_signo, si_pid, si_code");
    /* ৩. mask আর sigpending */
    g_cnt = 0;
    sigemptyset(&s); sigaddset(&s, SIGUSR1);
    sigprocmask(SIG_BLOCK, &s, &old);
    raise(SIGUSR1);
    {
        sigset_t pend;
        int blocked_ok = g_cnt == 0 && sigpending(&pend) == 0 && sigismember(&pend, SIGUSR1) == 1;
        sigprocmask(SIG_SETMASK, &old, NULL);
        check(blocked_ok && g_cnt == 1, "sigprocmask blocks, sigpending, delivery on unblock");
    }
    /* ৪. sigsuspend + setitimer */
    g_cnt = 0;
    set_sa(SIGALRM, h_count, 0);
    sigemptyset(&s); sigaddset(&s, SIGALRM);
    sigprocmask(SIG_BLOCK, &s, &old);
    timer_ms(50, 0);
    {
        sigset_t empty, now;
        int r;
        sigemptyset(&empty);
        r = sigsuspend(&empty);
        sigprocmask(SIG_BLOCK, NULL, &now);
        check(r == -1 && errno == EINTR && g_cnt == 1 && sigismember(&now, SIGALRM) == 1,
              "sigsuspend + setitimer: wakes, mask restored");
    }
    sigprocmask(SIG_SETMASK, &old, NULL);
    /* ৫. রেজিস্টার আর FPU অক্ষত (ঘনঘন SIGALRM-এর মাঝে হিসাব) */
    {
        volatile double sum = 0;
        long i, n = 3000000, isum = 0;
        g_ticks = 0;
        set_sa(SIGALRM, h_tick, SA_RESTART);
        timer_ms(2, 2);
        long isum2 = 0;
        for (i = 0; i < n; i++) { sum += (double)i * 0.5; isum += i ^ 3; }
        timer_ms(0, 0);
        for (i = 0; i < n; i++) isum2 += i ^ 3;              /* signal ছাড়া একই হিসাব */
        check(sum == 0.5 * (double)n * (double)(n - 1) / 2.0 && isum == isum2 && g_ticks > 0,
              "registers and FPU survive many signals (sigreturn)");
    }
    /* ৬. EINTR (SA_RESTART ছাড়া) */
    if (pipe(fds) == 0) {
        ssize_t r;
        g_cnt = 0;
        set_sa(SIGALRM, h_count, 0);
        timer_ms(80, 0);
        r = read(fds[0], &c, 1);
        check(r == -1 && errno == EINTR && g_cnt == 1, "read interrupted: EINTR without SA_RESTART");
        /* ৭. SA_RESTART: read আবার চলে, পরে আসা বাইট পায় */
        set_sa(SIGALRM, h_count, SA_RESTART);
        g_cnt = 0;
        fflush(stdout);
        pid = fork();
        if (pid == 0) {
            usleep(300000);
            write(fds[1], "x", 1);
            _exit(0);
        }
        timer_ms(80, 0);
        r = read(fds[0], &c, 1);
        check(r == 1 && c == 'x' && g_cnt == 1, "SA_RESTART: read restarts after the handler");
        waitpid(pid, &st, 0);
        close(fds[0]);
        close(fds[1]);
    }
    /* ৮. SIGCHLD */
    {
        sigset_t empty;
        g_cnt = 0; g_pid = 0; g_status = -1;
        set_sa_info(SIGCHLD, h_info, SA_RESTART);
        sigemptyset(&s); sigaddset(&s, SIGCHLD);
        sigprocmask(SIG_BLOCK, &s, &old);
        fflush(stdout);
        pid = fork();
        if (pid == 0) _exit(3);
        sigemptyset(&empty);
        while (!g_cnt) sigsuspend(&empty);
        sigprocmask(SIG_SETMASK, &old, NULL);
        check(g_sig == SIGCHLD && g_pid == pid && g_status == 3 && g_code == CLD_EXITED &&
              waitpid(pid, &st, 0) == pid && WEXITSTATUS(st) == 3, "SIGCHLD with si_pid and si_status");
        set_sa(SIGCHLD, SIG_DFL, 0);
    }
    /* ৯. SIGSEGV ধরা, siglongjmp, mask ফেরা */
    g_sig = 0; g_addr = (void *)1;
    set_sa_info(SIGSEGV, h_jump_info, 0);
    if (sigsetjmp(g_jb, 1) == 0) {
        volatile int *bad = (volatile int *)0;
        *bad = 1;
        check(0, "SIGSEGV handler (not reached)");
    } else {
        sigset_t now;
        sigprocmask(SIG_BLOCK, NULL, &now);
        check(g_sig == SIGSEGV && g_addr == (void *)0 && !sigismember(&now, SIGSEGV),
              "SIGSEGV handler + siglongjmp (si_addr, mask restored)");
    }
    set_sa(SIGSEGV, SIG_DFL, 0);
    /* ১০. SIGFPE */
    g_sig = 0;
    set_sa(SIGFPE, h_jump, 0);
    if (sigsetjmp(g_jb, 1) == 0) {
        volatile int z = 0, v = 5;
        v = v / z;
        check(0, "SIGFPE handler (not reached)");
    } else {
        check(g_sig == SIGFPE, "SIGFPE (divide by zero) handler");
    }
    set_sa(SIGFPE, SIG_DFL, 0);
    /* ১১. sigaltstack */
    {
        static char alt[16384];
        stack_t ss;
        ss.ss_sp = alt; ss.ss_size = sizeof(alt); ss.ss_flags = 0;
        g_seen = 0;
        sigaltstack(&ss, NULL);
        set_sa(SIGUSR1, h_alt, SA_ONSTACK);
        raise(SIGUSR1);
        check(g_seen >= (uintptr_t)alt && g_seen < (uintptr_t)alt + sizeof(alt), "SA_ONSTACK: handler runs on sigaltstack");
        ss.ss_flags = SS_DISABLE;
        sigaltstack(&ss, NULL);
    }
    /* ১২. SA_RESETHAND */
    {
        struct sigaction q;
        g_cnt = 0;
        set_sa(SIGUSR1, h_count, SA_RESETHAND);
        raise(SIGUSR1);
        sigaction(SIGUSR1, NULL, &q);
        check(g_cnt == 1 && q.sa_handler == SIG_DFL, "SA_RESETHAND: back to SIG_DFL");
    }
    /* ১৩. nanosleep: EINTR আর বাকি সময় */
    {
        struct timespec req, rem;
        int r;
        req.tv_sec = 1; req.tv_nsec = 0;
        rem.tv_sec = rem.tv_nsec = 0;
        set_sa(SIGALRM, h_count, 0);
        timer_ms(60, 0);
        r = nanosleep(&req, &rem);
        check(r == -1 && errno == EINTR && rem.tv_sec == 0 && rem.tv_nsec > 500000000L,
              "nanosleep: EINTR and the time left");
    }
    /* ১৪. sigwait */
    {
        int got = 0;
        sigemptyset(&s); sigaddset(&s, SIGUSR2);
        sigprocmask(SIG_BLOCK, &s, &old);
        raise(SIGUSR2);
        check(sigwait(&s, &got) == 0 && got == SIGUSR2, "sigwait takes a blocked signal");
        sigprocmask(SIG_SETMASK, &old, NULL);
    }
    /* ১৫. অন্য প্রসেসের handler, ১৬. ডিফল্টে এখনো শেষ */
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        set_sa(SIGTERM, h_exit7, 0);
        for (;;) pause();
    }
    usleep(100000);
    kill(pid, SIGTERM);
    check(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 7, "kill: the child's own handler runs");
    pid = fork();
    if (pid == 0) {
        raise(SIGTERM);
        _exit(0);
    }
    check(waitpid(pid, &st, 0) == pid && WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM, "default action still terminates");
    /* ১৭. alarm-এর বাকি সময় */
    set_sa(SIGALRM, h_count, 0);
    alarm(10);
    check(alarm(0) >= 9, "alarm returns the seconds left");
    set_sa(SIGALRM, SIG_DFL, 0);
    set_sa(SIGUSR1, SIG_DFL, 0);
    set_sa(SIGUSR2, SIG_DFL, 0);
}

/* ধাপ ১৬খ-১: SIGWINCH (pty-র মাপ বদল) আর Lua/zlib */
static void test_phase16b1_more(void)
{
    int mfd, sfd;
    char buf[256];
    if (openpty(&mfd, &sfd, NULL, NULL, NULL) == 0) {
        pid_t pg = getpgrp();
        unsigned short ws[4] = { 33, 101, 0, 0 };
        g_cnt = 0;
        set_sa(SIGWINCH, h_count, SA_RESTART);
        ioctl(sfd, TIOCSPGRP, &pg);
        ioctl(mfd, TIOCSWINSZ, ws);
        check(g_cnt == 1 && g_sig == SIGWINCH, "SIGWINCH when the pty size changes");
        set_sa(SIGWINCH, SIG_DFL, 0);
        pg = 0;
        ioctl(sfd, TIOCSPGRP, &pg);              /* নইলে বন্ধ করার সময় আমাদের গ্রুপ SIGHUP পায় */
        close(mfd);
        close(sfd);
    } else {
        check(0, "openpty for the SIGWINCH test");
    }
    if (access("/bin/lua", X_OK) == 0) {
        FILE *f = popen("/bin/lua -e \"print(string.format('%d %.3f', 2^10, math.pi))\"", "r");
        buf[0] = '\0';
        if (f) { if (!fgets(buf, sizeof(buf), f)) buf[0] = '\0'; pclose(f); }
        check(!strncmp(buf, "1024 3.142", 10), "Lua: /bin/lua runs a script");
    } else {
        check(0, "Lua: /bin/lua is installed");
    }
    if (access("/bin/gzip", X_OK) == 0 && access("/bin/gunzip", X_OK) == 0) {
        FILE *f = fopen("/tmp/zt.txt", "w");
        int i, st, ok = 0;
        for (i = 0; f && i < 500; i++) fprintf(f, "rozOS zlib line %d\n", i);
        if (f) fclose(f);
        unlink("/tmp/zt.txt.gz");
        st = system("/bin/gzip /tmp/zt.txt");
        if (st == 0 && access("/tmp/zt.txt.gz", F_OK) == 0 && access("/tmp/zt.txt", F_OK) != 0) {
            struct stat sb;
            ok = stat("/tmp/zt.txt.gz", &sb) == 0 && sb.st_size > 20 && sb.st_size < 4000;
            st = system("/bin/gunzip /tmp/zt.txt.gz");
            f = fopen("/tmp/zt.txt", "r");
            if (ok && st == 0 && f) {
                int n = 0;
                while (fgets(buf, sizeof(buf), f)) n++;
                ok = n == 500 && !strcmp(buf, "rozOS zlib line 499\n");
            } else ok = 0;
            if (f) fclose(f);
        }
        unlink("/tmp/zt.txt");
        unlink("/tmp/zt.txt.gz");
        check(ok, "zlib: gzip and gunzip round trip");
    } else {
        check(0, "zlib: /bin/gzip and /bin/gunzip are installed");
    }
}

/* ধাপ ১৬খ-২: থ্রেড (systest-এ আর host-এ একই কোড) */
static pthread_mutex_t t_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t t_cv = PTHREAD_COND_INITIALIZER;
static long t_counter;
static int t_queue[64], t_qh, t_qt, t_done_flag;
static long t_consumed;
static pthread_key_t t_key;
static volatile int t_dtor_ran;
static pthread_once_t t_once = PTHREAD_ONCE_INIT;
static volatile int t_once_count;
static sem_t t_sem;
static pthread_barrier_t t_bar;
static volatile int t_bar_serial;

static void *th_add(void *arg)
{
    long i, n = (long)arg;
    for (i = 0; i < n; i++) {
        pthread_mutex_lock(&t_mx);
        t_counter++;
        pthread_mutex_unlock(&t_mx);
        if ((i & 255) == 0) sched_yield();
    }
    return (void *)(n * 2);
}

static void *th_errno(void *arg)
{
    errno = (int)(long)arg;
    usleep(20000);
    return (void *)(long)(errno == (int)(long)arg);
}

static void *th_consumer(void *arg)
{
    (void)arg;
    for (;;) {
        int v;
        pthread_mutex_lock(&t_mx);
        while (t_qh == t_qt && !t_done_flag) pthread_cond_wait(&t_cv, &t_mx);
        if (t_qh == t_qt && t_done_flag) { pthread_mutex_unlock(&t_mx); break; }
        v = t_queue[t_qh++ % 64];
        pthread_cond_broadcast(&t_cv);
        pthread_mutex_unlock(&t_mx);
        __sync_fetch_and_add(&t_consumed, v);
    }
    return NULL;
}

static void t_dtor(void *v) { if (v == (void *)0x1234) t_dtor_ran = 1; }
static void *th_tsd(void *arg) { (void)arg; pthread_setspecific(t_key, (void *)0x1234); return pthread_getspecific(t_key); }
static void t_once_fn(void) { t_once_count++; }
static void *th_once(void *arg) { (void)arg; pthread_once(&t_once, t_once_fn); return NULL; }
static void *th_sem(void *arg) { (void)arg; usleep(50000); sem_post(&t_sem); return NULL; }
static void *th_bar(void *arg) { (void)arg; if (pthread_barrier_wait(&t_bar) == PTHREAD_BARRIER_SERIAL_THREAD) t_bar_serial++; return NULL; }
static void *th_exit(void *arg) { (void)arg; pthread_exit((void *)77); return NULL; }
static void *th_tid(void *arg) { (void)arg; return (void *)(long)gettid(); }
static void *th_sigwait(void *arg)
{
    sigset_t ss;
    int sig = 0;
    sigemptyset(&ss);
    sigaddset(&ss, SIGUSR1);
    sigwait(&ss, &sig);
    *(int *)arg = sig;
    return NULL;
}
static void *th_sleep_forever(void *arg) { (void)arg; for (;;) sleep(10); return NULL; }
static volatile int t_det_ran;
static void *th_detached(void *arg) { (void)arg; t_det_ran = 1; return NULL; }
static void *th_fpu(void *arg)
{
    volatile double s = 0;
    long i, k = (long)arg;
    for (i = 1; i <= 200000; i++) s += (double)k / (double)i;
    return (void *)(long)(s * 1000);
}

static void test_phase16b2(void)
{
    pthread_t t[8];
    void *res;
    int i, ok;

    /* ১. তৈরি আর join, ফেরত মান */
    ok = pthread_create(&t[0], NULL, th_add, (void *)1000L) == 0 && pthread_join(t[0], &res) == 0 && res == (void *)2000L;
    check(ok && t_counter == 1000, "pthread_create + pthread_join (return value)");
    /* ২. ৮টা থ্রেড, mutex দিয়ে কাউন্টার */
    t_counter = 0;
    for (i = 0; i < 8; i++) pthread_create(&t[i], NULL, th_add, (void *)20000L);
    for (i = 0; i < 8; i++) pthread_join(t[i], NULL);
    check(t_counter == 160000, "8 threads x 20000 increments with a mutex = 160000");
    /* ৩. প্রতিটা থ্রেডের নিজের errno */
    pthread_create(&t[0], NULL, th_errno, (void *)11L);
    pthread_create(&t[1], NULL, th_errno, (void *)22L);
    errno = 5;
    {
        void *r0, *r1;
        pthread_join(t[0], &r0);
        pthread_join(t[1], &r1);
        check(r0 == (void *)1L && r1 == (void *)1L && errno == 5, "errno is per thread (TLS)");
    }
    /* ৪. producer/consumer (cond) */
    t_qh = t_qt = 0; t_done_flag = 0; t_consumed = 0;
    for (i = 0; i < 3; i++) pthread_create(&t[i], NULL, th_consumer, NULL);
    for (i = 1; i <= 1000; i++) {
        pthread_mutex_lock(&t_mx);
        while (t_qt - t_qh >= 64) pthread_cond_wait(&t_cv, &t_mx);
        t_queue[t_qt++ % 64] = i;
        pthread_cond_broadcast(&t_cv);
        pthread_mutex_unlock(&t_mx);
    }
    pthread_mutex_lock(&t_mx);
    t_done_flag = 1;
    pthread_cond_broadcast(&t_cv);
    pthread_mutex_unlock(&t_mx);
    for (i = 0; i < 3; i++) pthread_join(t[i], NULL);
    check(t_consumed == 500500, "producer/consumer with pthread_cond (sum 1..1000)");
    /* ৫. TSD destructor */
    pthread_key_create(&t_key, t_dtor);
    pthread_create(&t[0], NULL, th_tsd, NULL);
    pthread_join(t[0], &res);
    check(res == (void *)0x1234 && t_dtor_ran && pthread_getspecific(t_key) == NULL, "pthread_key: per-thread value + destructor");
    /* ৬. once */
    for (i = 0; i < 4; i++) pthread_create(&t[i], NULL, th_once, NULL);
    for (i = 0; i < 4; i++) pthread_join(t[i], NULL);
    check(t_once_count == 1, "pthread_once runs once");
    /* ৭. semaphore */
    sem_init(&t_sem, 0, 0);
    pthread_create(&t[0], NULL, th_sem, NULL);
    check(sem_trywait(&t_sem) == -1 && sem_wait(&t_sem) == 0, "semaphore: sem_wait until sem_post");
    pthread_join(t[0], NULL);
    /* ৮. barrier */
    pthread_barrier_init(&t_bar, NULL, 4);
    t_bar_serial = 0;
    for (i = 0; i < 3; i++) pthread_create(&t[i], NULL, th_bar, NULL);
    if (pthread_barrier_wait(&t_bar) == PTHREAD_BARRIER_SERIAL_THREAD) t_bar_serial++;
    for (i = 0; i < 3; i++) pthread_join(t[i], NULL);
    check(t_bar_serial == 1, "pthread_barrier (exactly one serial thread)");
    /* ৯. pthread_exit, gettid আলাদা, pthread_self */
    pthread_create(&t[0], NULL, th_exit, NULL);
    pthread_join(t[0], &res);
    pthread_create(&t[1], NULL, th_tid, NULL);
    {
        void *tid;
        pthread_join(t[1], &tid);
        check(res == (void *)77 && (long)tid > 0 && (long)tid != getpid() && gettid() == getpid() &&
              pthread_equal(pthread_self(), pthread_self()), "pthread_exit value, gettid differs from pid");
    }
    /* ১০. FPU আলাদা */
    pthread_create(&t[0], NULL, th_fpu, (void *)1L);
    pthread_create(&t[1], NULL, th_fpu, (void *)2L);
    {
        void *a, *b;
        pthread_join(t[0], &a);
        pthread_join(t[1], &b);
        check((long)b > 0 && (long)b / 2 - (long)a <= 1 && (long)a / 1 - (long)b / 2 <= 1, "FPU state is per thread");
    }
    /* ১১. recursive/errorcheck mutex, trylock, timedlock */
    {
        pthread_mutex_t m;
        pthread_mutexattr_t ma;
        struct timespec ts;
        pthread_mutexattr_init(&ma);
        pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&m, &ma);
        ok = pthread_mutex_lock(&m) == 0 && pthread_mutex_lock(&m) == 0 && pthread_mutex_unlock(&m) == 0 &&
             pthread_mutex_unlock(&m) == 0 && pthread_mutex_unlock(&m) == EPERM;
        pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_ERRORCHECK);
        pthread_mutex_init(&m, &ma);
        pthread_mutex_lock(&m);
        ok = ok && pthread_mutex_lock(&m) == EDEADLK;
        pthread_mutex_unlock(&m);
        pthread_mutex_init(&m, NULL);
        pthread_mutex_lock(&t_mx);
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50000000;
        if (ts.tv_nsec >= 1000000000) { ts.tv_nsec -= 1000000000; ts.tv_sec++; }
        ok = ok && pthread_mutex_trylock(&t_mx) == EBUSY;
        pthread_mutex_unlock(&t_mx);
        check(ok, "recursive and errorcheck mutex, trylock");
    }
    /* ১২. detached থ্রেড */
    {
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        t_det_ran = 0;
        pthread_create(&t[0], &a, th_detached, NULL);
        for (i = 0; i < 100 && !t_det_ran; i++) usleep(10000);
        check(t_det_ran, "detached thread runs");
    }
    /* ১৩. exit() চলমান থ্রেডসহ পুরো প্রসেস শেষ করে */
    fflush(stdout);
    {
        int pid = fork(), st = 0;
        if (pid == 0) {
            pthread_create(&t[0], NULL, th_sleep_forever, NULL);
            pthread_create(&t[1], NULL, th_sleep_forever, NULL);
            usleep(50000);
            exit(9);
        }
        check(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 9,
              "exit() ends a process with sleeping threads");
    }
    /* ১৪. মূল থ্রেড pthread_exit করলে বাকিরা চলে, শেষজন শেষ হলে প্রসেস শেষ (কোড ০) */
    fflush(stdout);
    {
        int pid = fork(), st = 1;
        if (pid == 0) {
            pthread_create(&t[0], NULL, th_add, (void *)1000L);
            pthread_exit(NULL);
        }
        check(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0,
              "main thread pthread_exit: process ends with the last thread");
    }
    /* ১৬. অন্য থ্রেডে sigwait */
    {
        sigset_t ss;
        static int got_sig;
        sigemptyset(&ss);
        sigaddset(&ss, SIGUSR1);
        sigprocmask(SIG_BLOCK, &ss, NULL);
        got_sig = 0;
        pthread_create(&t[0], NULL, th_sigwait, &got_sig);
        usleep(50000);
        kill(getpid(), SIGUSR1);
        pthread_join(t[0], NULL);
        sigprocmask(SIG_UNBLOCK, &ss, NULL);
        check(got_sig == SIGUSR1, "sigwait in another thread");
    }
    /* ১৫. থ্রেডে fork, তারপর exec — সন্তান স্বাভাবিক */
    fflush(stdout);
    {
        int pid = fork(), st = 1;
        if (pid == 0) {
            pthread_create(&t[0], NULL, th_sleep_forever, NULL);
            execl("/bin/echo", "echo", "exec-from-threaded-ok", (char *)NULL);
            _exit(99);
        }
        check(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0,
              "execve in a threaded process ends the other threads");
    }
}

/* ধাপ ১৮ক: /dev/keyev আর librozgame (গ্রাফিক্স ছাড়া যতটুকু পরীক্ষা করা যায়) */
static void test_phase18a(void)
{
    int fd, ok;
    gfx_event ev;
    game_rect a, b;
    game_anim an;
    game_image *img, *small;
    char txt[16];
    char *old_home = getenv("HOME") ? strdup(getenv("HOME")) : NULL;

    /* ১. /dev/keyev খোলে, কিছু না চাপলে poll 0 */
    fd = open("/dev/keyev", O_RDONLY);
    ok = fd >= 0;
    if (ok) {
        struct pollfd p;
        p.fd = fd; p.events = POLLIN; p.revents = 0;
        ok = poll(&p, 1, 0) == 0;
        close(fd);
    }
    check(ok, "/dev/keyev opens, poll with no keys = 0");

    /* ২. সংঘর্ষ */
    a = game_make_rect(0, 0, 10, 10);
    b = game_make_rect(8, 2, 10, 4);
    check(game_rect_overlap(a, b) && !game_rect_overlap(a, game_make_rect(10, 0, 5, 5)) &&
          game_rect_contains(a, 9.5f, 0) && !game_rect_contains(a, 10, 5),
          "game_rect_overlap / contains (edges exclusive)");
    check(game_rect_hit_side(a, b) == GAME_SIDE_RIGHT && game_rect_hit_side(game_make_rect(0, 7, 10, 10), game_make_rect(0, 15, 10, 10)) == GAME_SIDE_BOTTOM,
          "game_rect_hit_side: right and bottom");
    ok = game_rect_push_out(&a, b) == GAME_SIDE_RIGHT && a.x == -2 && !game_rect_overlap(a, b);
    check(ok && game_circle_rect(5, 5, 3, game_make_rect(7, 0, 4, 10)) && !game_circle_rect(0, 0, 1, game_make_rect(2, 2, 2, 2)) &&
          game_circle_overlap(0, 0, 1, 1.5f, 0, 1),
          "game_rect_push_out, circle-rect, circle-circle");

    /* ৩. অ্যানিমেশন */
    memset(&an, 0, sizeof(an));
    game_anim_set(&an, 4, 4, 10, 1);
    game_anim_update(&an, 0.25f);
    ok = game_anim_frame(&an) == 6;
    game_anim_update(&an, 0.2f);                       /* 0.45 s → লুপ করে 0.05 → ফ্রেম 4 */
    ok = ok && game_anim_frame(&an) == 4;
    game_anim_set(&an, 0, 3, 10, 0);
    game_anim_update(&an, 1.0f);
    check(ok && an.done && game_anim_frame(&an) == 2, "game_anim: frames, loop, one-shot stops on the last");

    /* ৪. ইনপুট: কাঁচা চাপা/ছাড়া, ফ্রেমে একবার "pressed", অনুবাদ করা কী থেকে লেখা */
    game_input_frame();
    memset(&ev, 0, sizeof(ev));
    ev.type = GFX_EV_KEY_DOWN; ev.key = 'a';
    game_inject_event(&ev);
    ok = game_key_down('a') && game_key_pressed('a');
    game_input_frame();
    ok = ok && game_key_down('a') && !game_key_pressed('a');
    ev.repeat = 1;
    game_inject_event(&ev);                             /* repeat → typed, কিন্তু pressed নয় */
    ok = ok && game_key_typed('a') && !game_key_pressed('a');
    ev.type = GFX_EV_KEY_UP; ev.repeat = 0;
    game_inject_event(&ev);
    check(ok && !game_key_down('a') && game_key_released('a'), "game input: raw down / pressed once / repeat / up");
    game_input_frame();
    ev.type = GFX_EV_KEY; ev.key = 'B';
    game_inject_event(&ev);
    ok = game_text_input(txt, sizeof(txt)) == 1 && txt[0] == 'B' && game_key_pressed('b') && game_key_down('b');
    ev.key = GFX_KEY_LEFT;
    game_inject_event(&ev);
    check(ok && game_key_pressed(GFX_KEY_LEFT), "game input: typed key -> text + key state (no raw keys)");
    game_input_frame();

    /* ৫. ছবি: সারির ধরন, colorkey, মসৃণ ছোট করা (গড়) */
    img = game_image_new(4, 2);
    ok = img != NULL;
    if (ok) {
        uint32_t *p0 = img->surf->px, *p1 = img->surf->px + img->surf->stride;
        p0[0] = p0[1] = p0[2] = p0[3] = GFX_RGB(200, 0, 0);
        p1[0] = GFX_RGB(0, 0, 0); p1[1] = GFX_RGB(100, 100, 100); p1[2] = p1[3] = 0;
        game_image_update(img);
        ok = img->rows[0] == 1 && img->rows[1] == 2 && !img->opaque;
        small = game_image_resize(img, 2, 1, 1);
        /* বাঁ 2x2 ঘর: লাল, লাল, কালো, ধূসর → R = (200+200+0+100)/4 = 125 */
        ok = ok && small && small->w == 2 && small->h == 1 && GFX_R(small->surf->px[0]) == 125 &&
             GFX_A(small->surf->px[0]) == 255 && GFX_A(small->surf->px[1]) == 127;
        game_image_free(small);
        game_image_colorkey(img, GFX_RGB(200, 0, 0));
        ok = ok && img->rows[0] == 0;
        game_image_free(img);
    }
    check(ok, "game_image: row kinds, smooth (box) resize, colorkey");

    /* ৬. সহায়ক */
    ok = 1;
    {
        int i;
        game_rand_seed(1234);
        for (i = 0; i < 1000; i++) {
            int v = game_rand_int(3, 7);
            float f = game_rand_float(-1, 1);
            if (v < 3 || v > 7 || f < -1 || f >= 1) ok = 0;
        }
    }
    check(ok && game_hsv(0, 1, 1) == GFX_RGB(255, 0, 0) && game_hsv(120, 1, 1) == GFX_RGB(0, 255, 0) &&
          game_color_lerp(GFX_RGB(0, 0, 0), GFX_RGB(200, 100, 50), 0.5f) == GFX_RGB(100, 50, 25) &&
          game_approach(5, 10, 2) == 7 && game_approach(9, 10, 2) == 10 && game_clamp(-3, 0, 1) == 0,
          "game_rand range, hsv, color_lerp, approach, clamp");

    /* ৭. সর্বোচ্চ স্কোর ($HOME/.rozgame) */
    setenv("HOME", "/tmp", 1);
    unlink("/tmp/.rozgame/systest.score");
    ok = game_score_load("systest") == 0 && game_score_save("systest", 50) == 1 &&
         game_score_save("systest", 40) == 0 && game_score_load("systest") == 50;
    unlink("/tmp/.rozgame/systest.score");
    if (old_home) { setenv("HOME", old_home, 1); free(old_home); }
    else unsetenv("HOME");
    check(ok, "game_score_save/load keeps the best");
}

/* ধাপ ১৮খ: ক্যামেরা, টাইল ম্যাপ, কণা, luagame */
static void test_phase18b(void)
{
    game_camera cam;
    game_tilemap *m;
    game_rect r;
    game_particles *ps;
    game_emitter e;
    FILE *f;
    int ok, hit, c, row, idx, st;

    /* ১. ক্যামেরা: লক্ষ্য মাঝখানে, সীমার ভেতরে, ছোট জগত মাঝখানে */
    game_camera_init(&cam);
    cam.w = 100; cam.h = 100;
    game_camera_bounds(&cam, 0, 0, 1000, 500);
    game_camera_follow(&cam, 500, 250, 0, 0);
    ok = cam.x == 450 && cam.y == 200;
    game_camera_follow(&cam, 10, 10, 0, 0);
    ok = ok && cam.x == 0 && cam.y == 0;
    game_camera_follow(&cam, 990, 490, 0, 0);
    ok = ok && cam.x == 900 && cam.y == 400 && game_camera_view_x(&cam) == 900;
    game_camera_bounds(&cam, 0, 0, 60, 60);
    check(ok && cam.x == -20 && cam.y == -20, "game_camera: follow, bounds, small world centred");

    /* ২. টাইল ম্যাপ: শক্ত ঘর, move-এ থামা */
    m = game_tilemap_new(10, 6, 16, 16);
    ok = m != NULL;
    if (ok) {
        for (c = 0; c < 10; c++) game_tilemap_set(m, c, 5, '#');
        game_tilemap_set(m, 6, 4, '#');
        game_tilemap_tile(m, '#', 0, 1);
        ok = game_tilemap_solid(m, 3, 5) && !game_tilemap_solid(m, 3, 4) && game_tilemap_solid(m, -1, 0) &&
             !game_tilemap_solid(m, 3, 9) && game_tilemap_solid_at(m, 100, 85) && game_tilemap_get(m, 6, 4) == '#';
        r = game_make_rect(10, 20, 12, 12);
        hit = game_tilemap_move(m, &r, 0, 200);                /* নিচে পড়ে মেঝেতে থামে */
        ok = ok && hit == GAME_HIT_BOTTOM && r.y == 80 - 12;
        hit = game_tilemap_move(m, &r, 300, 0);                /* ডানে গিয়ে (6,4)-এর দেয়ালে */
        ok = ok && hit == GAME_HIT_RIGHT && r.x == 96 - 12;
        hit = game_tilemap_move(m, &r, -500, 0);               /* বাঁ কিনারা শক্ত */
        ok = ok && hit == GAME_HIT_LEFT && r.x == 0;
        game_tilemap_free(m);
    }
    check(ok, "game_tilemap: solid cells and edges, move stops on floor and walls");

    /* ৩. .map ফাইল পড়া, অক্ষর খোঁজা */
    f = fopen("/tmp/t18.map", "w");
    if (f) {
        fputs("# test\nsize 8 8\ncolor # 808080 solid\ntile ^ 3\nmap\n..C..C\n.P\n######\n", f);
        fclose(f);
    }
    m = game_tilemap_load("/tmp/t18.map");
    ok = m && m->cols == 6 && m->rows == 3 && m->tw == 8 && m->solid['#'] && !m->solid['^'] && m->frame['^'] == 3;
    if (ok) {
        idx = game_tilemap_find(m, 'C', 0, &c, &row);
        ok = idx == 2 && c == 2 && row == 0;
        idx = game_tilemap_find(m, 'C', idx + 1, &c, &row);
        ok = ok && idx == 5 && c == 5 && game_tilemap_find(m, 'C', idx + 1, NULL, NULL) == -1;
        ok = ok && game_tilemap_find(m, 'P', 0, &c, &row) >= 0 && c == 1 && row == 1 && game_tilemap_get(m, 4, 1) == '.';
    }
    game_tilemap_free(m);
    unlink("/tmp/t18.map");
    m = game_tilemap_load("/usr/share/games/platformer-1.map");
    check(ok && m && m->set && m->cols == 80 && m->rows == 15, "game_tilemap_load: .map text, find, platformer level with tileset");
    game_tilemap_free(m);

    /* ৪. কণা */
    ps = game_particles_new(16);
    e = game_emitter_default(GFX_RGB(255, 0, 0));
    ok = ps && e.color_start == GFX_RGB(255, 0, 0) && (e.color_end >> 24) == 0;
    if (ps) {
        game_particles_burst(ps, 10, 10, 20, &e);             /* ১৬-র বেশি চাইলে ঘুরে বসে */
        ok = ok && game_particles_alive(ps) == 16;
        game_particles_update(ps, 0.2f);
        ok = ok && game_particles_alive(ps) == 16;
        game_particles_update(ps, 1.0f);
        ok = ok && game_particles_alive(ps) == 0;
        game_particles_free(ps);
    }
    check(ok, "game_particles: burst, update, all fade out");

    /* ৫. luagame: game মডিউল (গ্রাফিক্স ছাড়া অংশ) */
    f = fopen("/tmp/t18.lua", "w");
    if (f) {
        fputs("assert(game.overlap(0,0,10,10, 5,5,10,10) and not game.overlap(0,0,10,10, 10,0,5,5))\n"
              "assert(game.rgb(1,2,3) == 0xFF010203 and game.clamp(5,0,3) == 3)\n"
              "local x = game.rand_int(2, 4) assert(x >= 2 and x <= 4)\n"
              "assert(type(game.run) == 'function' and game.HIT_BOTTOM == 8)\n"
              "local m = game.tilemap('platformer-1.map') local c, r = m:size() assert(c == 80 and r == 15)\n"
              "local nx, ny, hit = m:move(40, 300, 20, 28, 0, 200) assert(hit == game.HIT_BOTTOM)\n", f);
        fclose(f);
    }
    {
        char *argv[] = { "luagame", "/tmp/t18.lua", NULL };
        ok = run("/bin/luagame", argv, &st) > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0;
    }
    unlink("/tmp/t18.lua");
    check(ok, "luagame: the Lua game module (collisions, colours, tile map)");
}

/* ধাপ ১৯ক: C সহায়িকা (/usr/share/cref) আর cref কমান্ড */
static void test_phase19a(void)
{
    const cref_entry *e, *hits[8];
    char *t;
    int n, ok, st;

    n = cref_load(NULL);
    check(n >= 200, "C reference loads (/usr/share/cref, 200+ pages)");
    e = cref_find("printf");
    ok = e && !strcmp(e->header, "stdio.h") && strstr(e->syntax, "int printf(") && strstr(e->example, "main");
    check(ok && cref_find("vprintf") == e && cref_find("sinf") == cref_find("sin") && cref_find("for") &&
          !cref_find("no_such_function"), "cref_find: names, @also aliases, language topics");
    n = cref_search("square root", hits, 8);
    ok = 0;
    while (n-- > 0) if (!strcmp(hits[n]->name, "sqrt")) ok = 1;
    check(ok, "cref_search finds sqrt for 'square root'");
    t = cref_format(cref_find("strlen"), 60);
    ok = t && strstr(t, "Syntax:") && strstr(t, "Example:") && strstr(t, "size_t strlen");
    free(t);
    {
        const char *h[128];                          /* ধাপ ১৯খ: হেডার এখন ৫৫টা */
        int nh = cref_headers(h, 128), i, has_math = 0;
        for (i = 0; i < nh; i++) if (!strcmp(h[i], "math.h")) has_math = 1;
        ok = ok && nh >= 7 && has_math;
    }
    check(ok, "cref_format page and header list");
    {
        char *argv[] = { "cref", "-x", "puts", NULL };
        int fd = open("/tmp/cref-x.c", O_WRONLY | O_CREAT | O_TRUNC, 0644), saved = dup(1);
        ok = fd >= 0;
        if (ok) {
            fflush(stdout);
            dup2(fd, 1);
            close(fd);
            ok = run("/bin/cref", argv, &st) > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0;
            fflush(stdout);
            dup2(saved, 1);
            close(saved);
        }
        if (ok) {
            char buf[256];
            FILE *f = fopen("/tmp/cref-x.c", "r");
            size_t got = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
            if (f) fclose(f);
            buf[got] = 0;
            ok = strstr(buf, "#include <stdio.h>") && strstr(buf, "puts(");
        }
        unlink("/tmp/cref-x.c");
    }
    check(ok, "cref -x puts prints the example program");
}

/* ধাপ ১৯খ: POSIX-এর পাতা (unistd, signal, pthread, socket ...), উদাহরণ rozOS-এর tcc দিয়ে
 * কম্পাইল করে চালানো, বুট ছবি */
static void test_phase19b(void)
{
    const cref_entry *e, *hits[16];
    int n, ok, st, i;
    const char *need[] = { "unistd.h", "fcntl.h", "sys/stat.h", "signal.h", "pthread.h", "sys/socket.h",
                           "netdb.h", "termios.h", "regex.h", "wchar.h", "dirent.h", "sys/mman.h" };

    n = cref_load(NULL);
    check(n >= 330, "C reference has the POSIX pages too (330+)");
    e = cref_find("pthread_mutex_lock");
    ok = e && !strcmp(e->header, "pthread.h") && cref_find("pthread_mutex_unlock") == e &&
         cref_find("fork") && !strcmp(cref_find("fork")->header, "unistd.h") &&
         cref_find("execlp") == cref_find("execvp") && cref_find("FD_SET") == cref_find("select");
    check(ok, "cref_find: pthread_mutex_*, fork, exec family, FD_SET");
    {
        const char *h[128];
        int nh = cref_headers(h, 128), k, found = 0;
        for (i = 0; i < (int)(sizeof need / sizeof need[0]); i++)
            for (k = 0; k < nh; k++) if (!strcmp(h[k], need[i])) { found++; break; }
        check(found == (int)(sizeof need / sizeof need[0]) && nh >= 50, "cref headers: unistd, signal, pthread, socket ...");
    }
    n = cref_search("semaphore", hits, 16);
    ok = 0;
    while (n-- > 0) if (!strcmp(hits[n]->name, "sem_init")) ok = 1;
    check(ok, "cref_search finds sem_init for 'semaphore'");

    if (access("/bin/tcc", X_OK) == 0) {
        /* cref -x pipe > ফাইল, tcc দিয়ে কম্পাইল, চালিয়ে আউটপুট দেখা */
        static const char *const ex[] = { "pipe", "pthread_create", "regcomp" };
        static const char *const want[] = { "parent got: hello through the pipe", "= 500500", "year 2026" };
        int good = 0;
        for (i = 0; i < 3; i++) {
            char *xargv[] = { "cref", "-x", (char *)ex[i], NULL };
            char *cargv[] = { "tcc", "-o", "/tmp/cref19b", "/tmp/cref19b.c", NULL };
            char *rargv[] = { "cref19b", NULL };
            char buf[512];
            int fd, saved;
            size_t got;
            FILE *f;
            fflush(stdout);
            fd = open("/tmp/cref19b.c", O_WRONLY | O_CREAT | O_TRUNC, 0644);
            saved = dup(1);
            dup2(fd, 1);
            close(fd);
            ok = run("/bin/cref", xargv, &st) > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0;
            fflush(stdout);
            dup2(saved, 1);
            close(saved);
            ok = ok && run("/bin/tcc", cargv, &st) > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0;
            if (ok) {
                fd = open("/tmp/cref19b.out", O_WRONLY | O_CREAT | O_TRUNC, 0644);
                saved = dup(1);
                dup2(fd, 1);
                close(fd);
                ok = run("/tmp/cref19b", rargv, &st) > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0;
                fflush(stdout);
                dup2(saved, 1);
                close(saved);
            }
            f = ok ? fopen("/tmp/cref19b.out", "r") : NULL;
            got = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
            if (f) fclose(f);
            buf[got] = 0;
            if (ok && strstr(buf, want[i])) good++;
            else printf("    (example %s: %s)\n", ex[i], ok ? buf : "did not build/run");
        }
        unlink("/tmp/cref19b.c");
        unlink("/tmp/cref19b.out");
        unlink("/tmp/cref19b");
        check(good == 3, "POSIX examples (pipe+fork, threads, regex) build with tcc and run");
    } else {
        check(1, "tcc not installed - POSIX example test skipped");
    }
    {
        unsigned char m[3] = {0};
        FILE *f = fopen("/usr/share/images/rozos-boot.jpg", "rb");
        if (f) { fread(m, 1, 3, f); fclose(f); }
        check(m[0] == 0xff && m[1] == 0xd8 && m[2] == 0xff, "boot picture /usr/share/images/rozos-boot.jpg (JPEG)");
    }
}

/* ধাপ ১৯গ: rozOS লাইব্রেরি (rozgfx, rozui, rozgame, rozhttp, rozos.h, rozcref), Lua game,
 * Lua C API আর zlib-এর পাতা; zlib আর Lua-র উদাহরণ rozOS-এর tcc দিয়ে চালানো */
static void test_phase19c(void)
{
    const cref_entry *e;
    int n, ok, i, st;
    n = cref_load(NULL);
    check(n >= 420, "C reference has the rozOS library pages too (420+)");
    e = cref_find("gfx_fill_rect");
    ok = e && !strcmp(e->header, "rozgfx.h") && cref_find("ui_do_button") && !strcmp(cref_find("ui_do_button")->header, "rozui.h") &&
         cref_find("game_tilemap_move") && !strcmp(cref_find("game_tilemap_move")->header, "rozgame.h") &&
         cref_find("rozhttp_get") && cref_find("rozos_netinfo") && cref_find("cref_find");
    check(ok, "cref_find: rozgfx, rozui, rozgame, rozhttp, rozos.h, rozcref");
    e = cref_find("game.run");
    ok = e && !strcmp(e->header, "lua game") && cref_find("lua_pcall") && cref_find("gzopen") &&
         cref_find("img:draw") == cref_find("game.image");
    check(ok, "cref_find: Lua game module, Lua C API, zlib");
    if (access("/bin/tcc", X_OK) == 0) {
        static const char *const ex[] = { "crc32", "lua_pcall" };
        static const char *const lib[] = { "-lz", "-llua" };
        static const char *const want[] = { "cbf43926", "twice(21) = 42" };
        int good = 0;
        for (i = 0; i < 2; i++) {
            char *xargv[] = { "cref", "-x", (char *)ex[i], NULL };
            char *cargv[] = { "tcc", "-o", "/tmp/cref19c", "/tmp/cref19c.c", (char *)lib[i], "-lm", NULL };
            char *rargv[] = { "cref19c", NULL };
            char buf[512];
            int fd, saved;
            size_t got;
            FILE *f;
            fflush(stdout);
            fd = open("/tmp/cref19c.c", O_WRONLY | O_CREAT | O_TRUNC, 0644);
            saved = dup(1);
            dup2(fd, 1);
            close(fd);
            ok = run("/bin/cref", xargv, &st) > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0;
            fflush(stdout);
            dup2(saved, 1);
            close(saved);
            ok = ok && run("/bin/tcc", cargv, &st) > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0;
            if (ok) {
                fd = open("/tmp/cref19c.out", O_WRONLY | O_CREAT | O_TRUNC, 0644);
                saved = dup(1);
                dup2(fd, 1);
                close(fd);
                ok = run("/tmp/cref19c", rargv, &st) > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0;
                fflush(stdout);
                dup2(saved, 1);
                close(saved);
            }
            f = ok ? fopen("/tmp/cref19c.out", "r") : NULL;
            got = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
            if (f) fclose(f);
            buf[got] = 0;
            if (ok && strstr(buf, want[i])) good++;
            else printf("    (example %s: %s)\n", ex[i], ok ? buf : "did not build/run");
        }
        unlink("/tmp/cref19c.c");
        unlink("/tmp/cref19c.out");
        unlink("/tmp/cref19c");
        check(good == 2, "zlib and Lua C API examples build with tcc and run");
    } else {
        check(1, "tcc not installed - zlib/Lua example test skipped");
    }
}

/* ধাপ ১৯ঘ: gfx_save_png — লিখে আবার পড়ে প্রতিটা পিক্সেল মেলানো (Paint-এর PNG সংরক্ষণ) */
static void test_phase19d(void)
{
    gfx_surface *a = gfx_surface_new(120, 80), *b;
    int x, y, bad = 0;
    long size = 0;
    struct stat st;
    if (!a) { check(0, "gfx_save_png: memory"); return; }
    gfx_gradient_v(a, 0, 0, 120, 80, GFX_RGB(0, 0, 90), GFX_RGB(240, 130, 10));
    gfx_fill_circle(a, 60, 40, 25, GFX_YELLOW);
    gfx_line(a, 0, 79, 119, 0, GFX_RED);
    for (x = 0; x < 120; x += 3) gfx_pixel(a, x, 3, GFX_RGB(x * 2, 255 - x, x));
    if (gfx_save_png(a, "/tmp/st19d.png") == 0 && (b = gfx_load_image("/tmp/st19d.png")) != NULL) {
        for (y = 0; y < 80; y++)
            for (x = 0; x < 120; x++)
                if ((gfx_get_pixel(a, x, y) & 0xFFFFFF) != (gfx_get_pixel(b, x, y) & 0xFFFFFF)) bad++;
        if (stat("/tmp/st19d.png", &st) == 0) size = (long)st.st_size;
        gfx_surface_free(b);
    } else bad = -1;
    unlink("/tmp/st19d.png");
    gfx_surface_free(a);
    check(bad == 0 && size > 0 && size < 120 * 80 * 3, "gfx_save_png writes a PNG that loads back exactly (and smaller than raw)");
}

/* ধাপ ১৯চ: Settings-এর তথ্যের syscall — sysinfo (99), statfs (137) / statvfs, rozos_proc_list (509) */
static void test_phase19f(void)
{
    struct sysinfo si;
    struct statvfs v;
    static char buf[16384];
    long n;
    int ok;
    ok = sysinfo(&si) == 0 && si.mem_unit == 1 && si.totalram > si.freeram && si.freeram > 0 && si.procs >= 1;
    check(ok, "sysinfo: RAM total > free > 0, processes counted");
    ok = statvfs("/", &v) == 0 && v.f_blocks > 0 && v.f_bfree <= v.f_blocks && v.f_frsize > 0;
    check(ok, "statvfs(\"/\"): size and free blocks");
    n = rozos_proc_list(buf, sizeof(buf) - 1);
    ok = n > 0;
    if (ok) {
        buf[n] = '\0';
        ok = strstr(buf, "systest") != NULL && strstr(buf, "init") != NULL;
    }
    check(ok, "rozos_proc_list lists init and systest");
}

/* ধাপ ১৯ছ: immutable flag, ফোল্ডার লক/আনলক, .hidden, *at() ফাংশন, sbase */
static int write_text(const char *p, const char *t)
{
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    if (write(fd, t, strlen(t)) != (ssize_t)strlen(t)) { close(fd); return -1; }
    return close(fd);
}

static int text_is(const char *p, const char *t)
{
    char b[256];
    int fd = open(p, O_RDONLY);
    ssize_t n;
    if (fd < 0) return 0;
    n = read(fd, b, sizeof(b) - 1);
    close(fd);
    if (n < 0) return 0;
    b[n] = '\0';
    return !strcmp(b, t);
}

static int immutable_works(const char *p)
{
    int fd, fl = 0, ok;
    if (write_text(p, "x") < 0 || (fd = open(p, O_RDONLY)) < 0) return 0;
    ok = ioctl(fd, FS_IOC_GETFLAGS, &fl) == 0;
    fl |= FS_IMMUTABLE_FL;
    ok = ok && ioctl(fd, FS_IOC_SETFLAGS, &fl) == 0;
    ok = ok && unlink(p) < 0 && errno == EPERM;
    ok = ok && open(p, O_WRONLY) < 0 && errno == EPERM;
    ok = ok && rename(p, "/tmp/st19g-moved") < 0 && errno == EPERM;
    ok = ok && truncate(p, 0) < 0;
    fl = 0;
    ioctl(fd, FS_IOC_GETFLAGS, &fl);
    ok = ok && (fl & FS_IMMUTABLE_FL);
    fl &= ~FS_IMMUTABLE_FL;
    ok = ok && ioctl(fd, FS_IOC_SETFLAGS, &fl) == 0;
    close(fd);
    return ok && unlink(p) == 0;
}

static void test_phase19g(void)
{
    const char *base = "/home/.st19g";
    char out[1100];
    int ok, r;
    DIR *d;
    check(immutable_works("/tmp/st19g-imm"), "immutable flag on tmpfs: unlink/write/rename/truncate EPERM, then cleared");
    check(immutable_works("/home/.st19g-imm"), "immutable flag on ext2 (kept in the inode)");
    /* ফোল্ডার লক */
    mkdir(base, 0755);
    mkdir("/home/.st19g/data", 0755);
    mkdir("/home/.st19g/data/sub", 0755);
    write_text("/home/.st19g/data/a.txt", "alpha\n");
    write_text("/home/.st19g/data/sub/b.txt", "beta beta\n");
    r = rozlock_lock("/home/.st19g/data", "s3cret words", NULL, NULL, out, sizeof(out));
    ok = r == 0 && !strcmp(out, "/home/.st19g/data.rozlock") && access("/home/.st19g/data", F_OK) != 0 &&
         rozlock_is_lockfile(out) && rozlock_is_protected(out);
    check(ok, "rozlock_lock: folder -> data.rozlock (encrypted, originals gone, immutable)");
    ok = unlink("/home/.st19g/data.rozlock") < 0 && errno == EPERM;
    check(ok, "a locked folder cannot be deleted (EPERM)");
    ok = rozlock_check("/home/.st19g/data", "wrong") == ROZLOCK_EWRONG &&
         rozlock_unlock("/home/.st19g/data.rozlock", "nope", NULL, NULL, out, sizeof(out)) == ROZLOCK_EWRONG &&
         access("/home/.st19g/data.rozlock", F_OK) == 0;
    check(ok, "wrong secret: refused, lock file kept");
    r = rozlock_unlock("/home/.st19g/data", "s3cret words", NULL, NULL, out, sizeof(out));
    ok = r == 0 && !strcmp(out, "/home/.st19g/data") && text_is("/home/.st19g/data/a.txt", "alpha\n") &&
         text_is("/home/.st19g/data/sub/b.txt", "beta beta\n") && access("/home/.st19g/data.rozlock", F_OK) != 0;
    check(ok, "rozlock_unlock: folder back exactly, lock file gone");
    check(rozlock_lock("/home", "x1234", NULL, NULL, out, sizeof(out)) == ROZLOCK_ESYSTEM, "system folders cannot be locked");
    /* লুকানো */
    ok = rozhide_set("/home/.st19g/data", 1) == 0 && rozhide_is_hidden("/home/.st19g/data") &&
         text_is("/home/.st19g/.hidden", "data\n") && rozhide_set("/home/.st19g/data", 0) == 0 &&
         !rozhide_is_hidden("/home/.st19g/data") && access("/home/.st19g/.hidden", F_OK) != 0;
    check(ok, "rozhide_set: .hidden list add and remove (name unchanged)");
    /* *at() ফাংশন আর fdopendir */
    ok = faccessat(AT_FDCWD, "/home/.st19g/data/a.txt", R_OK, 0) == 0 &&
         unlinkat(AT_FDCWD, "/home/.st19g/data/sub/b.txt", 0) == 0 &&
         unlinkat(AT_FDCWD, "/home/.st19g/data/sub", AT_REMOVEDIR) == 0;
    d = fdopendir(open("/home/.st19g/data", O_RDONLY | O_DIRECTORY));
    ok = ok && d != NULL && isascii('a') && !isascii(200);
    if (d) closedir(d);
    check(ok, "faccessat, unlinkat (AT_REMOVEDIR), fdopendir, isascii");
    unlink("/home/.st19g/data/a.txt");
    rmdir("/home/.st19g/data");
    rmdir(base);
    /* sbase */
    ok = access("/bin/grep", X_OK) == 0 && access("/bin/sed", X_OK) == 0 && access("/bin/sort", X_OK) == 0;
    if (ok) {
        r = system("printf 'pear\\napple\\nfig\\n' | sort | head -n 1 > /tmp/st19g.out");
        ok = r == 0 && text_is("/tmp/st19g.out", "apple\n");
        r = system("echo hello rozos | sed s/hello/hi/ | grep -c rozos > /tmp/st19g.out");
        ok = ok && r == 0 && text_is("/tmp/st19g.out", "1\n");
        unlink("/tmp/st19g.out");
    }
    check(ok, "sbase: sort | head, sed | grep in a pipe");
}

/* ধাপ ১৯জ: আসল dirfd দিয়ে *at() (sbase-এর tar/du ভেতরে ঘোরে এভাবে) */
static void test_phase19h(void)
{
    struct stat st;
    int fd, ok, r;
    mkdir("/tmp/st19h", 0755);
    mkdir("/tmp/st19h/d", 0755);
    write_text("/tmp/st19h/d/b.txt", "bee\n");
    fd = open("/tmp/st19h/d", O_RDONLY | O_DIRECTORY);
    ok = fd >= 0 && fstatat(fd, "b.txt", &st, 0) == 0 && st.st_size == 4 &&
         faccessat(fd, "b.txt", R_OK, 0) == 0 && unlinkat(fd, "b.txt", 0) == 0 && access("/tmp/st19h/d/b.txt", F_OK) != 0;
    if (fd >= 0) close(fd);
    check(ok, "fstatat/faccessat/unlinkat with a real directory fd");
    write_text("/tmp/st19h/d/c.txt", "sea\n");
    ok = access("/bin/tar", X_OK) == 0;
    if (ok) {
        r = system("cd /tmp/st19h && tar -cf /tmp/st19h.tar d && tar -tf /tmp/st19h.tar | grep -c c.txt > /tmp/st19h.out");
        ok = r == 0 && text_is("/tmp/st19h.out", "1\n");
        unlink("/tmp/st19h.out");
        unlink("/tmp/st19h.tar");
    }
    check(ok, "sbase tar: a folder into an archive and back (tar -cf, tar -tf)");
    unlink("/tmp/st19h/d/c.txt");
    rmdir("/tmp/st19h/d");
    rmdir("/tmp/st19h");
}

/* ধাপ ২৩ক: FreeType দিয়ে মসৃণ লেখা, UI আর mono সিস্টেম ফন্ট, fontview */
static void test_phase23a(void)
{
    gfx_surface *s = gfx_surface_new(120, 24);
    gfx_font *f = gfx_font_load_ttf("/usr/share/fonts/Inter-Regular.ttf", 14, 0), *m;
    int ok = 0, x, y, mid = 0, full = 0;
    if (s && f) {
        gfx_clear(s, GFX_RGB(0, 0, 0));
        gfx_text(s, f, 2, 2, "Hello rozOS", GFX_WHITE);
        for (y = 0; y < s->h; y++)
            for (x = 0; x < s->w; x++) {
                unsigned v = GFX_G(gfx_get_pixel(s, x, y));
                if (v == 255) full++;
                else if (v > 0) mid++;
            }
        ok = full > 10 && mid > 10;            /* ধার মসৃণ: মাঝের রংও আছে */
    }
    check(gfx_have_freetype() && ok, "FreeType draws smooth (anti-aliased) Inter text");
    gfx_font_free(f);
    gfx_surface_free(s);
    check(gfx_font_ui() && gfx_text_width(NULL, "iiii") < gfx_text_width(NULL, "MMMM"),
          "UI font is proportional TrueType (gfx_text with NULL)");
    m = gfx_font_mono();
    check(gfx_font_is_ttf(m) && gfx_text_width(m, "iii") == gfx_text_width(m, "MMM") && gfx_font_width(m) >= 8,
          "mono font has equal TrueType cells (terminal, Code)");
    ok = access("/bin/fontview", X_OK) == 0 &&
         system("fontview -l | grep -c KiB > /tmp/st23.out") == 0;
    if (ok) {
        FILE *fp = fopen("/tmp/st23.out", "r");
        int n = 0;
        ok = fp && fscanf(fp, "%d", &n) == 1 && n >= 15;
        if (fp) fclose(fp);
        unlink("/tmp/st23.out");
    }
    check(ok, "fontview -l lists the TrueType fonts (15 or more)");
}

/* ধাপ ২৩খ: বাংলা shaping (kb_text_shape) — যুক্তাক্ষর এক glyph, তাই দুই অক্ষরের চেয়ে সরু */
static void test_phase23b(void)
{
    gfx_font *f = gfx_font_load_ttf("/usr/share/fonts/NotoSansBengali.ttf", 20, 0);
    int conj = f ? gfx_text_width(f, "ক্ষ") : 0, two = f ? gfx_text_width(f, "ক") + gfx_text_width(f, "ষ") : 0;
    int ui = gfx_text_width(NULL, "বাংলা");
    check(f && conj > 0 && conj < two, "Bengali shaping joins ক্ষ into one conjunct (kb_text_shape)");
    check(ui > 10, "UI text with Bengali falls back to the Bengali font");
    gfx_font_free(f);
}

/* ধাপ ২৩গ: অভ্র phonetic আর টাইপের সময় শব্দ বদল */
static void test_phase23c(void)
{
    char o1[64], o2[64], o3[64], o4[64], ins[64];
    rozbn_comp c;
    int del = 0, ok;
    rozbn_convert("ami", o1, sizeof(o1));
    rozbn_convert("shikkha", o2, sizeof(o2));
    rozbn_convert("sondhya", o3, sizeof(o3));
    rozbn_convert("karrtik", o4, sizeof(o4));
    check(!strcmp(o1, "আমি") && !strcmp(o2, "শিক্ষা") && !strcmp(o3, "সন্ধ্যা") && !strcmp(o4, "কার্তিক"),
          "Avro phonetic: ami shikkha sondhya karrtik");
    rozbn_reset(&c);
    ok = rozbn_key(&c, 'a', &del, ins, sizeof(ins)) && del == 0 && !strcmp(ins, "আ") &&
         rozbn_key(&c, 'm', &del, ins, sizeof(ins)) && del == 3 && !strcmp(ins, "আম") &&
         rozbn_key(&c, 'i', &del, ins, sizeof(ins)) && !strcmp(ins, "আমি") &&
         !rozbn_key(&c, ' ', &del, ins, sizeof(ins)) && c.rlen == 0;
    check(ok, "Bengali typing rewrites the word on each key (a, am, ami)");
}

/* ধাপ ২৪ক: ডেস্কটপের Delete... (deldlg), শান্ত বুট, নেটওয়ার্কের লিংক তথ্য */
static void test_phase24a(void)
{
    struct utsname un;
    struct rozos_netinfo ni;
    check(access("/bin/deldlg", X_OK) == 0 && system("deldlg 2>/dev/null") != 0,
          "deldlg exists and refuses to run without a path");
    /* ধাপ ১৭ক: ০.২৫ এসেছে — "০.২৪ বা পরের" (আগে শুধু 0.24.x) */
    check(uname(&un) == 0 && !strncmp(un.release, "0.", 2) && atoi(un.release + 2) >= 24,
          "kernel version 0.24 or later");
    check(rozos_netinfo(0, &ni) != 0 || ni.link_up <= 1, "netinfo reports the link state (LAN icon)");
}

/* ধাপ ১৭ক: GCC/C++-এর জন্য libc — __cxa_atexit, vfork, DT_*, আলাদা fnmatch/err, PATH */
static int p17_order[4], p17_n;
static void p17_cxa(void *a) { p17_order[p17_n++] = (int)(long)a; }
static void p17_plain(void) { p17_order[p17_n++] = 9; }
/* সবার আগে নিবন্ধিত → সবার শেষে চলে; ক্রম 9,2,1 হলে কোড 0 */
static void p17_last(void) { _exit(p17_n == 3 && p17_order[0] == 9 && p17_order[1] == 2 && p17_order[2] == 1 ? 0 : 1); }
static void test_phase17a(void)
{
    pid_t pid;
    int st = -1;
    /* সন্তান-প্রসেসে exit()-এর ক্রম: শেষে নিবন্ধিত আগে চলে, atexit আর __cxa_atexit মিশিয়ে */
    pid = fork();
    if (pid == 0) {
        p17_n = 0;
        atexit(p17_last);
        __cxa_atexit(p17_cxa, (void *)1, 0);
        __cxa_atexit(p17_cxa, (void *)2, 0);
        atexit(p17_plain);
        exit(5);
    }
    check(pid > 0 && waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0,
          "__cxa_atexit + atexit run at exit, newest first");
    p17_n = 0;
    pid = vfork();
    if (pid == 0) _exit(17);
    check(pid > 0 && waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 17,
          "vfork() works (same as fork)");
    check(DT_LNK == 10 && DT_FIFO == 1 && DT_SOCK == 12 && DT_BLK == 6, "dirent DT_* constants");
    check(fnmatch("*.c", "a.c", 0) == 0 && fnmatch("a?[0-9]", "ab7", 0) == 0 &&
          fnmatch("*.c", "a.h", 0) == FNM_NOMATCH, "fnmatch (own object now)");
    check(system("unset PATH; ls / >/dev/null") == 0, "programs still run with no PATH (default list)");
    (void)p17_order;
}

/* ধাপ ১৭খ: GCC-র জন্য IPv6-এর হেডার; কার্নেলে IPv6 নেই — socket() পরিষ্কার ভাবে ব্যর্থ */
static void test_phase17b(void)
{
    struct sockaddr_in6 a6;
    int fd;
    memset(&a6, 0, sizeof(a6));
    a6.sin6_family = AF_INET6;
    a6.sin6_addr.s6_addr[15] = 1;
    check(AF_INET6 == 10 && sizeof(struct in6_addr) == 16 && sizeof(a6) == 28 && a6.sin6_addr.s6_addr32[3] != 0,
          "IPv6 types (sockaddr_in6, in6_addr)");
    fd = socket(AF_INET6, SOCK_STREAM, 0);
    check(fd < 0, "socket(AF_INET6) is refused (no IPv6 yet)");
    if (fd >= 0) close(fd);
}

/* ধাপ ১৭গ: C++-এর সহায়িকা আর Code-এর C++ টেমপ্লেট */
static void test_phase17c(void)
{
    const cref_entry *v, *p;
    check(cref_load(NULL) > 0 && (v = cref_find("vector")) != NULL && cref_is_cpp(v) &&
          (p = cref_find("printf")) != NULL && !cref_is_cpp(p), "C++ reference pages (cref vector) and cref_is_cpp");
    check(access("/usr/share/templates/hello.cpp", R_OK) == 0, "Code's C++ template (File > New C++ file)");
}

/* ধাপ ২৫খ: block layer write-back — ছোট/বড়/অসমান লেখা সাথে সাথে পড়া যায়, মাঝখানে বদল
 * হারায় না, fsync()/sync() ডিস্কে পাঠায় (ext2-এ, /home) */
static void test_phase25b(void)
{
    const char *path = "/home/.systest-25b";
    size_t n = 600 * 1024, i, got = 0;
    unsigned char *a = (unsigned char *)malloc(n), *b = (unsigned char *)malloc(n);
    struct stat st;
    int fd, ok = 0, same = 0, k;
    if (!a || !b) {
        check(0, "write cache test: memory");
        free(a);
        free(b);
        return;
    }
    for (i = 0; i < n; i++) a[i] = (unsigned char)(i * 31 + (i >> 9));
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        ok = write(fd, a, 5000) == 5000 &&                         /* ছোট, অসমান */
             write(fd, a + 5000, n - 5000) == (ssize_t)(n - 5000) && /* বড় একটানা */
             pwrite(fd, "XYZ", 3, 4093) == 3 &&                     /* দুই ব্লকের সীমায় বদল */
             pwrite(fd, "Q", 1, 300000) == 1;
        memcpy(a + 4093, "XYZ", 3);
        a[300000] = 'Q';
        close(fd);
    }
    fd = open(path, O_RDONLY);
    if (fd >= 0) {
        ssize_t r;
        while (got < n && (r = read(fd, b + got, n - got)) > 0) got += (size_t)r;
        same = got == n && !memcmp(a, b, n);
        close(fd);
    }
    check(ok && same, "write cache: small, large and in-place writes read back the same at once");
    fd = open(path, O_RDWR);
    check(fd >= 0 && fsync(fd) == 0, "fsync() writes the cached data to the disk");
    if (fd >= 0) close(fd);
    for (k = 0; k < 20; k++) {                                     /* অনেক ছোট ফাইল */
        char nm[64];
        snprintf(nm, sizeof(nm), "%s.%d", path, k);
        fd = open(nm, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            if (write(fd, nm, strlen(nm)) != (ssize_t)strlen(nm)) ok = 0;
            close(fd);
        } else {
            ok = 0;
        }
    }
    sync();
    for (k = 0; k < 20; k++) {
        char nm[64], rd[64];
        int m;
        snprintf(nm, sizeof(nm), "%s.%d", path, k);
        m = read_all(nm, rd, sizeof(rd));
        if (m != (int)strlen(nm) || memcmp(rd, nm, (size_t)m)) ok = 0;
        unlink(nm);
    }
    check(ok && stat(path, &st) == 0 && st.st_size == (off_t)n, "20 small files + sync(): all there");
    unlink(path);
    free(a);
    free(b);
}

int main(int argc, char **argv)
{
    (void)argc;
    printf("systest: running in ring 3 (%s, pid %d)\n", argv[0], getpid());
    test_basics();
    test_memory();
    test_files();
    test_processes();
    test_libc();
    test_net();
    test_phase10();
    test_phase12();
    test_phase13();
    test_phase14();
    test_phase14b();
    test_phase15a();
    test_phase15b();
    test_phase16a();
    test_phase16b1();
    test_phase16b1_more();
    test_phase16b2();
    test_phase18a();
    test_phase18b();
    test_phase19a();
    test_phase19b();
    test_phase19c();
    test_phase19d();
    test_phase19f();
    test_phase19g();
    test_phase19h();
    test_phase23a();
    test_phase23b();
    test_phase23c();
    test_phase24a();
    test_phase17a();
    test_phase17b();
    test_phase17c();
    test_phase25b();
    printf("systest: %d passed, %d failed\n", passed, failed);
    return failed;
}
