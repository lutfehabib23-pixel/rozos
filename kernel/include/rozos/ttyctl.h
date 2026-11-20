/* rozOS — টার্মিনালের নিয়ন্ত্রণ (ধাপ ১৪): foreground process group, Ctrl+C → SIGINT,
 * termios-এর ISIG বিট, মাপ। কনসোল (/dev/console) আর প্রতিটা PTY-র একটা করে।
 *
 * নিয়ম: Ctrl+C (0x03) এলে — ISIG চালু থাকলে — foreground গ্রুপকে SIGINT। বাইটটাও
 * পড়ার জন্য থাকে (shell SIGINT উপেক্ষা করে আর বাইট দেখে লাইন বাতিল করে)। এডিটরের মতো
 * প্রোগ্রাম ISIG বন্ধ করে (tcsetattr) Ctrl+C-কে নিজের কাজে (copy) লাগায়।
 * TIOCSPGRP (shell নতুন কাজ সামনে আনলে) ISIG আবার চালু করে — তাই কোনো প্রোগ্রাম বন্ধ
 * না করে চলে গেলেও Ctrl+C হারায় না। */
#ifndef ROZOS_TTYCTL_H
#define ROZOS_TTYCTL_H

#include <stdint.h>
#include <stddef.h>

#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403
#define TCSETSF     0x5404
#define TIOCSCTTY   0x540E
#define TIOCGPGRP   0x540F
#define TIOCSPGRP   0x5410
#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414
#define TIOCNOTTY   0x5422

#define TTY_ISIG    0x0001       /* termios c_lflag-এর বিট (Linux-এর মতো) */

struct ttyctl {
    int fg_pgid;                 /* 0 = কেউ না */
    uint32_t lflag;              /* শুধু ISIG মানে রাখে; বাকি বিট ফেরত দেওয়ার জন্য রাখা */
    uint16_t rows, cols;         /* PTY-র মাপ (কনসোলের মাপ sys_fs.c নিজে রাখে) */
};

void ttyctl_init(struct ttyctl *t);
/* ইনপুটে একটা বাইট এল: 0x03 হলে আর ISIG চালু থাকলে SIGINT পাঠায়। ফেরত 1 = কেউ
 * signal পেয়েছে, বাইটটা ফেলে দাও (নইলে shell পরে আবার "^C" দেখত); 0 = বাইট রাখো */
int  ttyctl_input(struct ttyctl *t, unsigned char c);
/* টার্মিনাল বন্ধ: foreground গ্রুপে SIGHUP */
void ttyctl_hangup(struct ttyctl *t);
/* pgrp/termios-এর ioctl; না চিনলে -ENOTTY (ডাকা ফাংশন বাকিগুলো দেখে)
 * copy_in/copy_out = copy_from_user/copy_to_user */
int  ttyctl_ioctl(struct ttyctl *t, uint64_t req, uint64_t arg,
                  int (*copy_in)(void *, const void *, size_t),
                  int (*copy_out)(void *, const void *, size_t));

struct ttyctl *console_ttyctl(void);   /* devfs.c */
struct ttyctl *serial_ttyctl(void);    /* /dev/serial0 */

#endif
