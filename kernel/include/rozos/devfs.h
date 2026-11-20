/* rozOS — devfs: ডিভাইসগুলোকে /dev-এ ফাইল হিসেবে দেখানো
 *
 * ডিফল্ট ডিভাইস (devfs_init):
 *   /dev/null     লেখা হারায়, পড়লে শেষ (EOF)
 *   /dev/zero     পড়লে শূন্য বাইট
 *   /dev/console  লেখা → স্ক্রিন + serial; পড়া → কিবোর্ড/serial-এর ASCII অক্ষর
 *   /dev/serial0  COM1 সরাসরি
 *   /dev/kbd      কিবোর্ডের কাঁচা কী-কোড (int32, KEY_* সহ) — এডিটরের জন্য
 * পরে ড্রাইভাররা (ডিস্ক, নেটওয়ার্ক) devfs_register() দিয়ে নিজেদের যোগ করবে। */
#ifndef ROZOS_DEVFS_H
#define ROZOS_DEVFS_H

#include <stdint.h>
#include <stddef.h>
#include <rozos/vfs.h>

#define DEVFS_MAX_DEVICES 32

typedef int64_t (*devfs_read_fn)(void *ctx, uint64_t offset, void *buf, size_t len);
typedef int64_t (*devfs_write_fn)(void *ctx, uint64_t offset, const void *buf, size_t len);

int devfs_init(void);   /* ফাইলসিস্টেম নিবন্ধন + ডিফল্ট ডিভাইস */
int devfs_register(const char *name, devfs_read_fn read, devfs_write_fn write, void *ctx);

extern const struct filesystem devfs_filesystem;

#endif
