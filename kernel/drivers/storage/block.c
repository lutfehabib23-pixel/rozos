/* rozOS — ব্লক ডিভাইস স্তর ও write-through cache (rozos/block.h দেখুন)
 *
 * cache-এর একক 4 KiB (512-বাইট sector হলে ৮টা)। প্রতিটি এন্ট্রির ডেটা PMM-এর একটা
 * পুরো পেজ (HHDM দিয়ে অ্যাক্সেস)। পূর্ণ হলে সবচেয়ে কম সম্প্রতি ব্যবহৃত (LRU) এন্ট্রি
 * বদলানো হয়। লেখা সবসময় সাথে সাথে ডিস্কে যায় (write-through), তাই cache-এ কখনো
 * "ময়লা" ডেটা থাকে না এবং sync শুধু ডিস্কের নিজস্ব cache খালি করে। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/block.h>
#include <rozos/devfs.h>
#include <rozos/pmm.h>
#include <rozos/errno.h>
#include <rozos/string.h>
#include <rozos/sched.h>

#define CHUNK_SIZE     4096ULL
#define CACHE_ENTRIES  256           /* 1 MiB */
#define BIG_WRITE      (256 * 1024)  /* ধাপ ২৫ক: একটানা লেখার এক কমান্ডে সর্বোচ্চ */

struct cache_entry {
    struct block_device *dev;
    uint64_t chunk;                  /* বাইট offset / CHUNK_SIZE */
    uint64_t last_use;
    uint8_t *data;                   /* NULL = এখনো পেজ নেওয়া হয়নি */
    int valid;
};

static struct block_device *devices[BLOCK_MAX_DEVICES];
static size_t dev_count;
static struct cache_entry cache[CACHE_ENTRIES];
static uint64_t use_clock;

/* ---------- নিবন্ধন ---------- */

static int64_t dev_file_read(void *ctx, uint64_t off, void *buf, size_t len)
{
    struct block_device *d = (struct block_device *)ctx;
    uint64_t size = d->sector_count * d->sector_size;
    int err;
    if (off >= size) {
        return 0;
    }
    if (len > size - off) {
        len = (size_t)(size - off);
    }
    err = block_read_bytes(d, off, buf, len);
    return err < 0 ? err : (int64_t)len;
}

static int64_t dev_file_write(void *ctx, uint64_t off, const void *buf, size_t len)
{
    struct block_device *d = (struct block_device *)ctx;
    uint64_t size = d->sector_count * d->sector_size;
    int err;
    if (off >= size) {
        return -ENOSPC;
    }
    if (len > size - off) {
        len = (size_t)(size - off);
    }
    err = block_write_bytes(d, off, buf, len);
    return err < 0 ? err : (int64_t)len;
}

int block_register(struct block_device *dev, const char *prefix)
{
    size_t i, n = 0, plen = strlen(prefix);

    if (dev_count >= BLOCK_MAX_DEVICES) {
        return -ENOMEM;
    }
    if (dev->sector_size == 0 || CHUNK_SIZE % dev->sector_size != 0) {
        return -EINVAL;                               /* 512 বা 4096 বাইট sector সাপোর্টেড */
    }
    if (plen + 2 > BLOCK_NAME_MAX) {
        return -ENAMETOOLONG;
    }
    for (i = 0; i < dev_count; i++) {
        if (!devices[i]->parent && !strncmp(devices[i]->name, prefix, plen)) {   /* partition গোনা নয় */
            n++;
        }
    }
    memcpy(dev->name, prefix, plen);
    dev->name[plen] = (char)('a' + n);
    dev->name[plen + 1] = '\0';
    devices[dev_count++] = dev;
    devfs_register(dev->name, dev_file_read, dev_file_write, dev);
    return 0;
}

int block_register_partition(struct block_device *part)
{
    struct block_device *disk = part->parent;
    size_t plen;
    if (!disk || dev_count >= BLOCK_MAX_DEVICES || part->part_number == 0 || part->part_number > 99) {
        return -EINVAL;
    }
    plen = strlen(disk->name);
    if (plen + 3 > BLOCK_NAME_MAX) {
        return -ENAMETOOLONG;
    }
    memcpy(part->name, disk->name, plen);
    if (part->part_number >= 10) {
        part->name[plen++] = (char)('0' + part->part_number / 10);
    }
    part->name[plen++] = (char)('0' + part->part_number % 10);
    part->name[plen] = '\0';
    part->sector_size = disk->sector_size;
    part->ops = NULL;
    memcpy(part->model, disk->model, sizeof(part->model));
    devices[dev_count++] = part;
    devfs_register(part->name, dev_file_read, dev_file_write, part);
    return 0;
}

size_t block_device_count(void) { return dev_count; }

struct block_device *block_device_at(size_t index)
{
    return index < dev_count ? devices[index] : NULL;
}

struct block_device *block_find(const char *name)
{
    size_t i;
    if (!strncmp(name, "/dev/", 5)) {
        name += 5;
    }
    for (i = 0; i < dev_count; i++) {
        if (!strcmp(devices[i]->name, name)) {
            return devices[i];
        }
    }
    return NULL;
}

/* ---------- cache ---------- */

static struct cache_entry *cache_lookup(struct block_device *dev, uint64_t chunk)
{
    size_t i;
    for (i = 0; i < CACHE_ENTRIES; i++) {
        if (cache[i].valid && cache[i].dev == dev && cache[i].chunk == chunk) {
            cache[i].last_use = ++use_clock;
            return &cache[i];
        }
    }
    return NULL;
}

static struct cache_entry *cache_victim(void)
{
    size_t i, best = 0;
    for (i = 0; i < CACHE_ENTRIES; i++) {
        if (!cache[i].valid) {
            best = i;
            break;
        }
        if (cache[i].last_use < cache[best].last_use) {
            best = i;
        }
    }
    if (!cache[best].data) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            return NULL;
        }
        cache[best].data = (uint8_t *)pmm_phys_to_virt(phys);
    }
    cache[best].valid = 0;
    return &cache[best];
}

/* chunk-এ কতটা sector ডিস্কের ভেতরে পড়ে (শেষ chunk ছোট হতে পারে) */
static uint32_t chunk_sectors(struct block_device *dev, uint64_t chunk, uint64_t *first_lba)
{
    uint32_t per = (uint32_t)(CHUNK_SIZE / dev->sector_size);
    uint64_t lba = chunk * per;
    *first_lba = lba;
    if (lba >= dev->sector_count) {
        return 0;
    }
    if (lba + per > dev->sector_count) {
        per = (uint32_t)(dev->sector_count - lba);
    }
    return per;
}

/* chunk cache-এ আনা (দরকার হলে ডিস্ক থেকে) */
static int load_chunk(struct block_device *dev, uint64_t chunk, struct cache_entry **out)
{
    struct cache_entry *e = cache_lookup(dev, chunk);
    uint64_t lba;
    uint32_t n;
    int err;

    if (e) {
        dev->cache_hits++;
        *out = e;
        return 0;
    }
    dev->cache_misses++;
    n = chunk_sectors(dev, chunk, &lba);
    if (n == 0) {
        return -EINVAL;
    }
    e = cache_victim();
    if (!e) {
        return -ENOMEM;
    }
    if (n * dev->sector_size < CHUNK_SIZE) {
        memset(e->data, 0, CHUNK_SIZE);
    }
    err = dev->ops->read(dev, lba, n, e->data);
    dev->reads++;
    if (err < 0) {
        return err;
    }
    e->dev = dev;
    e->chunk = chunk;
    e->last_use = ++use_clock;
    e->valid = 1;
    *out = e;
    return 0;
}

/* ধাপ ২৪গ: পুরো chunk-টাই নতুন লেখা হবে — আগে ডিস্ক থেকে পড়ার দরকার নেই
 * (আগে প্রতি ৪ KiB লেখায় একবার পড়া + একবার লেখা হতো; পেন ড্রাইভে খুব ধীর) */
static int load_chunk_for_write(struct block_device *dev, uint64_t chunk, int full, struct cache_entry **out)
{
    struct cache_entry *e;
    uint64_t lba;
    if (!full || cache_lookup(dev, chunk) || chunk_sectors(dev, chunk, &lba) * dev->sector_size != CHUNK_SIZE) {
        return load_chunk(dev, chunk, out);
    }
    dev->cache_misses++;
    e = cache_victim();
    if (!e) {
        return -ENOMEM;
    }
    e->dev = dev;
    e->chunk = chunk;
    e->last_use = ++use_clock;
    e->valid = 1;
    *out = e;
    return 0;
}

/* ---------- পড়া/লেখা ---------- */

/* ধাপ ১১খ: partition-এর offset → parent ডিস্কের offset (সীমা যাচাই partition-এর মাপে) */
static int to_disk(struct block_device **dev, uint64_t *offset, size_t len, int write)
{
    struct block_device *d = *dev;
    if (*offset + len > d->sector_count * d->sector_size) {
        return -EINVAL;
    }
    while (d->parent) {
        if (write) d->writes++; else d->reads++;
        *offset += d->start_lba * d->sector_size;
        d = d->parent;
    }
    *dev = d;
    return 0;
}

static int do_block_read_bytes(struct block_device *dev, uint64_t offset, void *buf, size_t len)
{
    uint8_t *out = (uint8_t *)buf;
    if (to_disk(&dev, &offset, len, 0) < 0 || offset + len > dev->sector_count * dev->sector_size) {
        return -EINVAL;
    }
    if (dev->offline) {
        return -ENODEV;                          /* ধাপ ১৫খ */
    }
    while (len) {
        uint64_t chunk = offset / CHUNK_SIZE;
        size_t in = (size_t)(offset % CHUNK_SIZE);
        size_t n = (size_t)(CHUNK_SIZE - in);
        struct cache_entry *e;
        int err;
        if (n > len) {
            n = len;
        }
        err = load_chunk(dev, chunk, &e);
        if (err < 0) {
            return err;
        }
        memcpy(out, e->data + in, n);
        out += n;
        offset += n;
        len -= n;
    }
    return 0;
}

static int do_block_write_bytes(struct block_device *dev, uint64_t offset, const void *buf, size_t len)
{
    const uint8_t *in_buf = (const uint8_t *)buf;
    if (to_disk(&dev, &offset, len, 1) < 0 || offset + len > dev->sector_count * dev->sector_size) {
        return -EINVAL;
    }
    if (dev->read_only) {
        return -EROFS;                           /* ধাপ ১৫খ: সুরক্ষিত ডিস্ক */
    }
    if (dev->offline) {
        return -ENODEV;                          /* ধাপ ১৫খ: খুলে নেওয়া পেন ড্রাইভ */
    }
    while (len) {
        uint64_t chunk = offset / CHUNK_SIZE, first_lba;
        size_t in = (size_t)(offset % CHUNK_SIZE);
        size_t n = (size_t)(CHUNK_SIZE - in);
        uint32_t ss = dev->sector_size, s_first, s_last;
        struct cache_entry *e;
        int err;

        /* ধাপ ২৫ক: chunk-এর শুরু থেকে অন্তত দুটো পুরো chunk — এক কমান্ডে (BIG_WRITE পর্যন্ত)।
         * আগে প্রতি ৪ KiB আলাদা USB কমান্ড ছিল। লেখা এখনো সাথে সাথে ডিস্কে (write-through);
         * cache-এ যে chunk-গুলো আছে সেগুলোও একই ডেটায় হালনাগাদ, যাতে পড়ায় পুরোনো না আসে। */
        if (in == 0 && len >= 2 * CHUNK_SIZE &&
            chunk_sectors(dev, chunk, &first_lba) * ss == CHUNK_SIZE) {
            size_t big = len - len % CHUNK_SIZE, i;
            uint64_t last;
            if (big > BIG_WRITE) {
                big = BIG_WRITE;
            }
            last = chunk + big / CHUNK_SIZE - 1;
            for (i = 0; i < CACHE_ENTRIES; i++) {
                if (cache[i].valid && cache[i].dev == dev && cache[i].chunk >= chunk && cache[i].chunk <= last) {
                    memcpy(cache[i].data, in_buf + (size_t)(cache[i].chunk - chunk) * CHUNK_SIZE, CHUNK_SIZE);
                }
            }
            err = dev->ops->write(dev, first_lba, (uint32_t)(big / ss), in_buf);
            dev->writes++;
            if (err < 0) {
                for (i = 0; i < CACHE_ENTRIES; i++) {    /* ডিস্কের সাথে না মিলতে পারে — বাতিল */
                    if (cache[i].valid && cache[i].dev == dev && cache[i].chunk >= chunk && cache[i].chunk <= last) {
                        cache[i].valid = 0;
                    }
                }
                return err;
            }
            in_buf += big;
            offset += big;
            len -= big;
            continue;
        }

        if (n > len) {
            n = len;
        }
        /* chunk cache-এ এনে (আংশিক লেখার জন্য বাকি অংশ লাগে) তাতে বদল */
        err = load_chunk_for_write(dev, chunk, in == 0 && n == CHUNK_SIZE, &e);   /* ধাপ ২৪গ */
        if (err < 0) {
            return err;
        }
        memcpy(e->data + in, in_buf, n);

        /* শুধু বদলানো sector-গুলো ডিস্কে */
        chunk_sectors(dev, chunk, &first_lba);
        s_first = (uint32_t)(in / ss);
        s_last = (uint32_t)((in + n - 1) / ss);
        err = dev->ops->write(dev, first_lba + s_first, s_last - s_first + 1, e->data + s_first * ss);
        dev->writes++;
        if (err < 0) {
            e->valid = 0;                            /* ডিস্কের সাথে না মিললে cache বাতিল */
            return err;
        }
        in_buf += n;
        offset += n;
        len -= n;
    }
    return 0;
}

int block_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buf)
{
    return block_read_bytes(dev, lba * dev->sector_size, buf, (size_t)count * dev->sector_size);
}

int block_write(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf)
{
    return block_write_bytes(dev, lba * dev->sector_size, buf, (size_t)count * dev->sector_size);
}

static int do_block_sync(struct block_device *dev)
{
    size_t i;
    int err = 0;
    while (dev && dev->parent) {
        dev = dev->parent;                           /* partition → তার ডিস্ক */
    }
    for (i = 0; i < dev_count; i++) {
        if ((!dev || devices[i] == dev) && devices[i]->ops && devices[i]->ops->flush) {
            int e = devices[i]->ops->flush(devices[i]);
            if (e < 0) {
                err = e;
            }
        }
    }
    return err;
}

/* ---------- ধাপ ৬: ক্যাশ ও ড্রাইভার একসাথে একটাই থ্রেড ব্যবহার করে ---------- */

static struct kmutex block_lock = KMUTEX_INIT("block");

int block_read_bytes(struct block_device *dev, uint64_t offset, void *buf, size_t len)
{
    int r;
    kmutex_lock(&block_lock);
    r = do_block_read_bytes(dev, offset, buf, len);
    kmutex_unlock_yield(&block_lock);            /* ধাপ ২৫ক: অন্য প্রোগ্রামও ডিস্ক পায় */
    return r;
}

int block_write_bytes(struct block_device *dev, uint64_t offset, const void *buf, size_t len)
{
    int r;
    kmutex_lock(&block_lock);
    r = do_block_write_bytes(dev, offset, buf, len);
    kmutex_unlock_yield(&block_lock);            /* ধাপ ২৫ক: বারবার লেখার ফাঁকে অন্যরাও */
    return r;
}

/* ধাপ ২৪গ: বড় একটানা লেখা (mkfs-এর শূন্য ইত্যাদি) — cache এড়িয়ে সরাসরি ড্রাইভারে,
 * একবারে অনেক sector (USB-তে এক কমান্ডে ৬৪ KiB)। offset/len sector-এর গুণিতক হতে হবে।
 * এই অংশের cache entry বাতিল করা হয়। */
int block_write_direct(struct block_device *dev, uint64_t offset, const void *buf, size_t len)
{
    const uint8_t *in = (const uint8_t *)buf;
    uint32_t ss;
    size_t i;
    int err = 0;
    kmutex_lock(&block_lock);
    if (to_disk(&dev, &offset, len, 1) < 0 || offset + len > dev->sector_count * dev->sector_size) {
        err = -EINVAL;
    } else if (dev->read_only) {
        err = -EROFS;
    } else if (dev->offline) {
        err = -ENODEV;
    }
    ss = dev->sector_size;
    if (!err && (offset % ss || len % ss)) err = -EINVAL;
    if (!err) {
        for (i = 0; i < CACHE_ENTRIES; i++) {
            if (cache[i].valid && cache[i].dev == dev &&
                cache[i].chunk >= offset / CHUNK_SIZE && cache[i].chunk <= (offset + len - 1) / CHUNK_SIZE) {
                cache[i].valid = 0;
            }
        }
        while (len && !err) {
            size_t n = len > 256 * 1024 ? 256 * 1024 : len;
            err = dev->ops->write(dev, offset / ss, (uint32_t)(n / ss), in);
            dev->writes++;
            in += n;
            offset += n;
            len -= n;
        }
    }
    kmutex_unlock_yield(&block_lock);            /* ধাপ ২৫ক */
    return err;
}

/* ধাপ ২৪চ: batch (২৪ঘ-এ লেখা জমিয়ে পরে একসাথে) আসল পিসিতে ডেস্কটপ আটকে দিত, তাই
 * ২৪গ-এর পরীক্ষিত write-through-এ ফেরা। API থাকছে (ext2/FAT/exFAT/mkfs ডাকে), এখন
 * কিছু করে না — পরে আলাদা করে, আসল পিসিতে যাচাই করে ফেরানো যাবে। */
void block_batch_begin(struct block_device *dev)
{
    (void)dev;
}

int block_batch_end(struct block_device *dev)
{
    (void)dev;
    return 0;
}

int block_is_read_only(struct block_device *dev)
{
    while (dev && dev->parent) dev = dev->parent;
    return dev ? dev->read_only : 0;
}

int block_sync(struct block_device *dev)
{
    int r;
    kmutex_lock(&block_lock);
    r = do_block_sync(dev);
    kmutex_unlock(&block_lock);
    return r;
}
