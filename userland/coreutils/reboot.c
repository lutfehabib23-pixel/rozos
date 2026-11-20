/* rozOS — reboot: কম্পিউটার আবার চালু (ধাপ ১১খ)
 * কার্নেল আগে ডিস্ক sync করে। Limine-এর মেনুতে অন্য kernel বাছা যায় (help boot-menu)। */

#include <stdio.h>
#include <sys/reboot.h>

int main(void)
{
    printf("rebooting...\n");
    fflush(stdout);
    reboot(RB_AUTOBOOT);
    perror("reboot");
    return 1;
}
