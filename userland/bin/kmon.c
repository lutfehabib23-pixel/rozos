/* rozOS — /bin/kmon: কার্নেল মনিটর (kmon) ডাকা (ধাপ ৭)
 *
 * kmon কার্নেলের ভেতরে আলাদা থ্রেডে চলে; kmon-এ "exit" দিলে এখানে ফেরে।
 * (পরে Ctrl+K দিয়েও ডাকা যাবে।)
 * ধাপ ১৪: kmon সরাসরি কনসোলে (পর্দা আর কিবোর্ড) কথা বলে — ডেস্কটপের টার্মিনাল-উইন্ডো
 * (PTY) থেকে নয়। তাই stdin কনসোল বা serial না হলে বুঝিয়ে বলে থামে। */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <rozos.h>
#include <sys/stat.h>

/* stdin কি /dev/console বা /dev/serial0? */
static int on_console(void)
{
    struct stat a, b;
    if (fstat(0, &a) < 0) return 0;
    if (stat("/dev/console", &b) == 0 && a.st_ino == b.st_ino) return 1;
    if (stat("/dev/serial0", &b) == 0 && a.st_ino == b.st_ino) return 1;
    return 0;
}

int main(void)
{
    if (!on_console()) {
        fprintf(stderr, "kmon: the kernel monitor talks to the text console directly, not to a\n"
                        "terminal window. Use the serial terminal (make run-serial), or go to the\n"
                        "text console: Start > Text mode (or `textmode`), then run kmon.\n");
        return 1;
    }
    fflush(stdout);
    if (rozos_kmon() < 0) {
        fprintf(stderr, "kmon: %s\n", errno == EBUSY ? "already running" : strerror(errno));
        return 1;
    }
    return 0;
}
