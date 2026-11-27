/* rozlibc — pty.h: pseudo-terminal (ধাপ ১৪)
 *   int m, s;  openpty(&m, &s, NULL, NULL, NULL);
 *   fork → সন্তানে: setsid(); dup2(s, 0/1/2); exec("/bin/rozsh") — shell ভাবে সে কনসোলে
 *   মা (টার্মিনাল-অ্যাপ): read(m) = shell-এর লেখা, write(m) = চাপা কী
 * মাপ: ioctl(m, TIOCSWINSZ, &ws) (struct winsize, <sys/ioctl.h>)। name সবসময় খালি। */
#ifndef _PTY_H
#define _PTY_H
#ifdef __cplusplus
extern "C" {
#endif

#include <termios.h>
#include <sys/ioctl.h>

int openpty(int *master, int *slave, char *name, const struct termios *tp, const struct winsize *wp);

#ifdef __cplusplus
}
#endif
#endif
