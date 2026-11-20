/* rozOS — partition table পড়া: GPT ও MBR (ধাপ ১১খ)
 *
 * GPT (UEFI-র নিয়ম): LBA 1-এ হেডার ("EFI PART"), তাতে partition entry-র তালিকা কোথায়।
 * প্রতিটি entry-তে type GUID, শুরু/শেষ LBA আর UTF-16 নাম। MBR (পুরোনো BIOS-নিয়ম):
 * sector 0-এর শেষে চারটে ১৬-বাইট entry আর 0x55AA। MBR-এ type 0xEE মানে "আসলে GPT"
 * (protective MBR)। extended/logical partition এখনো নয়।
 *
 * পাওয়া প্রতিটি partition একটা block_device (sda1, sda2 ...) — ext2/FAT সেগুলো mount করে। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/block.h>
#include <rozos/heap.h>
#include <rozos/errno.h>
#include <rozos/string.h>
#include <rozos/printf.h>

#define GPT_MAX_ENTRIES 128

struct gpt_header {
    char     signature[8];                /* "EFI PART" */
    uint32_t revision, header_size, header_crc, reserved;
    uint64_t my_lba, alt_lba, first_usable, last_usable;
    uint8_t  disk_guid[16];
    uint64_t entries_lba;
    uint32_t entry_count, entry_size, entries_crc;
} __attribute__((packed));

struct gpt_entry {
    uint8_t  type_guid[16];
    uint8_t  part_guid[16];
    uint64_t first_lba, last_lba, attrs;
    uint16_t name[36];                    /* UTF-16LE */
} __attribute__((packed));

struct mbr_entry {
    uint8_t  status, chs_first[3], type, chs_last[3];
    uint32_t lba_first, sectors;
} __attribute__((packed));

typedef char gpt_entry_size_check[(sizeof(struct gpt_entry) == 128) ? 1 : -1];
typedef char mbr_entry_size_check[(sizeof(struct mbr_entry) == 16) ? 1 : -1];

/* GUID-এর ডিস্কের বাইট-ক্রম (প্রথম তিন অংশ little-endian) */
static const struct { uint8_t guid[16]; const char *name; } gpt_types[] = {
    { { 0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B }, "EFI System" },
    { { 0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47, 0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4 }, "Linux data" },
    { { 0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44, 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7 }, "Microsoft basic data" },
    { { 0x48, 0x61, 0x68, 0x21, 0x49, 0x64, 0x6F, 0x6E, 0x74, 0x4E, 0x65, 0x65, 0x64, 0x45, 0x46, 0x49 }, "BIOS boot" },
    { { 0x6D, 0xFD, 0x57, 0x06, 0xAB, 0xA4, 0xC4, 0x43, 0x84, 0xE5, 0x09, 0x33, 0xC8, 0x4B, 0x4F, 0x4F }, "Linux swap" },
};

static void copy_str(char *dst, size_t size, const char *src)
{
    size_t n = strlen(src);
    if (n >= size) n = size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static const char *mbr_type_name(uint8_t t)
{
    switch (t) {
    case 0x01: return "FAT12";
    case 0x04: case 0x06: case 0x0E: return "FAT16";
    case 0x0B: case 0x0C: return "FAT32";
    case 0x07: return "NTFS/exFAT";
    case 0x82: return "Linux swap";
    case 0x83: return "Linux";
    case 0xEF: return "EFI System";
    case 0x05: case 0x0F: return "Extended";
    }
    return "unknown";
}

static int add_partition(struct block_device *disk, uint32_t num, uint64_t start, uint64_t count,
                         const char *type, const char *label)
{
    struct block_device *p;
    int err;
    if (count == 0 || start >= disk->sector_count || start + count > disk->sector_count) {
        return -EINVAL;
    }
    p = (struct block_device *)kzalloc(sizeof(*p));
    if (!p) return -ENOMEM;
    p->parent = disk;
    p->start_lba = start;
    p->sector_count = count;
    p->part_number = num;
    copy_str(p->part_type, sizeof(p->part_type), type);
    copy_str(p->part_label, sizeof(p->part_label), label ? label : "");
    err = block_register_partition(p);
    if (err < 0) {
        kfree(p);
        return err;
    }
    disk->partitions++;
    return 0;
}

static int scan_gpt(struct block_device *disk)
{
    struct gpt_header h;
    struct gpt_entry *e;
    uint32_t i, n, found = 0;
    size_t bytes;

    if (block_read_bytes(disk, (uint64_t)disk->sector_size, &h, sizeof(h)) < 0 ||
        memcmp(h.signature, "EFI PART", 8) != 0) {
        return -1;
    }
    if (h.entry_size < sizeof(struct gpt_entry) || h.entry_size > 1024 || h.entry_count == 0) {
        return -1;
    }
    n = h.entry_count > GPT_MAX_ENTRIES ? GPT_MAX_ENTRIES : h.entry_count;
    bytes = (size_t)n * h.entry_size;
    e = (struct gpt_entry *)kmalloc(bytes);
    if (!e) return -1;
    if (block_read_bytes(disk, h.entries_lba * disk->sector_size, e, bytes) < 0) {
        kfree(e);
        return -1;
    }
    copy_str(disk->table, sizeof(disk->table), "gpt");
    for (i = 0; i < n; i++) {
        const struct gpt_entry *pe = (const struct gpt_entry *)((const uint8_t *)e + (size_t)i * h.entry_size);
        static const uint8_t zero[16];
        const char *type = "unknown";
        char label[40];
        size_t k, j;
        if (!memcmp(pe->type_guid, zero, 16) || pe->last_lba < pe->first_lba) {
            continue;                                     /* খালি entry */
        }
        for (k = 0; k < sizeof(gpt_types) / sizeof(gpt_types[0]); k++) {
            if (!memcmp(pe->type_guid, gpt_types[k].guid, 16)) {
                type = gpt_types[k].name;
            }
        }
        for (j = 0; j < 36 && j + 1 < sizeof(label) && pe->name[j]; j++) {
            label[j] = pe->name[j] < 0x80 ? (char)pe->name[j] : '?';
        }
        label[j] = '\0';
        if (add_partition(disk, i + 1, pe->first_lba, pe->last_lba - pe->first_lba + 1, type, label) == 0) {
            found++;
        }
    }
    kfree(e);
    return (int)found;
}

static int scan_mbr(struct block_device *disk)
{
    uint8_t sec[512];
    const struct mbr_entry *m;
    int i, found = 0;

    if (block_read_bytes(disk, 0, sec, sizeof(sec)) < 0 || sec[510] != 0x55 || sec[511] != 0xAA) {
        return -1;
    }
    m = (const struct mbr_entry *)(sec + 446);
    /* ext2/FAT সরাসরি পুরো ডিস্কে থাকলেও শেষে 0x55AA থাকতে পারে (FAT-এর boot sector) —
     * তাই entry-গুলো যুক্তিসঙ্গত কিনা দেখা: status 0 বা 0x80, শুরু > 0 */
    for (i = 0; i < 4; i++) {
        if (m[i].type == 0) continue;
        if ((m[i].status != 0 && m[i].status != 0x80) || m[i].lba_first == 0) {
            return -1;
        }
    }
    copy_str(disk->table, sizeof(disk->table), "mbr");
    for (i = 0; i < 4; i++) {
        if (m[i].type == 0 || m[i].type == 0xEE || m[i].type == 0x05 || m[i].type == 0x0F) {
            continue;
        }
        if (add_partition(disk, (uint32_t)i + 1, m[i].lba_first, m[i].sectors, mbr_type_name(m[i].type), NULL) == 0) {
            found++;
        }
    }
    return found;
}

int partition_scan(struct block_device *disk)
{
    int n;
    if (!disk || disk->parent) {
        return 0;
    }
    disk->table[0] = '\0';
    disk->partitions = 0;
    n = scan_gpt(disk);
    if (n < 0) {
        n = scan_mbr(disk);
    }
    return n < 0 ? 0 : n;
}
