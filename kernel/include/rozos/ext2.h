/* rozOS — ext2 ফাইলসিস্টেম (fs/ext2.c)
 * vfs_mount(path, "ext2", "sda") — arg হলো ব্লক ডিভাইসের নাম। */
#ifndef ROZOS_EXT2_H
#define ROZOS_EXT2_H

#include <rozos/vfs.h>

extern const struct filesystem ext2_filesystem;

/* ধাপ ২৪খ: নতুন ext2 বানানো (৪ KiB block, sparse_super) আর নাম যাচাই */
struct block_device;
int ext2_mkfs(struct block_device *dev, const char *label);
int ext2_has_label(struct block_device *dev, const char *label);

#endif
