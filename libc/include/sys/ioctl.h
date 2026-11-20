/* rozlibc — sys/ioctl.h (শুধু কনসোলের মাপ) */
#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H

#define TCGETS     0x5401
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

int ioctl(int fd, unsigned long req, ...);

#endif
