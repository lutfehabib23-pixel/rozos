/* rozOS — /bin/systest: ধাপ ৬-এর কার্নেল-টেস্ট (ধাপ ৬-এ এটাই /bin/init ছিল)
 *
 * ধাপ ৭ থেকে init shell চালায়, তাই টেস্টগুলো এখানে সরানো — shell থেকে `systest`।
 * syscall দুই পথে, brk, mmap, স্ট্যাক বাড়া, ফাইল, getdents64, fork/execve/wait,
 * ভুল করা প্রসেস শেষ হওয়া, preemption ও FPU অবস্থা, sleep, kill। শেষে exit (ব্যর্থ টেস্টের সংখ্যা)।
 * ধাপ ৭: rozlibc দিয়ে লেখা (আগে ছোট rozsys.h ছিল), সাথে কয়েকটা libc-টেস্ট। */

#include <stdio.h>
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
#include <rozos.h>

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

int main(int argc, char **argv)
{
    (void)argc;
    printf("systest: running in ring 3 (%s, pid %d)\n", argv[0], getpid());
    test_basics();
    test_memory();
    test_files();
    test_processes();
    test_libc();
    printf("systest: %d passed, %d failed\n", passed, failed);
    return failed;
}
