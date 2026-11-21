/* rozOS — sys/mount.h (ধাপ ১৫ক): ডিস্ক/পেন ড্রাইভ জোড়া আর খোলা
 *   mount("/dev/nvmea1", "/mnt/nvme", "auto", 0, NULL)   — ext2 বা FAT নিজে চেনে
 *   umount("/mnt/nvme") */
#ifndef _SYS_MOUNT_H
#define _SYS_MOUNT_H

int mount(const char *source, const char *target, const char *fstype, unsigned long flags, const void *data);
int umount(const char *target);
int umount2(const char *target, int flags);

#endif
