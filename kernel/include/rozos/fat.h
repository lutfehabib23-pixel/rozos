/* rozOS — FAT16/FAT32 ফাইলসিস্টেম (ধাপ ১১খ; kernel/fs/fat.c)
 * boot partition (ESP) — Limine এখান থেকে kernel পড়ে, rozOS এটা /boot-এ mount করে। */
#ifndef ROZOS_FAT_H
#define ROZOS_FAT_H

#include <rozos/vfs.h>
#include <rozos/block.h>

extern const struct filesystem fat_filesystem;

/* ধাপ ২৪গ: বাইরের ডিস্কের আরো দুটো (fs/ntfs.c শুধু পড়া, fs/exfat.c পড়া-লেখা) */
extern const struct filesystem ntfs_filesystem;
extern const struct filesystem exfat_filesystem;
/* probe অনুযায়ী mount-এর driver নাম ("ext2", "fat", "ntfs", "exfat"); চেনা না হলে NULL */
const char *fs_driver_for(struct block_device *dev);

/* ডিভাইসে কোন ফাইলসিস্টেম আছে, mount না করে ("ext2", "fat32", "fat16", "ntfs", "-" ...) */
const char *fs_probe_name(struct block_device *dev);

#endif
