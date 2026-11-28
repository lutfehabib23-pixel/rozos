/* rozOS — ব্লক ডিভাইস স্তর ও cache (rozos/block.h দেখুন)
 *
 * cache-এর একক 4 KiB (512-বাইট sector হলে ৮টা)। প্রতিটি এন্ট্রির ডেটা PMM-এর একটা
 * পুরো পেজ (HHDM দিয়ে অ্যাক্সেস)। পূর্ণ হলে সবচেয়ে কম সম্প্রতি ব্যবহৃত (LRU) এন্ট্রি
 * বদলানো হয়।
 *
 * ধাপ ২৫খ: write-back। ছোট লেখা (bitmap, inode, superblock, ছোট ফাইল) সাথে সাথে ডিস্কে
 * না গিয়ে cache-এ "dirty" হয়ে থাকে — একই ব্লক বারবার বদলালেও ডিস্কে যায় একবার।
 * "bflush" কার্নেল-থ্রেড ~২ সেকেন্ড পরপর dirty অংশগুলো ঠিকানার ক্রমে সাজিয়ে, পাশাপাশি
 * chunk জুড়ে বড় কমান্ডে (BIG_WRITE পর্যন্ত) লেখে; প্রতিটা কমান্ডের ফাঁকে lock ছাড়ে।
 * sync/fsync/umount/reboot/poweroff সব সাথে সাথে লেখে। বড় একটানা লেখা আগের মতোই
 * সরাসরি ডিস্কে (write-through)। flusher চালুর আগে (বুটের শুরুতে) সব write-through। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/block.h>
#include <rozos/devfs.h>
#include <rozos/pmm.h>
#include <rozos/errno.h>
#include <rozos/string.h>
#include <rozos/sched.h>
#include <rozos/heap.h>
#include <rozos/timer.h>
#include <rozos/printf.h>

#define CHUNK_SIZE     4096ULL
#define CACHE_ENTRIES  4096          /* ধাপ ২৫খ: 16 MiB (আগে 256 = 1 MiB); পেজ লাগলে তবেই নেওয়া */
#define BIG_WRITE      (256 * 1024)  /* ধাপ ২৫ক: একটানা লেখার এক কমান্ডে সর্বোচ্চ */
#define HASH_SIZE      1024          /* ধাপ ২৫খ: (dev, chunk) → এন্ট্রি দ্রুত খোঁজা */
#define DIRTY_MAX      2048          /* ধাপ ২৫খ: 8 MiB dirty জমলে লেখক নিজেই লিখে ফেলে */
#define FLUSH_AGE_MS   2000          /* ধাপ ২৫খ: এর বেশি পুরোনো dirty থাকলে bflush লেখে */
#define FLUSH_RUN      (BIG_WRITE / CHUNK_SIZE)

struct cache_entry {
    struct block_device *dev;
    uint64_t chunk;                  /* বাইট offset / CHUNK_SIZE */
    uint64_t last_use;
    uint8_t *data;                   /* NULL = এখনো পেজ নেওয়া হয়নি */
    int valid;
    int dirty;                       /* ধাপ ২৫খ: ডিস্কে এখনো লেখা হয়নি */
    uint64_t dirty_ms;               /* কখন থেকে dirty */
    uint32_t hnext;                  /* hash-এর পরের এন্ট্রি (index + 1; 0 = শেষ) */
};

static struct block_device *devices[BLOCK_MAX_DEVICES];
static size_t dev_count;
static struct cache_entry cache[CACHE_ENTRIES];
static uint64_t use_clock;
static uint32_t hash_head[HASH_SIZE];  /* index + 1; 0 = খালি */
static size_t dirty_count;
static struct thread *flusher;
static uint8_t *flush_buf;             /* পাশাপাশি chunk জোড়া লাগানোর জায়গা (BIG_WRITE) */
static struct block_wb_stats wb;       /* kmon usbstat/disks */
static struct kmutex block_lock = KMUTEX_INIT("block");   /* ধাপ ৬; ২৫খ-এ ওপরে আনা */

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

/* ধাপ ২৫খ: hash — আগে প্রতিটা খোঁজায় পুরো cache ঘোরা হতো (২৫৬টা; এখন ৪০৯৬টা) */
static uint32_t hash_of(struct block_device *dev, uint64_t chunk)
{
    uint64_t h = ((uint64_t)(uintptr_t)dev >> 4) ^ (chunk * 0x9E3779B97F4A7C15ULL);
    return (uint32_t)((h >> 32) % HASH_SIZE);
}

static void hash_insert(struct cache_entry *e)
{
    uint32_t h = hash_of(e->dev, e->chunk);
    e->hnext = hash_head[h];
    hash_head[h] = (uint32_t)(e - cache) + 1;
}

static void hash_remove(struct cache_entry *e)
{
    uint32_t *p = &hash_head[hash_of(e->dev, e->chunk)];
    uint32_t me = (uint32_t)(e - cache) + 1;
    while (*p) {
        if (*p == me) {
            *p = e->hnext;
            break;
        }
        p = &cache[*p - 1].hnext;
    }
    e->hnext = 0;
}

/* এন্ট্রি বাতিল (dirty হলে তার হিসাবও কমে — যে ডাকে সে নিশ্চিত যে ডেটা আর লাগবে না) */
static void cache_drop(struct cache_entry *e)
{
    if (!e->valid) return;
    if (e->dirty) {
        e->dirty = 0;
        dirty_count--;
    }
    hash_remove(e);
    e->valid = 0;
}

/* এন্ট্রিকে (dev, chunk)-এর হিসেবে চালু করা */
static void cache_set(struct cache_entry *e, struct block_device *dev, uint64_t chunk)
{
    e->dev = dev;
    e->chunk = chunk;
    e->last_use = ++use_clock;
    e->dirty = 0;
    e->valid = 1;
    hash_insert(e);
}

static void mark_dirty(struct cache_entry *e)
{
    if (e->dirty) {
        wb.absorbed++;                          /* ডিস্কে না গিয়েই আরেকটা লেখা জমল */
        return;
    }
    e->dirty = 1;
    e->dirty_ms = timer_uptime_ms();
    dirty_count++;
}

static void mark_clean(struct cache_entry *e)
{
    if (e->dirty) {
        e->dirty = 0;
        dirty_count--;
    }
}

static struct cache_entry *cache_find(struct block_device *dev, uint64_t chunk)
{
    uint32_t i = hash_head[hash_of(dev, chunk)];
    while (i) {
        struct cache_entry *e = &cache[i - 1];
        if (e->valid && e->dev == dev && e->chunk == chunk) {
            return e;
        }
        i = e->hnext;
    }
    return NULL;
}

static struct cache_entry *cache_lookup(struct block_device *dev, uint64_t chunk)
{
    struct cache_entry *e = cache_find(dev, chunk);
    if (e) {
        e->last_use = ++use_clock;
    }
    return e;
}

static int flush_locked(struct block_device *only);

/* ধাপ ২৫খ: বদলানোর এন্ট্রি — আগে পেজসহ খালি এন্ট্রি, তারপর নতুন পেজ, শেষে সবচেয়ে
 * পুরোনো "পরিষ্কার" এন্ট্রি। dirty এন্ট্রি কখনো সরাসরি নয় — সব dirty হলে আগে লিখে ফেলা।
 * পেজ না পেলে (মেমোরি কম) যা আছে তাতেই চলে। */
static struct cache_entry *cache_victim(void)
{
    int pass;
    for (pass = 0; pass < 2; pass++) {
        struct cache_entry *lru = NULL, *empty = NULL;
        size_t i;
        for (i = 0; i < CACHE_ENTRIES; i++) {
            struct cache_entry *e = &cache[i];
            if (!e->valid) {
                if (e->data) {
                    return e;
                }
                if (!empty) {
                    empty = e;
                }
            } else if (!e->dirty && (!lru || e->last_use < lru->last_use)) {
                lru = e;
            }
        }
        if (empty) {
            uint64_t phys = pmm_alloc_page();
            if (phys) {
                empty->data = (uint8_t *)pmm_phys_to_virt(phys);
                return empty;
            }
        }
        if (lru) {
            cache_drop(lru);
            return lru;
        }
        if (flush_locked(NULL) < 0 && pass) {
            break;
        }
    }
    return NULL;
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
    cache_set(e, dev, chunk);
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
    cache_set(e, dev, chunk);
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
            /* ধাপ ২৫খ: পুরো cache না ঘুরে hash দিয়ে শুধু এই chunk-গুলো */
            for (i = 0; chunk + i <= last; i++) {
                struct cache_entry *c = cache_find(dev, chunk + i);
                if (c) {
                    memcpy(c->data, in_buf + i * CHUNK_SIZE, CHUNK_SIZE);
                }
            }
            err = dev->ops->write(dev, first_lba, (uint32_t)(big / ss), in_buf);
            dev->writes++;
            for (i = 0; chunk + i <= last; i++) {
                struct cache_entry *c = cache_find(dev, chunk + i);
                if (!c) {
                    continue;
                }
                if (err < 0) {
                    cache_drop(c);                       /* ডিস্কের সাথে না মিলতে পারে — বাতিল */
                } else {
                    mark_clean(c);                       /* ধাপ ২৫খ: পুরো chunk এইমাত্র ডিস্কে গেল */
                }
            }
            if (err < 0) {
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

        /* ধাপ ২৫খ: write-back — পরে bflush/sync একসাথে লিখবে */
        if (flusher) {
            mark_dirty(e);
            wb.cached_writes++;
            in_buf += n;
            offset += n;
            len -= n;
            continue;
        }

        /* শুধু বদলানো sector-গুলো ডিস্কে */
        chunk_sectors(dev, chunk, &first_lba);
        s_first = (uint32_t)(in / ss);
        s_last = (uint32_t)((in + n - 1) / ss);
        err = dev->ops->write(dev, first_lba + s_first, s_last - s_first + 1, e->data + s_first * ss);
        dev->writes++;
        if (err < 0) {
            cache_drop(e);                           /* ডিস্কের সাথে না মিললে cache বাতিল */
            return err;
        }
        in_buf += n;
        offset += n;
        len -= n;
    }
    if (dirty_count >= DIRTY_MAX) {
        wb.forced++;                                 /* ধাপ ২৫খ: অনেক জমেছে — লেখক নিজেই লেখে */
        flush_locked(NULL);
    }
    return 0;
}

/* ---------- ধাপ ২৫খ: dirty অংশ ডিস্কে লেখা ---------- */

/* খুলে নেওয়া ডিস্কের dirty অংশ আর লেখার উপায় নেই — বাতিল (একবার জানানো) */
static void drop_offline(struct block_device *dev)
{
    size_t i, n = 0;
    for (i = 0; i < CACHE_ENTRIES; i++) {
        if (cache[i].valid && cache[i].dirty && cache[i].dev == dev) {
            cache_drop(&cache[i]);
            n++;
        }
    }
    if (n) {
        wb.lost_chunks += n;
        kprintf("[block] /dev/%s was removed: %llu KiB not yet written were lost\n", dev->name,
                (unsigned long long)(n * CHUNK_SIZE / 1024));
    }
}

/* ঠিকানার ক্রমে সবচেয়ে আগের dirty chunk থেকে শুরু করে পাশাপাশি যতগুলো dirty আছে
 * (FLUSH_RUN পর্যন্ত) এক কমান্ডে লেখা। ফেরত: 1 = লেখা হলো, 0 = আর কিছু নেই, <0 = ত্রুটি।
 * only = NULL হলে সব ডিস্ক, নইলে শুধু সেই ডিস্ক (partition নয়, আসল ডিস্ক)। */
static int flush_one_run(struct block_device *only)
{
    struct cache_entry *first = NULL;
    struct block_device *dev;
    uint64_t lba;
    uint32_t ss, n, run = 1;
    size_t i;
    int err;

    for (i = 0; i < CACHE_ENTRIES; i++) {
        struct cache_entry *e = &cache[i];
        if (!e->valid || !e->dirty || (only && e->dev != only)) {
            continue;
        }
        if (!first || (uintptr_t)e->dev < (uintptr_t)first->dev ||
            (e->dev == first->dev && e->chunk < first->chunk)) {
            first = e;
        }
    }
    if (!first) {
        return 0;
    }
    dev = first->dev;
    if (dev->offline) {
        drop_offline(dev);
        return 1;
    }
    ss = dev->sector_size;
    n = chunk_sectors(dev, first->chunk, &lba);
    if (n * ss == CHUNK_SIZE && !flush_buf) {
        flush_buf = (uint8_t *)kmalloc(BIG_WRITE);   /* না পেলে এক chunk করে */
    }
    if (n * ss == CHUNK_SIZE && flush_buf) {
        uint64_t l2;
        while (run < FLUSH_RUN) {
            struct cache_entry *e = cache_find(dev, first->chunk + run);
            if (!e || !e->dirty || chunk_sectors(dev, first->chunk + run, &l2) * ss != CHUNK_SIZE) {
                break;
            }
            run++;
        }
    }
    if (run == 1) {
        err = dev->ops->write(dev, lba, n, first->data);
    } else {
        for (i = 0; i < run; i++) {
            memcpy(flush_buf + i * CHUNK_SIZE, cache_find(dev, first->chunk + i)->data, CHUNK_SIZE);
        }
        err = dev->ops->write(dev, lba, (uint32_t)(run * (CHUNK_SIZE / ss)), flush_buf);
    }
    dev->writes++;
    wb.commands++;
    wb.chunks += run;
    for (i = 0; i < run; i++) {
        struct cache_entry *e = cache_find(dev, first->chunk + i);
        if (!e) {
            continue;
        }
        if (err < 0) {
            cache_drop(e);                           /* বারবার চেষ্টা করে আটকে না থাকা */
        } else {
            mark_clean(e);
        }
    }
    if (err < 0) {
        wb.lost_chunks += run;
        wb.errors++;
        kprintf("[block] /dev/%s: writing cached data failed (error %d), %u KiB lost\n", dev->name, err,
                (unsigned)(run * CHUNK_SIZE / 1024));
        return err;
    }
    return 1;
}

/* block_lock হাতে রেখে সব (বা একটা ডিস্কের) dirty লেখা */
static int flush_locked(struct block_device *only)
{
    int r, err = 0;
    while ((r = flush_one_run(only)) != 0) {
        if (r < 0) {
            err = r;
        }
    }
    return err;
}

/* bflush: আধা সেকেন্ড পরপর দেখে; সবচেয়ে পুরোনো dirty FLUSH_AGE_MS পেরোলে বা অনেক জমলে
 * সব লেখে — প্রতিটা কমান্ডের পরে lock ছেড়ে অন্যদের (পড়া, অন্য প্রোগ্রাম) সুযোগ দিয়ে */
static int flusher_main(void *arg)
{
    (void)arg;
    for (;;) {
        uint64_t now, oldest;
        size_t i;
        int due;
        sched_sleep_ms(500);
        if (!dirty_count) {
            continue;
        }
        kmutex_lock(&block_lock);
        now = timer_uptime_ms();
        oldest = now;
        for (i = 0; i < CACHE_ENTRIES; i++) {
            if (cache[i].valid && cache[i].dirty && cache[i].dirty_ms < oldest) {
                oldest = cache[i].dirty_ms;
            }
        }
        due = dirty_count >= DIRTY_MAX / 2 || now - oldest >= FLUSH_AGE_MS;
        kmutex_unlock(&block_lock);
        if (!due) {
            continue;
        }
        wb.flushes++;
        for (;;) {
            int r;
            kmutex_lock(&block_lock);
            r = flush_one_run(NULL);
            kmutex_unlock_yield(&block_lock);
            if (r == 0) {
                break;
            }
        }
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
    err = flush_locked(dev);                         /* ধাপ ২৫খ: আগে cache-এর dirty অংশ */
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
/* (block_lock ধাপ ২৫খ থেকে ফাইলের ওপরে — flusher-ও নেয়) */

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
    if (!flusher && sched_can_sleep()) {
        flusher = sched_create_kthread("bflush", flusher_main, NULL);   /* ধাপ ২৫খ: প্রথম লেখায় */
    }
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
                if (cache[i].dirty) {                /* ধাপ ২৫খ: আংশিক chunk-এর বাকিটা হারাবে না */
                    uint64_t lba;
                    uint32_t n = chunk_sectors(dev, cache[i].chunk, &lba);
                    if (dev->ops->write(dev, lba, n, cache[i].data) < 0) err = -EIO;
                    dev->writes++;
                }
                cache_drop(&cache[i]);
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
 * ২৪গ-এর পরীক্ষিত write-through-এ ফেরা হয়েছিল। (আটকানোর আসল কারণ ছিল অন্যায্য kmutex
 * আর usbd-এর না-পাওয়া — ধাপ ২৫ক-এ ঠিক।) ধাপ ২৫খ: জমানো এখন সবসময় (write-back cache),
 * তাই begin কিছু করে না, end সেই ডিস্কের জমা লেখা সাথে সাথে ডিস্কে পাঠায়। */
void block_batch_begin(struct block_device *dev)
{
    (void)dev;
}

int block_batch_end(struct block_device *dev)
{
    int r;
    while (dev && dev->parent) dev = dev->parent;
    kmutex_lock(&block_lock);
    r = flush_locked(dev);
    kmutex_unlock_yield(&block_lock);
    return r;
}

/* ধাপ ২৫খ: kmon usbstat/disks-এর জন্য */
void block_wb_get_stats(struct block_wb_stats *out)
{
    kmutex_lock(&block_lock);
    *out = wb;
    out->dirty_kib = dirty_count * CHUNK_SIZE / 1024;
    out->enabled = flusher != NULL;
    kmutex_unlock(&block_lock);
}

void block_wb_reset_stats(void)
{
    kmutex_lock(&block_lock);
    memset(&wb, 0, sizeof(wb));
    kmutex_unlock(&block_lock);
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
