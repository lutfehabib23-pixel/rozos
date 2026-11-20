/* rozlibc — termios.h (ধাপ ১৪, ছোট রূপ)
 * rozOS-এর কনসোল আর PTY সবসময় "raw" (কার্নেল echo বা লাইন-এডিটিং করে না) — তাই এখানে
 * আসলে মানে রাখে শুধু ISIG: চালু থাকলে Ctrl+C foreground প্রোগ্রামকে SIGINT দেয়।
 * এডিটরের মতো যারা Ctrl+C নিজে চায় তারা ISIG বন্ধ করে:
 *   struct termios t; tcgetattr(0, &t); t.c_lflag &= ~ISIG; tcsetattr(0, TCSANOW, &t);
 * বাকি ফিল্ড আর বিট রাখা হয় Linux-এর মাপে, যাতে বাইরের কোড কম্পাইল হয়। */
#ifndef _TERMIOS_H
#define _TERMIOS_H

#include <_rozlibc.h>

typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int speed_t;

#define NCCS 32
struct termios {
    tcflag_t c_iflag, c_oflag, c_cflag, c_lflag;
    cc_t c_line;
    cc_t c_cc[NCCS];
    speed_t c_ispeed, c_ospeed;
};

/* c_lflag */
#define ISIG    0000001
#define ICANON  0000002
#define ECHO    0000010
#define ECHOE   0000020
#define ECHOK   0000040
#define ECHONL  0000100
#define IEXTEN  0100000
/* c_cc */
#define VINTR 0
#define VMIN  6
#define VTIME 5
/* tcsetattr */
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

int   tcgetattr(int fd, struct termios *t);
int   tcsetattr(int fd, int action, const struct termios *t);
void  cfmakeraw(struct termios *t);
pid_t tcgetpgrp(int fd);
int   tcsetpgrp(int fd, pid_t pgrp);

#endif
