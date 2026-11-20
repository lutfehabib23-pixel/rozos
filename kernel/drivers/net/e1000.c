/* rozOS — Intel e1000 / e1000e নেটওয়ার্ক কার্ড (ধাপ ৯)
 *
 * QEMU-র q35-এর ডিফল্ট কার্ড 82574L (8086:10D3, "e1000e"), i440fx-এর 82540EM
 * (8086:100E) — দুটোই "legacy" descriptor দিয়ে একইভাবে চলে।
 *
 * RX: 32টা descriptor-এর বৃত্ত, প্রতিটিতে 2 KiB বাফার। কার্ড ফ্রেম লিখে DD বিট
 *     বসায়; poll() সেটা কপি করে descriptor আবার কার্ডকে ফেরত দেয় (RDT)।
 * TX: 32টা descriptor, প্রতিটির নিজস্ব 2 KiB বাফার; transmit() ফ্রেম কপি করে TDT
 *     এগিয়ে দেয়। ইন্টারাপ্ট বন্ধ — netd থ্রেড poll করে (rozos/net.h দেখুন)।
 * সব DMA মেমোরি PMM থেকে (ফিজিক্যাল ঠিকানা কার্ডকে, HHDM দিয়ে CPU-তে)। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/net.h>
#include <rozos/pci.h>
#include <rozos/pmm.h>
#include <rozos/vmm.h>
#include <rozos/heap.h>
#include <rozos/string.h>
#include <rozos/printf.h>
#include <rozos/timer.h>

#define REG_CTRL     0x0000
#define REG_STATUS   0x0008
#define REG_ICR      0x00C0
#define REG_IMC      0x00D8
#define REG_RCTL     0x0100
#define REG_TCTL     0x0400
#define REG_TIPG     0x0410
#define REG_RDBAL    0x2800
#define REG_RDBAH    0x2804
#define REG_RDLEN    0x2808
#define REG_RDH      0x2810
#define REG_RDT      0x2818
#define REG_TDBAL    0x3800
#define REG_TDBAH    0x3804
#define REG_TDLEN    0x3808
#define REG_TDH      0x3810
#define REG_TDT      0x3818
#define REG_MTA      0x5200
#define REG_RAL      0x5400
#define REG_RAH      0x5404

#define CTRL_SLU     (1u << 6)            /* set link up */
#define CTRL_RST     (1u << 26)
#define STATUS_LU    (1u << 1)            /* link up */
#define RCTL_EN      (1u << 1)
#define RCTL_BAM     (1u << 15)           /* broadcast গ্রহণ */
#define RCTL_SECRC   (1u << 26)           /* CRC বাদ দিয়ে দেওয়া */
#define TCTL_EN      (1u << 1)
#define TCTL_PSP     (1u << 3)
#define TCTL_CT      (0x10u << 4)
#define TCTL_COLD    (0x40u << 12)
#define RAH_AV       (1u << 31)

#define TX_CMD_EOP   0x01
#define TX_CMD_IFCS  0x02
#define TX_CMD_RS    0x08
#define DESC_DD      0x01

#define NUM_RX       32
#define NUM_TX       32
#define BUF_SIZE     2048

struct rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed));

struct tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed));

typedef char rx_desc_size_check[(sizeof(struct rx_desc) == 16) ? 1 : -1];
typedef char tx_desc_size_check[(sizeof(struct tx_desc) == 16) ? 1 : -1];

struct e1000 {
    struct net_device dev;
    volatile uint8_t *regs;
    struct rx_desc *rx;                   /* HHDM ঠিকানা */
    struct tx_desc *tx;
    uint8_t *rx_buf[NUM_RX];
    uint8_t *tx_buf[NUM_TX];
    unsigned rx_next;                     /* পরের যে descriptor দেখব */
    unsigned tx_next;
};

static const uint16_t supported_ids[] = {
    0x100E,   /* 82540EM (QEMU "e1000") */
    0x100F,   /* 82545EM */
    0x10D3,   /* 82574L  (QEMU "e1000e", q35-এর ডিফল্ট) */
    0x153A,   /* I217-LM */
    0x10EA,   /* 82577LM */
};

static inline uint32_t rd(struct e1000 *e, uint32_t reg)
{
    return *(volatile uint32_t *)(e->regs + reg);
}

static inline void wr(struct e1000 *e, uint32_t reg, uint32_t val)
{
    *(volatile uint32_t *)(e->regs + reg) = val;
}

/* এক পেজ DMA মেমোরি: ফিজিক্যাল ঠিকানা ও HHDM পয়েন্টার */
static void *dma_page(uint64_t *phys)
{
    uint64_t p = pmm_alloc_zeroed_page();
    if (!p) {
        return NULL;
    }
    *phys = p;
    return pmm_phys_to_virt(p);
}

static int e1000_transmit(struct net_device *dev, const void *frame, size_t len)
{
    struct e1000 *e = (struct e1000 *)dev->priv;
    struct tx_desc *d = &e->tx[e->tx_next];
    uint64_t start;

    if (len > BUF_SIZE || len > NET_FRAME_MAX) {
        dev->tx_errors++;
        return -1;
    }
    /* আগের ব্যবহারের পর কার্ড এই descriptor শেষ করেছে কিনা (DD) — না হলে একটু অপেক্ষা */
    start = timer_uptime_ms();
    while (d->cmd && !(d->status & DESC_DD)) {
        if (timer_uptime_ms() - start > 50) {
            dev->tx_errors++;
            return -1;
        }
    }
    memcpy(e->tx_buf[e->tx_next], frame, len);
    d->length = (uint16_t)len;
    d->cso = 0;
    d->css = 0;
    d->special = 0;
    d->status = 0;
    d->cmd = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
    __asm__ __volatile__("" : : : "memory");
    e->tx_next = (e->tx_next + 1) % NUM_TX;
    wr(e, REG_TDT, e->tx_next);
    dev->tx_packets++;
    dev->tx_bytes += len;
    return 0;
}

static size_t e1000_poll(struct net_device *dev, void *buf, size_t max)
{
    struct e1000 *e = (struct e1000 *)dev->priv;
    for (;;) {
        struct rx_desc *d = &e->rx[e->rx_next];
        size_t len;
        unsigned idx;

        if (!(d->status & DESC_DD)) {
            return 0;
        }
        len = d->length;
        idx = e->rx_next;
        /* ভাঙা বা খণ্ডিত ফ্রেম (EOP নেই) — বাদ */
        if (d->errors || !(d->status & 0x02) || len > max) {
            dev->rx_dropped++;
            len = 0;
        } else {
            memcpy(buf, e->rx_buf[idx], len);
            dev->rx_packets++;
            dev->rx_bytes += len;
        }
        d->status = 0;
        e->rx_next = (idx + 1) % NUM_RX;
        wr(e, REG_RDT, idx);                 /* descriptor কার্ডকে ফেরত */
        if (len) {
            return len;
        }
    }
}

static int e1000_link(struct net_device *dev)
{
    struct e1000 *e = (struct e1000 *)dev->priv;
    return (rd(e, REG_STATUS) & STATUS_LU) != 0;
}

static int setup_card(struct e1000 *e)
{
    uint64_t rx_phys = 0, tx_phys = 0, buf_phys = 0;
    uint32_t ral, rah;
    unsigned i;
    uint64_t t0;

    /* রিসেট, তারপর সব ইন্টারাপ্ট বন্ধ */
    wr(e, REG_IMC, 0xFFFFFFFF);
    wr(e, REG_CTRL, rd(e, REG_CTRL) | CTRL_RST);
    t0 = timer_uptime_ms();
    while ((rd(e, REG_CTRL) & CTRL_RST) && timer_uptime_ms() - t0 < 100) {
    }
    wr(e, REG_IMC, 0xFFFFFFFF);
    (void)rd(e, REG_ICR);
    wr(e, REG_CTRL, rd(e, REG_CTRL) | CTRL_SLU);

    /* MAC: firmware/QEMU RAL0/RAH0-তে বসিয়ে রাখে */
    ral = rd(e, REG_RAL);
    rah = rd(e, REG_RAH);
    for (i = 0; i < 4; i++) e->dev.mac[i] = (uint8_t)(ral >> (8 * i));
    e->dev.mac[4] = (uint8_t)rah;
    e->dev.mac[5] = (uint8_t)(rah >> 8);
    wr(e, REG_RAH, rah | RAH_AV);
    for (i = 0; i < 128; i++) {
        wr(e, REG_MTA + i * 4, 0);           /* multicast টেবিল খালি */
    }

    /* descriptor বৃত্ত: প্রতিটা এক পেজে (32 × 16 = 512 বাইট) */
    e->rx = (struct rx_desc *)dma_page(&rx_phys);
    e->tx = (struct tx_desc *)dma_page(&tx_phys);
    if (!e->rx || !e->tx) {
        return 0;
    }
    /* বাফার: প্রতি পেজে দুটো 2 KiB */
    for (i = 0; i < NUM_RX; i += 2) {
        uint8_t *p = (uint8_t *)dma_page(&buf_phys);
        if (!p) return 0;
        e->rx_buf[i] = p;
        e->rx_buf[i + 1] = p + BUF_SIZE;
        e->rx[i].addr = buf_phys;
        e->rx[i + 1].addr = buf_phys + BUF_SIZE;
    }
    for (i = 0; i < NUM_TX; i += 2) {
        uint8_t *p = (uint8_t *)dma_page(&buf_phys);
        if (!p) return 0;
        e->tx_buf[i] = p;
        e->tx_buf[i + 1] = p + BUF_SIZE;
        e->tx[i].addr = buf_phys;
        e->tx[i + 1].addr = buf_phys + BUF_SIZE;
    }

    wr(e, REG_RDBAL, (uint32_t)rx_phys);
    wr(e, REG_RDBAH, (uint32_t)(rx_phys >> 32));
    wr(e, REG_RDLEN, NUM_RX * sizeof(struct rx_desc));
    wr(e, REG_RDH, 0);
    wr(e, REG_RDT, NUM_RX - 1);
    e->rx_next = 0;
    wr(e, REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);   /* BSIZE 00 = 2048 */

    wr(e, REG_TDBAL, (uint32_t)tx_phys);
    wr(e, REG_TDBAH, (uint32_t)(tx_phys >> 32));
    wr(e, REG_TDLEN, NUM_TX * sizeof(struct tx_desc));
    wr(e, REG_TDH, 0);
    wr(e, REG_TDT, 0);
    e->tx_next = 0;
    wr(e, REG_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD);
    wr(e, REG_TIPG, 0x0060200A);
    return 1;
}

int e1000_init(void)
{
    size_t i, n;
    int found = 0;

    for (i = 0; i < sizeof(supported_ids) / sizeof(supported_ids[0]); i++) {
        for (n = 0; ; n++) {
            struct pci_device *pd = pci_find_id(0x8086, supported_ids[i], n);
            struct e1000 *e;
            if (!pd) {
                break;
            }
            if (!pd->bar[0] || pd->bar_is_io[0]) {
                continue;
            }
            pci_enable_device(pd);
            e = (struct e1000 *)kzalloc(sizeof(*e));
            if (!e) {
                return found;
            }
            e->regs = (volatile uint8_t *)vmm_map_mmio(pd->bar[0], 0x20000);
            if (!e->regs || !setup_card(e)) {
                kprintf("[WARN] e1000 %04x: setup failed\n", supported_ids[i]);
                kfree(e);
                continue;
            }
            e->dev.driver = supported_ids[i] == 0x10D3 ? "e1000e (82574L)" : "e1000";
            e->dev.transmit = e1000_transmit;
            e->dev.poll = e1000_poll;
            e->dev.link_status = e1000_link;
            e->dev.priv = e;
            e->dev.link_up = e1000_link(&e->dev);
            if (net_register_device(&e->dev) == 0) {
                found++;
            }
        }
    }
    return found;
}
