/* rozOS — AHCI ড্রাইভার (rozos/ahci.h দেখুন)
 *
 * মেমোরি বিন্যাস (প্রতিটি পোর্ট, সব PMM পেজ, 64-বিট সাপোর্ট না থাকলে 4 GiB-এর নিচে):
 *   পেজ ১: command list (32 × 32 B = 1 KiB) + received-FIS এলাকা (256 B, offset 1 KiB)
 *   পেজ ২: command table (slot 0) — 64 B CFIS + 16 B ACMD + reserved + PRDT
 *   ১৬ পেজ: DMA bounce buffer (64 KiB) — এক কমান্ডে সর্বোচ্চ এতটুকু
 * আমরা শুধু slot 0 ব্যবহার করি (একবারে এক কমান্ড) — সরল এবং নির্ভরযোগ্য। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/ahci.h>
#include <rozos/block.h>
#include <rozos/pci.h>
#include <rozos/pmm.h>
#include <rozos/vmm.h>
#include <rozos/heap.h>
#include <rozos/timer.h>
#include <rozos/errno.h>
#include <rozos/string.h>
#include <rozos/compiler.h>
#include <arch/x86_64/cpu.h>

/* HBA রেজিস্টার */
#define HBA_CAP    0x00
#define HBA_GHC    0x04
#define HBA_IS     0x08
#define HBA_PI     0x0C
#define HBA_VS     0x10
#define HBA_CAP2   0x24
#define HBA_BOHC   0x28

#define CAP_S64A       (1u << 31)
#define GHC_AE         (1u << 31)
#define GHC_IE         (1u << 1)
#define CAP2_BOH       (1u << 0)
#define BOHC_BOS       (1u << 0)
#define BOHC_OOS       (1u << 1)

/* পোর্ট রেজিস্টার (0x100 + port × 0x80) */
#define PORT_CLB   0x00
#define PORT_CLBU  0x04
#define PORT_FB    0x08
#define PORT_FBU   0x0C
#define PORT_IS    0x10
#define PORT_IE    0x14
#define PORT_CMD   0x18
#define PORT_TFD   0x20
#define PORT_SIG   0x24
#define PORT_SSTS  0x28
#define PORT_SERR  0x30
#define PORT_CI    0x38

#define CMD_ST     (1u << 0)
#define CMD_SUD    (1u << 1)
#define CMD_POD    (1u << 2)
#define CMD_FRE    (1u << 4)
#define CMD_FR     (1u << 14)
#define CMD_CR     (1u << 15)

#define TFD_ERR    (1u << 0)
#define TFD_DRQ    (1u << 3)
#define TFD_BSY    (1u << 7)
#define IS_TFES    (1u << 30)

#define SIG_ATA    0x00000101u
#define SSTS_DET_PRESENT 3
#define SSTS_IPM_ACTIVE  1

/* ATA কমান্ড */
#define ATA_CMD_READ_DMA_EXT   0x25
#define ATA_CMD_WRITE_DMA_EXT  0x35
#define ATA_CMD_FLUSH_EXT      0xEA
#define ATA_CMD_IDENTIFY       0xEC

#define FIS_TYPE_REG_H2D 0x27

#define BOUNCE_PAGES   16
#define BOUNCE_BYTES   (BOUNCE_PAGES * 4096u)
#define CMD_TIMEOUT_MS 5000

struct hba_cmd_header {
    uint16_t flags;          /* বিট 0-4 CFL (dword), 6 = write */
    uint16_t prdtl;          /* PRDT এন্ট্রির সংখ্যা */
    uint32_t prdbc;          /* হস্তান্তরিত বাইট (HBA লেখে) */
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t reserved[4];
} ROZ_PACKED;

struct hba_prdt_entry {
    uint32_t dba;
    uint32_t dbau;
    uint32_t reserved;
    uint32_t dbc;            /* বিট 0-21 = বাইট সংখ্যা - 1, বিট 31 = interrupt */
} ROZ_PACKED;

struct ahci_port {
    struct block_device bdev;
    volatile uint8_t *regs;
    unsigned index;
    struct hba_cmd_header *cmd_list;
    uint64_t cmd_list_phys;
    uint8_t *cmd_table;
    uint64_t cmd_table_phys;
    uint8_t *bounce;
    uint64_t bounce_phys;
};

static volatile uint8_t *hba;
static struct ahci_info info;

static inline uint32_t hba_read(uint32_t reg)          { return *(volatile uint32_t *)(hba + reg); }
static inline void hba_write(uint32_t reg, uint32_t v) { *(volatile uint32_t *)(hba + reg) = v; }
static inline uint32_t port_read(struct ahci_port *p, uint32_t reg)          { return *(volatile uint32_t *)(p->regs + reg); }
static inline void port_write(struct ahci_port *p, uint32_t reg, uint32_t v) { *(volatile uint32_t *)(p->regs + reg) = v; }

/* cond-এর বিটগুলো mask অনুযায়ী মান পাওয়া পর্যন্ত অপেক্ষা; সফল হলে 1 */
static int wait_reg(struct ahci_port *p, uint32_t reg, uint32_t mask, uint32_t value, uint64_t ms)
{
    uint64_t end = timer_uptime_ms() + ms;
    while ((port_read(p, reg) & mask) != value) {
        if (timer_uptime_ms() > end) {
            return 0;
        }
        cpu_pause();
    }
    return 1;
}

static void stop_port(struct ahci_port *p)
{
    port_write(p, PORT_CMD, port_read(p, PORT_CMD) & ~CMD_ST);
    wait_reg(p, PORT_CMD, CMD_CR, 0, 500);
    port_write(p, PORT_CMD, port_read(p, PORT_CMD) & ~CMD_FRE);
    wait_reg(p, PORT_CMD, CMD_FR, 0, 500);
}

static void start_port(struct ahci_port *p)
{
    wait_reg(p, PORT_CMD, CMD_CR, 0, 500);
    port_write(p, PORT_CMD, port_read(p, PORT_CMD) | CMD_FRE);
    port_write(p, PORT_CMD, port_read(p, PORT_CMD) | CMD_ST);
}

/* একটা ATA কমান্ড (slot 0) চালিয়ে শেষ হওয়া পর্যন্ত অপেক্ষা।
 * bytes > 0 হলে bounce buffer দিয়ে ডেটা যায় (write = 1 হলে ডিস্কে)। */
static int issue(struct ahci_port *p, uint8_t command, uint64_t lba, uint16_t count,
                 uint32_t bytes, int write)
{
    struct hba_cmd_header *h = &p->cmd_list[0];
    uint8_t *fis = p->cmd_table;
    struct hba_prdt_entry *prd = (struct hba_prdt_entry *)(p->cmd_table + 0x80);
    uint64_t end;

    if (!wait_reg(p, PORT_TFD, TFD_BSY | TFD_DRQ, 0, 1000)) {
        return -EIO;
    }

    memset(p->cmd_table, 0, 0x80 + sizeof(*prd));
    h->flags = (uint16_t)(5 | (write ? (1u << 6) : 0));   /* H2D FIS = 5 dword */
    h->prdtl = bytes ? 1 : 0;
    h->prdbc = 0;
    if (bytes) {
        prd->dba  = (uint32_t)p->bounce_phys;
        prd->dbau = (uint32_t)(p->bounce_phys >> 32);
        prd->dbc  = bytes - 1;
    }

    fis[0] = FIS_TYPE_REG_H2D;
    fis[1] = 0x80;                                        /* C = কমান্ড */
    fis[2] = command;
    fis[4] = (uint8_t)lba;
    fis[5] = (uint8_t)(lba >> 8);
    fis[6] = (uint8_t)(lba >> 16);
    fis[7] = 1u << 6;                                     /* LBA মোড */
    fis[8] = (uint8_t)(lba >> 24);
    fis[9] = (uint8_t)(lba >> 32);
    fis[10] = (uint8_t)(lba >> 40);
    fis[12] = (uint8_t)count;
    fis[13] = (uint8_t)(count >> 8);

    port_write(p, PORT_IS, 0xFFFFFFFFu);                  /* পুরনো স্ট্যাটাস মোছা */
    port_write(p, PORT_CI, 1);

    end = timer_uptime_ms() + CMD_TIMEOUT_MS;
    for (;;) {
        if (!(port_read(p, PORT_CI) & 1)) {
            break;
        }
        if (port_read(p, PORT_IS) & IS_TFES) {
            return -EIO;
        }
        if (timer_uptime_ms() > end) {
            return -EIO;
        }
        cpu_pause();
    }
    if ((port_read(p, PORT_IS) & IS_TFES) || (port_read(p, PORT_TFD) & TFD_ERR)) {
        return -EIO;
    }
    return 0;
}

/* ---------- block_ops ---------- */

static int ahci_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buf)
{
    struct ahci_port *p = (struct ahci_port *)dev->driver_data;
    uint8_t *out = (uint8_t *)buf;
    uint32_t per = BOUNCE_BYTES / dev->sector_size;

    while (count) {
        uint32_t n = count > per ? per : count;
        int err = issue(p, ATA_CMD_READ_DMA_EXT, lba, (uint16_t)n, n * dev->sector_size, 0);
        if (err < 0) {
            return err;
        }
        memcpy(out, p->bounce, (size_t)n * dev->sector_size);
        out += (size_t)n * dev->sector_size;
        lba += n;
        count -= n;
    }
    return 0;
}

static int ahci_write(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf)
{
    struct ahci_port *p = (struct ahci_port *)dev->driver_data;
    const uint8_t *in = (const uint8_t *)buf;
    uint32_t per = BOUNCE_BYTES / dev->sector_size;

    while (count) {
        uint32_t n = count > per ? per : count;
        int err;
        memcpy(p->bounce, in, (size_t)n * dev->sector_size);
        err = issue(p, ATA_CMD_WRITE_DMA_EXT, lba, (uint16_t)n, n * dev->sector_size, 1);
        if (err < 0) {
            return err;
        }
        in += (size_t)n * dev->sector_size;
        lba += n;
        count -= n;
    }
    return 0;
}

static int ahci_flush(struct block_device *dev)
{
    return issue((struct ahci_port *)dev->driver_data, ATA_CMD_FLUSH_EXT, 0, 0, 0, 0);
}

static const struct block_ops ahci_ops = { ahci_read, ahci_write, ahci_flush };

/* ---------- চালু করা ---------- */

static uint64_t dma_pages(uint64_t count)
{
    /* 64-বিট ঠিকানা না বুঝলে DMA মেমোরি 4 GiB-এর নিচে লাগবে */
    uint64_t phys = pmm_alloc_contiguous(count, info.supports_64bit ? 0 : 0x100000000ULL);
    if (phys) {
        memset(pmm_phys_to_virt(phys), 0, count * 4096);
    }
    return phys;
}

/* IDENTIFY-র ATA স্ট্রিং (প্রতি word-এ বাইট উল্টানো) থেকে মডেলের নাম */
static void ata_string(char *dst, const uint16_t *words, unsigned count)
{
    unsigned i;
    int n;
    for (i = 0; i < count; i++) {
        dst[i * 2]     = (char)(words[i] >> 8);
        dst[i * 2 + 1] = (char)(words[i] & 0xFF);
    }
    dst[count * 2] = '\0';
    for (n = (int)(count * 2) - 1; n >= 0 && dst[n] == ' '; n--) {
        dst[n] = '\0';
    }
}

static int setup_port(unsigned index)
{
    struct ahci_port *p;
    const uint16_t *id;
    uint32_t ssts;
    uint64_t sectors;

    p = (struct ahci_port *)kzalloc(sizeof(*p));
    if (!p) {
        return 0;
    }
    p->index = index;
    p->regs = hba + 0x100 + index * 0x80;

    ssts = port_read(p, PORT_SSTS);
    if ((ssts & 0xF) != SSTS_DET_PRESENT || ((ssts >> 8) & 0xF) != SSTS_IPM_ACTIVE ||
        port_read(p, PORT_SIG) != SIG_ATA) {
        kfree(p);
        return 0;                                         /* ডিস্ক নেই বা ATAPI (CD) */
    }

    stop_port(p);
    p->cmd_list_phys = dma_pages(1);
    p->cmd_table_phys = dma_pages(1);
    p->bounce_phys = dma_pages(BOUNCE_PAGES);
    if (!p->cmd_list_phys || !p->cmd_table_phys || !p->bounce_phys) {
        kfree(p);
        return 0;
    }
    p->cmd_list = (struct hba_cmd_header *)pmm_phys_to_virt(p->cmd_list_phys);
    p->cmd_table = (uint8_t *)pmm_phys_to_virt(p->cmd_table_phys);
    p->bounce = (uint8_t *)pmm_phys_to_virt(p->bounce_phys);

    port_write(p, PORT_CLB,  (uint32_t)p->cmd_list_phys);
    port_write(p, PORT_CLBU, (uint32_t)(p->cmd_list_phys >> 32));
    port_write(p, PORT_FB,   (uint32_t)(p->cmd_list_phys + 1024));
    port_write(p, PORT_FBU,  (uint32_t)((p->cmd_list_phys + 1024) >> 32));
    p->cmd_list[0].ctba  = (uint32_t)p->cmd_table_phys;
    p->cmd_list[0].ctbau = (uint32_t)(p->cmd_table_phys >> 32);

    port_write(p, PORT_SERR, 0xFFFFFFFFu);
    port_write(p, PORT_IS, 0xFFFFFFFFu);
    port_write(p, PORT_IE, 0);                            /* polling */
    start_port(p);

    /* IDENTIFY: 512 বাইট তথ্য */
    if (issue(p, ATA_CMD_IDENTIFY, 0, 0, 512, 0) < 0) {
        stop_port(p);
        return 0;
    }
    id = (const uint16_t *)p->bounce;
    if (!(id[83] & (1u << 10))) {
        stop_port(p);
        return 0;                                         /* LBA48 নেই — খুব পুরনো ডিস্ক */
    }
    sectors = (uint64_t)id[100] | ((uint64_t)id[101] << 16) |
              ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);

    p->bdev.sector_size = 512;
    if ((id[106] & 0xC000) == 0x4000 && (id[106] & (1u << 12))) {
        p->bdev.sector_size = 2u * ((uint32_t)id[117] | ((uint32_t)id[118] << 16));
    }
    p->bdev.sector_count = sectors;
    p->bdev.ops = &ahci_ops;
    p->bdev.driver_data = p;
    ata_string(p->bdev.model, id + 27, 20);

    if (block_register(&p->bdev, "sd") < 0) {
        stop_port(p);
        return 0;
    }
    return 1;
}

int ahci_init(void)
{
    struct pci_device *d = pci_find_class(0x01, 0x06, 0x01, 0);
    uint32_t cap, pi;
    unsigned i;

    memset(&info, 0, sizeof(info));
    if (!d || !d->bar[5]) {
        return 0;
    }
    pci_enable_device(d);
    hba = (volatile uint8_t *)vmm_map_mmio(d->bar[5], 0x1100);
    if (!hba) {
        return 0;
    }

    /* ফার্মওয়্যারের কাছ থেকে নিয়ন্ত্রণ নেওয়া (BIOS/OS handoff) */
    if (hba_read(HBA_CAP2) & CAP2_BOH) {
        uint64_t end = timer_uptime_ms() + 1000;
        hba_write(HBA_BOHC, hba_read(HBA_BOHC) | BOHC_OOS);
        while ((hba_read(HBA_BOHC) & BOHC_BOS) && timer_uptime_ms() < end) {
            cpu_pause();
        }
    }
    hba_write(HBA_GHC, (hba_read(HBA_GHC) | GHC_AE) & ~GHC_IE);

    cap = hba_read(HBA_CAP);
    pi = hba_read(HBA_PI);
    info.present = 1;
    info.version = hba_read(HBA_VS);
    info.ports_implemented = pi;
    info.command_slots = ((cap >> 8) & 0x1F) + 1;
    info.supports_64bit = (cap & CAP_S64A) != 0;

    for (i = 0; i < AHCI_MAX_PORTS; i++) {
        if (pi & (1u << i)) {
            info.disks += setup_port(i);
        }
    }
    return info.disks;
}

void ahci_get_info(struct ahci_info *out)
{
    *out = info;
}
