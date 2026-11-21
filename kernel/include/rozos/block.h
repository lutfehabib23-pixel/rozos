/* rozOS — ব্লক ডিভাইস স্তর
 *
 * ডিস্ক ড্রাইভার (AHCI, পরে ATA PIO, NVMe) নিচে, ফাইলসিস্টেম (ext2) ওপরে। ড্রাইভার
 * শুধু "sector n থেকে count টা পড়ো/লেখো" দেয়; বাকিটা এই স্তরের:
 *   - নাম (sda, sdb ...) আর /dev-এ কাঁচা ডিভাইস ফাইল
 *   - বাইট-ভিত্তিক পড়া/লেখা (block_read_bytes) — sector-এর মাঝখান থেকেও
 *   - 4 KiB-এর write-through cache: পড়া দ্রুত, আর প্রতিটি লেখা সাথে সাথে ডিস্কে যায়
 *     (বিদ্যুৎ চলে গেলেও ফাইলসিস্টেম নষ্ট হওয়ার ঝুঁকি কম)
 *   - ধাপ ১১খ: partition (sda1, sda2 ...) — নিজস্ব ড্রাইভার নেই, parent ডিস্কের একটা
 *     অংশ; পড়া/লেখা parent-এর cache দিয়েই যায়, তাই /dev/sda আর /dev/sda2 সবসময় এক। */
#ifndef ROZOS_BLOCK_H
#define ROZOS_BLOCK_H

#include <stdint.h>
#include <stddef.h>

#define BLOCK_MAX_DEVICES 32      /* ধাপ ১১খ: ডিস্ক + partition (আগে 8) */
#define BLOCK_NAME_MAX    16

struct block_device;

struct block_ops {
    /* ফেরত: 0 = সফল, <0 = -ERRNO */
    int (*read)(struct block_device *dev, uint64_t lba, uint32_t count, void *buf);
    int (*write)(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf);
    int (*flush)(struct block_device *dev);          /* ডিস্কের নিজস্ব cache খালি করা */
};

struct block_device {
    char     name[BLOCK_NAME_MAX];
    char     model[41];                              /* ডিস্কের মডেলের নাম */
    uint32_t sector_size;                            /* সাধারণত 512 */
    uint64_t sector_count;
    const struct block_ops *ops;
    void    *driver_data;
    /* পরিসংখ্যান */
    uint64_t reads, writes, cache_hits, cache_misses;
    /* ধাপ ১১খ: partition হলে parent != NULL, ops == NULL */
    struct block_device *parent;
    uint64_t start_lba;                              /* parent-এর sector-এ */
    uint32_t part_number;                            /* 1, 2 ... */
    char     part_type[24];                          /* "EFI System", "Linux data", "FAT32" ... */
    char     part_label[40];                         /* GPT-র নাম (ASCII) */
    /* ডিস্কের জন্য: partition table ("gpt", "mbr", "" = নেই) আর কয়টা partition */
    char     table[8];
    uint32_t partitions;
    /* ধাপ ১৫খ */
    int      read_only;                              /* লেখা -EROFS (আসল কম্পিউটারের অন্য OS-এর ডিস্ক) */
    int      removable;                              /* USB পেন ড্রাইভ ইত্যাদি */
    int      offline;                                /* খুলে নেওয়া হয়েছে — পড়া/লেখা -ENODEV */
};

/* ড্রাইভার নিবন্ধন: নাম নিজে দেয় ("sd" + a, b, c ...), /dev-এও যোগ করে */
int block_register(struct block_device *dev, const char *prefix);
/* ধাপ ১১খ: partition নিবন্ধন — part->parent, start_lba, sector_count, part_number আগে বসানো;
 * নাম = parent-এর নাম + নম্বর ("sda2"), /dev-এও যোগ হয় */
int block_register_partition(struct block_device *part);
/* ধাপ ১১খ: ডিস্কের GPT বা MBR পড়ে partition-গুলো নিবন্ধন; ফেরত: কয়টা পাওয়া গেল */
int partition_scan(struct block_device *disk);
size_t block_device_count(void);
struct block_device *block_device_at(size_t index);
struct block_device *block_find(const char *name);

/* sector-ভিত্তিক (cache হয়ে) */
int block_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buf);
int block_write(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf);
/* বাইট-ভিত্তিক — offset/len যেকোনো হতে পারে */
int block_read_bytes(struct block_device *dev, uint64_t offset, void *buf, size_t len);
int block_write_bytes(struct block_device *dev, uint64_t offset, const void *buf, size_t len);
/* ধাপ ১৫খ: ডিস্ক (বা partition-এর ডিস্ক) শুধু পড়া যায় কিনা */
int block_is_read_only(struct block_device *dev);

int block_sync(struct block_device *dev);           /* NULL = সব ডিভাইস */

#endif
