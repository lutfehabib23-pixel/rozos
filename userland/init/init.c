/* rozOS — /bin/init (pid 1, ধাপ ৭)
 *
 * কার্নেল বুট শেষে এটা চালায়। কাজ: কনসোলে shell (/bin/rozsh) চালানো, shell শেষ হলে
 * আবার চালানো, আর মাঝে যে কোনো সন্তান শেষ হলে তাকে wait করে সরানো।
 * init কখনো শেষ হয় না (হলে কার্নেল kmon-কে কনসোল দেয়)।
 * ধাপ ৬-এর টেস্টগুলো এখন /bin/systest। */

#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include <string.h>
#include <time.h>

#define SHELL "/bin/rozsh"

static pid_t start_shell(void)
{
    static char *const argv[] = { "rozsh", "-l", NULL };
    static char *const envp[] = { "PATH=/bin", "HOME=/home", "TERM=rozos", "USER=root",
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

int main(void)
{
    int fails = 0;

    if (getpid() != 1) {
        fprintf(stderr, "init: must run as pid 1 (started by the kernel)\n");
        return 1;
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
