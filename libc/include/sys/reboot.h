/* rozlibc — sys/reboot.h (ধাপ ১১খ): Linux-এর মতো reboot(cmd)
 * কার্নেল আগে ডিস্ক sync করে। শুধু ফেরে ব্যর্থ হলে (-1, errno)। */
#ifndef _SYS_REBOOT_H
#define _SYS_REBOOT_H
#ifdef __cplusplus
extern "C" {
#endif

#define RB_AUTOBOOT    0x01234567     /* আবার চালু */
#define RB_POWER_OFF   0x4321fedc     /* বিদ্যুৎ বন্ধ (ACPI) */
#define RB_HALT_SYSTEM 0xcdef0123     /* থামা */

int reboot(int cmd);

#ifdef __cplusplus
}
#endif
#endif
