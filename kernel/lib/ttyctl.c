/* rozOS — টার্মিনালের নিয়ন্ত্রণ (ধাপ ১৪), rozos/ttyctl.h দেখুন */

#include <stdint.h>
#include <stddef.h>
#include <rozos/ttyctl.h>
#include <rozos/proc.h>
#include <rozos/errno.h>
#include <rozos/string.h>

void ttyctl_init(struct ttyctl *t)
{
    memset(t, 0, sizeof(*t));
    t->lflag = TTY_ISIG;
    t->rows = 24;
    t->cols = 80;
}

int ttyctl_input(struct ttyctl *t, unsigned char c)
{
    if (c == 0x03 && (t->lflag & TTY_ISIG) && t->fg_pgid > 0) {
        return proc_kill_group(t->fg_pgid, SIGINT) > 0;
    }
    return 0;
}

void ttyctl_hangup(struct ttyctl *t)
{
    if (t->fg_pgid > 0) proc_kill_group(t->fg_pgid, SIGHUP);
}

int ttyctl_ioctl(struct ttyctl *t, uint64_t req, uint64_t arg,
                 int (*copy_in)(void *, const void *, size_t),
                 int (*copy_out)(void *, const void *, size_t))
{
    if (req == TIOCSPGRP) {
        int32_t pg;
        if (copy_in(&pg, (const void *)(uintptr_t)arg, sizeof(pg)) < 0) return -EFAULT;
        if (pg < 0) return -EINVAL;
        t->fg_pgid = pg;
        t->lflag |= TTY_ISIG;            /* নতুন কাজ সামনে: Ctrl+C আবার চালু */
        return 0;
    }
    if (req == TIOCGPGRP) {
        int32_t pg = t->fg_pgid;
        return copy_out((void *)(uintptr_t)arg, &pg, sizeof(pg));
    }
    if (req == TCGETS) {
        uint8_t tio[60];
        memset(tio, 0, sizeof(tio));
        memcpy(tio + 12, &t->lflag, 4);  /* c_iflag c_oflag c_cflag c_lflag */
        return copy_out((void *)(uintptr_t)arg, tio, sizeof(tio));
    }
    if (req == TCSETS || req == TCSETSW || req == TCSETSF) {
        uint8_t tio[60];
        if (copy_in(tio, (const void *)(uintptr_t)arg, sizeof(tio)) < 0) return -EFAULT;
        memcpy(&t->lflag, tio + 12, 4);
        return 0;
    }
    if (req == TIOCSCTTY || req == TIOCNOTTY) {
        return 0;
    }
    return -ENOTTY;
}
