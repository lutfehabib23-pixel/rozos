/* rozOS — /bin/systest: ধাপ ৬-এর কার্নেল-টেস্ট (ধাপ ৬-এ এটাই /bin/init ছিল)
 *
 * ধাপ ৭ থেকে init shell চালায়, তাই টেস্টগুলো এখানে সরানো — shell থেকে `systest`।
 * syscall দুই পথে, brk, mmap, স্ট্যাক বাড়া, ফাইল, getdents64, fork/execve/wait,
 * ভুল করা প্রসেস শেষ হওয়া, preemption ও FPU অবস্থা, sleep, kill। শেষে exit (ব্যর্থ টেস্টের সংখ্যা)।
 * ধাপ ৭: rozlibc দিয়ে লেখা (আগে ছোট rozsys.h ছিল), সাথে কয়েকটা libc-টেস্ট।
 * ধাপ ১০: getrandom, /dev/urandom, sscanf-এর double, rozhttp-এর SHA-256।
 * ধাপ ১২: sscanf %[...] ও %n, fscanf। */

#include <stdio.h>
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
#include "rozhttp.h"
#include "rozui.h"
#include <sys/mount.h>
#include <sys/stat.h>

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
        mkdir("/mnt/.st15", 0755);
        check(mount("/dev/nvmea", "/mnt/.st15", "auto", 0, NULL) == 0 && stat("/mnt/.st15", &st) == 0 &&
              umount("/mnt/.st15") == 0, "NVMe: /dev/nvmea mounts and unmounts");
        rmdir("/mnt/.st15");
    } else {
        check(1, "no NVMe disk attached (make run NVME=1) - skipped");
    }
}

/* ধাপ ১৫খ: USB পেন ড্রাইভ (make run USB=1 হলে) */
static void test_phase15b(void)
{
    char buf[1024];
    long n = rozos_usb_list(buf, sizeof(buf));
    check(n >= 0, "lsusb: the USB device list can be read");
    if (n > 0 && strstr(buf, " storage ") && access("/mnt/usb", F_OK) == 0) {
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
    printf("systest: %d passed, %d failed\n", passed, failed);
    return failed;
}
