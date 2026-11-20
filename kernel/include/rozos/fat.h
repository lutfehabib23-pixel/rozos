/* rozOS — FAT16/FAT32 ফাইলসিস্টেম (ধাপ ১১খ; kernel/fs/fat.c)
 * boot partition (ESP) — Limine এখান থেকে kernel পড়ে, rozOS এটা /boot-এ mount করে। */
#ifndef ROZOS_FAT_H
#define ROZOS_FAT_H

#include <rozos/vfs.h>
#include <rozos/block.h>

extern const struct filesystem fat_filesystem;

/* ডিভাইসে কোন ফাইলসিস্টেম আছে, mount না করে ("ext2", "fat32", "fat16", "ntfs", "-" ...) */
const char *fs_probe_name(struct block_device *dev);

#endif
