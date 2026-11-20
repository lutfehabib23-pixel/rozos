/* rozOS — ext2 ফাইলসিস্টেম (fs/ext2.c)
 * vfs_mount(path, "ext2", "sda") — arg হলো ব্লক ডিভাইসের নাম। */
#ifndef ROZOS_EXT2_H
#define ROZOS_EXT2_H

#include <rozos/vfs.h>

extern const struct filesystem ext2_filesystem;

#endif
