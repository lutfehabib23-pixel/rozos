/* rozOS — initrd: Limine মডিউল হিসেবে আসা USTAR (tar) আর্কাইভ খুলে VFS-এ কপি করা
 *
 * মডিউল খোঁজা হয় bootinfo থেকে: cmdline "initrd" অথবা পাথ "initrd.tar"-এ শেষ।
 * সাপোর্টেড এন্ট্রি: সাধারণ ফাইল ('0' / '\0') ও ডিরেক্টরি ('5')। লিংক ও ডিভাইস
 * এন্ট্রি বাদ যায় (গোনা হয়)। ".keep" নামের ফাইল বাদ — শুধু খালি ডিরেক্টরি
 * git-এ রাখার জন্য থাকে। */
#ifndef ROZOS_INITRD_H
#define ROZOS_INITRD_H

#include <stdint.h>
#include <rozos/vfs.h>

struct initrd_stats {
    uint64_t archive_size;
    uint64_t files;
    uint64_t dirs;
    uint64_t bytes;
    uint64_t skipped;       /* সাপোর্ট নেই এমন এন্ট্রি */
    uint64_t errors;        /* checksum বা লেখার ত্রুটি */
    char     module_path[128];
};

/* initrd খুঁজে dest ডিরেক্টরিতে খোলা; সফল হলে 0, মডিউল না পেলে -ENOENT */
int initrd_load(const char *dest, struct initrd_stats *st);
/* ধাপ ২৪খ: একই, কিন্তু keep(path) সত্য হলে আগে থেকে থাকা ফাইল বদলায় না */
/* ধাপ ২৪গ: প্রতি ফাইলের আগে (কত বাইট হলো, মোট) — NULL = বন্ধ */
void initrd_set_progress(void (*fn)(uint64_t done, uint64_t total));
int initrd_load_keep(const char *dest, struct initrd_stats *st, int (*keep)(const char *path));

/* tmpfs ফাইলসিস্টেম (fs/tmpfs.c) */
extern const struct filesystem tmpfs_filesystem;

#endif
