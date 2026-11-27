/* rozlibc — sys/ioctl.h (শুধু কনসোলের মাপ) */
#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H
#ifdef __cplusplus
extern "C" {
#endif

#define TCGETS     0x5401
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define TIOCSCTTY  0x540E      /* ধাপ ১৪ */
#define TIOCGPGRP  0x540F
#define TIOCSPGRP  0x5410
/* ধাপ ১২ (rozOS-এর নিজস্ব): serial টার্মিনালের মাপ নিজে থেকে ধরা, ফল winsize-এ।
 * DETECT হাতে বসানো মাপ (stty ROWS COLS) রাখে; DETECT_FORCE সবসময় নতুন করে ধরে */
#define TIOCROZDETECT       0x54F0
#define TIOCROZDETECT_FORCE 0x54F1
/* ধাপ ১৩: কনসোলের ফন্ট বদলানো (setfont) */
#define TIOCROZSETFONT      0x54F2
struct rozos_font_req {
    unsigned int width, height;          /* width ≤ 16, height ≤ 32 */
    unsigned int flags;                  /* 1 = বুটের ফন্টে ফেরা */
    unsigned int pad;
    unsigned long data;                  /* 256 গ্লিফ, প্রতিটা height × ((width+7)/8) বাইট */
};

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

int ioctl(int fd, unsigned long req, ...);

/* ধাপ ১৯ছ: ফাইলের flag (Linux-এর <linux/fs.h>-এর একই মান) — chattr/lsattr।
 * rozOS-এ শুধু FS_IMMUTABLE_FL: মোছা, নাম বদল, লেখা সব EPERM (ext2 আর tmpfs-এ; FAT-এ নেই)।
 *   int fl; ioctl(fd, FS_IOC_GETFLAGS, &fl); fl |= FS_IMMUTABLE_FL; ioctl(fd, FS_IOC_SETFLAGS, &fl); */
#define FS_IOC_GETFLAGS 0x80086601UL
#define FS_IOC_SETFLAGS 0x40086602UL
#define FS_IMMUTABLE_FL 0x00000010

#ifdef __cplusplus
}
#endif
#endif
