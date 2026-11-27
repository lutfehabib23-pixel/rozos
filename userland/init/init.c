/* rozOS — /bin/init (pid 1, ধাপ ৭)
 *
 * কার্নেল বুট শেষে এটা চালায়। কাজ: কনসোলে shell (/bin/rozsh) চালানো, shell শেষ হলে
 * আবার চালানো, আর মাঝে যে কোনো সন্তান শেষ হলে তাকে wait করে সরানো।
 * init কখনো শেষ হয় না (হলে কার্নেল kmon-কে কনসোল দেয়)।
 * ধাপ ৬-এর টেস্টগুলো এখন /bin/systest।
 * ধাপ ১৩: /etc/console-font থাকলে (এক লাইনে ফন্টের নাম, যেমন rozos-mono-16) shell-এর
 * আগে setfont দিয়ে সেটা বসায়।
 * ধাপ ১৪: ডেস্কটপ — /etc/rozos.conf-এ mode=desktop (ডিফল্ট) হলে আর কার্নেলের কমান্ড
 * লাইনে "text" না থাকলে (init তখন argv[1] = "text" পায়) প্রথমে rozwm -t -k চালায়
 * (কিবোর্ড ডেস্কটপের, serial-এ আলাদা shell — make run-serial-এও কাজ চলে)। rozwm
 * থেকে Text mode বাছলে (বা `textmode`) বা rozwm কোনো কারণে শেষ হলে আগের মতো টেক্সট
 * কনসোলে shell। টেক্সট মোড থেকে আবার ডেস্কটপ: `desktop`। */

#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>

#define SHELL "/bin/rozsh"

static char *const shell_envp[] = { "PATH=/bin:/usr/bin:/usr/local/bin:/home/bin", "HOME=/home", "TERM=rozos",
                                    "USER=root", "SHELL=/bin/rozsh", NULL };

/* ধাপ ১৪: /etc/rozos.conf-এর mode= (না থাকলে desktop) */
static int conf_wants_desktop(void)
{
    FILE *f = fopen("/etc/rozos.conf", "r");
    char line[128];
    int desktop = 1;
    if (!f) return 1;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "mode=", 5)) desktop = strncmp(line + 5, "text", 4) != 0;
    }
    fclose(f);
    return desktop;
}

/* serial-এ shell (ডেস্কটপ চলার সময়) */
static pid_t start_serial_shell(void)
{
    static char *const argv[] = { "rozsh", "-l", NULL };
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open("/dev/serial0", O_RDWR);
        if (fd < 0) _exit(1);
        setsid();
        dup2(fd, 0);
        dup2(fd, 1);
        dup2(fd, 2);
        if (fd > 2) close(fd);
        chdir("/home");
        printf("\ninit: the desktop is on the screen (make run); this is a shell on the serial line\n");
        execve(SHELL, argv, shell_envp);
        _exit(127);
    }
    return pid;
}

/* ডেস্কটপ চালানো, শেষ পর্যন্ত অপেক্ষা; ফেরত rozwm-এর exit কোড (−1 = চালানো গেল না) */
static int run_desktop(void)
{
    struct stat st;
    pid_t wm, ser, pid;
    int status = 0;
    if (stat("/bin/rozwm", &st) < 0 || stat("/dev/fb0", &st) < 0) return -1;
    printf("init: starting the desktop (rozwm) - Start > Text mode, or `textmode`, for the text console\n");
    ser = start_serial_shell();
    wm = fork();
    if (wm == 0) {
        static char *const argv[] = { "rozwm", "-t", "-k", NULL };
        chdir("/home");
        execve("/bin/rozwm", argv, shell_envp);
        _exit(127);
    }
    if (wm < 0) return -1;
    for (;;) {
        pid = wait(&status);
        if (pid == wm) break;
        if (pid == ser) ser = start_serial_shell();    /* serial shell শেষ হলে নতুন */
        if (pid < 0) break;
    }
    if (ser > 0) {                                     /* টেক্সট মোডে serial কনসোলের shell-এ যায় */
        kill(-ser, SIGHUP);
        kill(ser, SIGKILL);
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 128 + WTERMSIG(status);
}

static pid_t start_shell(void)
{
    static char *const argv[] = { "rozsh", "-l", NULL };
    /* ধাপ ১২: PATH-এ নিজের প্রোগ্রামের জায়গাও (/usr/local/bin, ~/bin = /home/bin) */
    static char *const envp[] = { "PATH=/bin:/usr/bin:/usr/local/bin:/home/bin", "HOME=/home", "TERM=rozos", "USER=root",
                                  "SHELL=/bin/rozsh", NULL };
    pid_t pid = fork();
    if (pid == 0) {
        chdir("/home");
        execve(SHELL, argv, envp);
        fprintf(stderr, "init: cannot run %s: %s\n", SHELL, strerror(errno));
        _exit(127);
    }
    return pid;
}

/* ধাপ ১৩ */
static void apply_console_font(void)
{
    FILE *f = fopen("/etc/console-font", "r");
    char name[128];
    pid_t pid;
    int st;
    if (!f) return;
    if (!fgets(name, sizeof(name), f)) name[0] = '\0';
    fclose(f);
    name[strcspn(name, "\r\n \t")] = '\0';
    if (!name[0] || name[0] == '#') return;
    pid = fork();
    if (pid == 0) {
        char *const argv[] = { "setfont", "-q", name, NULL };
        execv("/bin/setfont", argv);
        _exit(127);
    }
    if (pid > 0) waitpid(pid, &st, 0);
}

int main(int argc, char **argv)
{
    int fails = 0;

    if (getpid() != 1) {
        fprintf(stderr, "init: must run as pid 1 (started by the kernel)\n");
        return 1;
    }
    apply_console_font();
    if (!(argc > 1 && !strcmp(argv[1], "text")) && conf_wants_desktop()) {
        int r = run_desktop();
        if (r > 0) printf("\ninit: the desktop ended (code %d) - text console\n", r);
    }
    printf("init: starting " SHELL "\n");

    for (;;) {
        int status;
        pid_t shell = start_shell(), pid;
        long t0;

        if (shell < 0) {
            fprintf(stderr, "init: fork failed: %s\n", strerror(errno));
            sleep(1);
            continue;
        }
        t0 = (long)time(NULL);
        do {
            pid = wait(&status);                 /* অন্য সন্তান এলে শুধু সরানো */
        } while (pid > 0 && pid != shell);

        if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
            fails++;
        } else {
            fails = 0;
        }
        if (fails >= 3) {
            fprintf(stderr, "init: %s keeps failing - giving up (kmon takes the console)\n", SHELL);
            return 1;
        }
        printf("\ninit: shell exited (%s %d) - starting a new one\n",
               WIFEXITED(status) ? "code" : "signal",
               WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status));
        if ((long)time(NULL) == t0) {
            sleep(1);                            /* খুব দ্রুত বারবার শেষ হলে একটু থামা */
        }
    }
}
