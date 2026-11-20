/* rozOS — /bin/kmon: কার্নেল মনিটর (kmon) ডাকা (ধাপ ৭)
 *
 * kmon কার্নেলের ভেতরে আলাদা থ্রেডে চলে; kmon-এ "exit" দিলে এখানে ফেরে।
 * (পরে Ctrl+K দিয়েও ডাকা যাবে।) */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <rozos.h>

int main(void)
{
    fflush(stdout);
    if (rozos_kmon() < 0) {
        fprintf(stderr, "kmon: %s\n", errno == EBUSY ? "already running" : strerror(errno));
        return 1;
    }
    return 0;
}
