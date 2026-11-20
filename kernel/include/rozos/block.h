/* rozOS — ব্লক ডিভাইস স্তর
 *
 * ডিস্ক ড্রাইভার (AHCI, পরে ATA PIO, NVMe) নিচে, ফাইলসিস্টেম (ext2) ওপরে। ড্রাইভার
 * শুধু "sector n থেকে count টা পড়ো/লেখো" দেয়; বাকিটা এই স্তরের:
 *   - নাম (sda, sdb ...) আর /dev-এ কাঁচা ডিভাইস ফাইল
 *   - বাইট-ভিত্তিক পড়া/লেখা (block_read_bytes) — sector-এর মাঝখান থেকেও
 *   - 4 KiB-এর write-through cache: পড়া দ্রুত, আর প্রতিটি লেখা সাথে সাথে ডিস্কে যায়
 *     (বিদ্যুৎ চলে গেলেও ফাইলসিস্টেম নষ্ট হওয়ার ঝুঁকি কম) */
#ifndef ROZOS_BLOCK_H
#define ROZOS_BLOCK_H

#include <stdint.h>
#include <stddef.h>

#define BLOCK_MAX_DEVICES 8
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
};

/* ড্রাইভার নিবন্ধন: নাম নিজে দেয় ("sd" + a, b, c ...), /dev-এও যোগ করে */
int block_register(struct block_device *dev, const char *prefix);
size_t block_device_count(void);
struct block_device *block_device_at(size_t index);
struct block_device *block_find(const char *name);

/* sector-ভিত্তিক (cache হয়ে) */
int block_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buf);
int block_write(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf);
/* বাইট-ভিত্তিক — offset/len যেকোনো হতে পারে */
int block_read_bytes(struct block_device *dev, uint64_t offset, void *buf, size_t len);
int block_write_bytes(struct block_device *dev, uint64_t offset, const void *buf, size_t len);

int block_sync(struct block_device *dev);           /* NULL = সব ডিভাইস */

#endif
