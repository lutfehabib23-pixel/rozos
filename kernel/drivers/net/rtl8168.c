/* rozOS — Realtek RTL8111/8168 গিগাবিট নেটওয়ার্ক কার্ড (ধাপ ২২)
 *
 * লক্ষ্য: আসল PC-র বোর্ডের চিপ (Gigabyte B550M DS3H R2: PCI 10ec:8168 rev 0x15 =
 * RTL8111H/8168H)। QEMU এই চিপ চালাতে পারে না, তাই প্রতিটা ধাপের ফল `log`-এ রাখা হয়
 * আর kmon-এর `nic` কমান্ড তা দেখায় (বুটের বার্তা ডেস্কটপের নিচে চাপা পড়ে)।
 *
 * পদ্ধতি Linux-এর r8169 ড্রাইভারের মূল পথ অনুসরণ করে (rtl_hw_start → 8168H):
 *   PCI D0 + ASPM বন্ধ → TxConfig থেকে XID → ChipCmd রিসেট → Cfg9346 খোলা →
 *   ASPM/CLKREQ বন্ধ (Config2/5) → CPlusCmd → (8168G+) FIFO মাপ, packet filter রিসেট,
 *   RXDV gate বন্ধ → RxMaxSize, descriptor ঠিকানা → Cfg9346 বন্ধ → TE/RE চালু →
 *   RxConfig, TxConfig, rx mode → ইন্টারাপ্ট mask (netd poll করে, e1000-এর মতো)।
 *
 * Descriptor (১৬ বাইট): opts1 (OWN 31, EOR 30, FS 29, LS 28, দৈর্ঘ্য 0-13), opts2,
 * ৬৪-বিট বাফার ঠিকানা। চিপ ফ্রেম লিখে OWN মুছে দেয়; আমরা কপি করে OWN আবার বসাই।
 * সব DMA মেমোরি 4 GiB-এর নিচে (pmm_alloc_contiguous), যাতে পুরোনো চিপেও চলে। */

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <rozos/net.h>
#include <rozos/pci.h>
#include <rozos/pmm.h>
#include <rozos/vmm.h>
#include <rozos/heap.h>
#include <rozos/string.h>
#include <rozos/printf.h>
#include <rozos/timer.h>
#include <arch/x86_64/cpu.h>

/* ---------- রেজিস্টার ---------- */
#define R_MAC0          0x00
#define R_MAR0          0x08
#define R_COUNTER_LO    0x10               /* ধাপ ২২গ: tally counter */
#define R_COUNTER_HI    0x14
#define COUNTER_DUMP    0x08
#define R_TXDESC_LO     0x20
#define R_TXDESC_HI     0x24
#define R_CHIPCMD       0x37
#define R_TXPOLL        0x38
#define R_INTRMASK      0x3C
#define R_INTRSTATUS    0x3E
#define R_TXCONFIG      0x40
#define R_RXCONFIG      0x44
#define R_CFG9346       0x50
#define R_CONFIG2       0x53
#define R_CONFIG5       0x56
#define R_PHYAR         0x60
#define R_PHYSTATUS     0x6C
#define R_PMCH          0x6F
#define R_ERIDR         0x70
#define R_ERIAR         0x74
#define R_OCPDR         0xB0               /* MAC OCP (ধাপ ২২খ) */
#define R_GPHY_OCP      0xB8
#define R_DLLPR         0xD0
#define R_RXMAXSIZE     0xDA
#define R_CPLUSCMD      0xE0
#define R_INTRMITIGATE  0xE2
#define R_RXDESC_LO     0xE4
#define R_RXDESC_HI     0xE8
#define R_MAXTXPKT      0xEC
#define R_MISC          0xF0
#define R_MISC_1        0xF2

#define CMD_RESET       0x10
#define CMD_RX_EN       0x08
#define CMD_TX_EN       0x04
#define TXPOLL_NPQ      0x40
#define CFG9346_UNLOCK  0xC0
#define CFG9346_LOCK    0x00
#define CONFIG2_CLKREQ  0x80
#define CONFIG5_ASPM    0x01
#define MISC_RXDV_GATE  (1u << 19)
#define DLLPR_PFM_EN    0x40               /* ধাপ ২২খ: power saving বন্ধ করতে */
#define DLLPR_TX_10M_PS 0x80
#define MISC1_PFM_D3COLD 0x40

/* IntrStatus */
#define IS_RX_OK        0x0001
#define IS_RX_DESC_UNAV 0x0010             /* 8168-এ "RxOverflow" = descriptor নেই */

#define PHY_TBI         0x80
#define PHY_1000F       0x10
#define PHY_100         0x08
#define PHY_10          0x04
#define PHY_LINK        0x02
#define PHY_FULL        0x01

/* RxConfig */
#define RX_ACCEPT_PHYS  0x02
#define RX_ACCEPT_MULTI 0x04
#define RX_ACCEPT_BCAST 0x08
#define RX_ACCEPT_MASK  0x3F
#define RX_DMA_BURST    (7u << 8)         /* সীমাহীন */
#define RX_EARLY_OFF    (1u << 11)
#define RX_MULTI_EN     (1u << 14)
#define RX_128_INT_EN   (1u << 15)
#define RX_FIFO_THRESH  (7u << 13)        /* পুরোনো চিপ: পুরো ফ্রেম এলে DMA */

/* TxConfig */
#define TX_DMA_BURST    (7u << 8)
#define TX_IFG          (3u << 24)
#define TX_AUTO_FIFO    (1u << 7)

/* CPlusCmd */
#define CP_RXCHKSUM     0x0020
#define CP_RXVLAN       0x0040

/* ERI (চিপের ভেতরের রেজিস্টার) */
#define ERIAR_FLAG      0x80000000u
#define ERIAR_WRITE     0x80000000u
#define ERIAR_MASK_0011 (0x3u << 12)
#define ERIAR_MASK_1111 (0xFu << 12)

/* descriptor বিট */
#define D_OWN           (1u << 31)
#define D_EOR           (1u << 30)
#define D_FS            (1u << 29)
#define D_LS            (1u << 28)
#define D_RX_RES        (1u << 21)        /* ফ্রেমে ভুল */
#define D_LEN_MASK      0x3FFFu

/* MII */
#define MII_BMCR        0
#define MII_BMSR        1
#define MII_ANAR        4
#define MII_GBCR        9
#define ANAR_ALL        0x05E1             /* 10/100 half+full, pause, 802.3 */
#define GBCR_1000F      0x0200
#define BMCR_ANENABLE   0x1000
#define BMCR_ANRESTART  0x0200
#define BMCR_PDOWN      0x0800
#define BMSR_LINK       0x0004
#define BMSR_ANDONE     0x0020

#define NUM_RX          256
#define NUM_TX          64
#define BUF_SIZE        2048
#define LOG_SIZE        2048

struct rtl_desc {
    uint32_t opts1;
    uint32_t opts2;
    uint64_t addr;
};
typedef char rtl_desc_size_check[(sizeof(struct rtl_desc) == 16) ? 1 : -1];

/* ধাপ ২২গ: চিপের tally counter (Linux struct rtl8169_counters), ৬৪-বাইট aligned */
struct rtl_tally {
    uint64_t tx_packets;
    uint64_t rx_packets;
    uint64_t tx_errors;
    uint32_t rx_errors;
    uint16_t rx_missed;
    uint16_t align_errors;
    uint32_t tx_one_collision;
    uint32_t tx_multi_collision;
    uint64_t rx_unicast;
    uint64_t rx_broadcast;
    uint32_t rx_multicast;
    uint16_t tx_aborted;
    uint16_t tx_underrun;
} __attribute__((packed));

struct rtl {
    struct net_device dev;
    struct pci_device *pci;
    volatile uint8_t *regs;
    uint32_t xid;                         /* TxConfig[31:20] থেকে চিপের সংস্করণ */
    int modern;                           /* 8168G বা নতুন (RXDV gate, OCP PHY) */
    const char *chip;
    struct rtl_desc *rx, *tx;             /* HHDM ঠিকানা */
    uint64_t rx_phys, tx_phys;
    uint8_t *rx_buf, *tx_buf;             /* পাশাপাশি বাফার */
    uint64_t rx_buf_phys, tx_buf_phys;
    unsigned rx_next, tx_next;
    uint64_t link_check_ms;               /* শেষ autoneg চেষ্টার সময় */
    int an_restarts;
    uint64_t rx_errors;
    uint64_t rx_zero_late;                /* ধাপ ২৪ক: হেডার শূন্য পেয়ে আবার পড়তে হয়েছে */
    uint64_t rx_zero_drop;                /* ধাপ ২৪ক: অপেক্ষার পরেও শূন্য — ফেলে দেওয়া */
    uint16_t last_istatus;
    uint16_t istatus_seen;                /* ধাপ ২২খ: চালুর পর থেকে সব বিটের OR */
    int rx_restarts;
    /* ধাপ ২২গ: চিপের নিজের গণনা (tally) আর RX আটকে যাওয়া ধরা */
    struct rtl_tally *tally;
    uint64_t tally_phys;
    uint64_t wd_ms;                       /* শেষ watchdog */
    uint64_t wd_chip_rx, wd_our_rx;       /* তখনকার গণনা */
    int stall_restarts;
    uint32_t rxcfg;                       /* RxConfig-এর মান (আবার চালুর সময় লেখা) */
    char log[LOG_SIZE];
    size_t log_len;
};

/* ---------- MMIO ---------- */
static inline uint8_t  r8(struct rtl *t, uint32_t r)  { return *(volatile uint8_t  *)(t->regs + r); }
static inline uint16_t r16(struct rtl *t, uint32_t r) { return *(volatile uint16_t *)(t->regs + r); }
static inline uint32_t r32(struct rtl *t, uint32_t r) { return *(volatile uint32_t *)(t->regs + r); }
static inline void w8(struct rtl *t, uint32_t r, uint8_t v)   { *(volatile uint8_t  *)(t->regs + r) = v; }
static inline void w16(struct rtl *t, uint32_t r, uint16_t v) { *(volatile uint16_t *)(t->regs + r) = v; }
static inline void w32(struct rtl *t, uint32_t r, uint32_t v) { *(volatile uint32_t *)(t->regs + r) = v; }

static inline void barrier(void) { __asm__ __volatile__("" : : : "memory"); }

/* লগ: বুটে kprintf-ও হয়, আর `nic` পরে পুরোটা দেখায় */
static void tlog(struct rtl *t, const char *fmt, ...)
{
    char line[160];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = kvsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if ((size_t)n >= sizeof(line)) {
        n = (int)sizeof(line) - 1;
    }
    kprintf("[rtl8168] %s\n", line);
    if (t->log_len + (size_t)n + 2 < LOG_SIZE) {
        memcpy(t->log + t->log_len, line, (size_t)n);
        t->log_len += (size_t)n;
        t->log[t->log_len++] = '\n';
        t->log[t->log_len] = '\0';
    }
}

/* রেজিস্টারের বিট বসা (set=1) বা মোছা (set=0) পর্যন্ত অপেক্ষা, সর্বোচ্চ ms; 1 = হয়েছে */
static int wait32(struct rtl *t, uint32_t reg, uint32_t mask, int set, unsigned ms)
{
    uint64_t t0 = timer_uptime_ms();
    for (;;) {
        if (((r32(t, reg) & mask) != 0) == (set != 0)) return 1;
        if (timer_uptime_ms() - t0 > ms) return 0;
        cpu_pause();
    }
}

static int wait8(struct rtl *t, uint32_t reg, uint8_t mask, int set, unsigned ms)
{
    uint64_t t0 = timer_uptime_ms();
    for (;;) {
        if (((r8(t, reg) & mask) != 0) == (set != 0)) return 1;
        if (timer_uptime_ms() - t0 > ms) return 0;
        cpu_pause();
    }
}

/* ---------- ERI ---------- */
static void eri_write(struct rtl *t, uint32_t addr, uint32_t mask, uint32_t val)
{
    w32(t, R_ERIDR, val);
    w32(t, R_ERIAR, ERIAR_WRITE | mask | addr);
    if (!wait32(t, R_ERIAR, ERIAR_FLAG, 0, 10)) {
        tlog(t, "ERI write 0x%03x timed out", addr);
    }
}

static uint32_t eri_read(struct rtl *t, uint32_t addr)
{
    w32(t, R_ERIAR, ERIAR_MASK_1111 | addr);
    if (!wait32(t, R_ERIAR, ERIAR_FLAG, 1, 10)) {
        tlog(t, "ERI read 0x%03x timed out", addr);
        return 0;
    }
    return r32(t, R_ERIDR);
}

/* ---------- MAC OCP (8168G+; ধাপ ২২খ) ---------- */
static uint16_t mac_ocp_read(struct rtl *t, uint32_t reg)
{
    w32(t, R_OCPDR, reg << 15);
    return (uint16_t)r32(t, R_OCPDR);
}

static void mac_ocp_write(struct rtl *t, uint32_t reg, uint16_t val)
{
    w32(t, R_OCPDR, 0x80000000u | (reg << 15) | val);
}

static void mac_ocp_modify(struct rtl *t, uint32_t reg, uint16_t mask, uint16_t set)
{
    mac_ocp_write(t, reg, (uint16_t)((mac_ocp_read(t, reg) & ~mask) | set));
}

/* ---------- PHY (MII) ---------- */
/* 8168G থেকে PHY OCP দিয়ে (GPHY_OCP, ঠিকানা 0xa400 + reg*2); পুরোনোয় PHYAR */
static int phy_read(struct rtl *t, unsigned reg)
{
    if (t->modern) {
        uint32_t a = 0xA400u + reg * 2;
        w32(t, R_GPHY_OCP, a << 15);
        if (!wait32(t, R_GPHY_OCP, 0x80000000u, 1, 10)) return -1;
        return (int)(r32(t, R_GPHY_OCP) & 0xFFFF);
    }
    w32(t, R_PHYAR, (reg & 0x1F) << 16);
    if (!wait32(t, R_PHYAR, 0x80000000u, 1, 10)) return -1;
    return (int)(r32(t, R_PHYAR) & 0xFFFF);
}

static void phy_write(struct rtl *t, unsigned reg, uint16_t val)
{
    if (t->modern) {
        uint32_t a = 0xA400u + reg * 2;
        w32(t, R_GPHY_OCP, 0x80000000u | (a << 15) | val);
        wait32(t, R_GPHY_OCP, 0x80000000u, 0, 10);
        return;
    }
    w32(t, R_PHYAR, 0x80000000u | ((reg & 0x1F) << 16) | val);
    wait32(t, R_PHYAR, 0x80000000u, 0, 10);
}

/* PHY চালু রাখা আর autoneg আবার শুরু */
static void phy_restart_an(struct rtl *t)
{
    int bmcr = phy_read(t, MII_BMCR);
    if (bmcr < 0) {
        tlog(t, "PHY: BMCR read failed");
        return;
    }
    /* ধাপ ২২খ: Windows বন্ধের পর PHY শুধু 10M ঘোষণা করে (WoL power saving) — সব গতি */
    phy_write(t, MII_ANAR, ANAR_ALL);
    phy_write(t, MII_GBCR, GBCR_1000F);
    phy_write(t, MII_BMCR, (uint16_t)((bmcr & ~BMCR_PDOWN) | BMCR_ANENABLE | BMCR_ANRESTART));
    t->an_restarts++;
}

/* ---------- চিপ চেনা ---------- */
static const char *chip_name(uint32_t xid)
{
    /* Linux r8169-এর টেবিলের কয়েকটা (mask, মান) */
    if ((xid & 0x7C8) == 0x540 || (xid & 0x7CF) == 0x541) return "RTL8168H/8111H";
    if ((xid & 0x7CF) == 0x4C0 || (xid & 0x7CF) == 0x509) return "RTL8168G/8111G";
    if ((xid & 0x7CF) == 0x5C8) return "RTL8411B";
    if ((xid & 0x7CF) == 0x502 || (xid & 0x7CF) == 0x501) return "RTL8168EP";
    if ((xid & 0x7CF) == 0x54A || (xid & 0x7CF) == 0x54B) return "RTL8168FP";
    if ((xid & 0x7C8) == 0x480) return "RTL8168F/8111F";
    if ((xid & 0x7C8) == 0x2C0 || (xid & 0x7C8) == 0x2C8) return "RTL8168E/8111E";
    if ((xid & 0x7C8) == 0x280 || (xid & 0x7C8) == 0x288) return "RTL8168D/8111D";
    if ((xid & 0x7C8) == 0x3C0 || (xid & 0x7C8) == 0x3C8) return "RTL8168C/8111C";
    return "RTL8168 family";
}

/* ---------- পাঠানো ---------- */
static int rtl_transmit(struct net_device *dev, const void *frame, size_t len)
{
    struct rtl *t = (struct rtl *)dev->priv;
    unsigned i = t->tx_next;
    struct rtl_desc *d = &t->tx[i];
    uint8_t *buf = t->tx_buf + (size_t)i * BUF_SIZE;
    uint32_t opts;

    if (len > NET_FRAME_MAX) {
        dev->tx_errors++;
        return -1;
    }
    /* চিপ আগের ফ্রেম শেষ না করলে একটু অপেক্ষা */
    {
        uint64_t t0 = timer_uptime_ms();
        while (d->opts1 & D_OWN) {
            if (timer_uptime_ms() - t0 > 50) {
                dev->tx_errors++;
                return -1;
            }
            cpu_pause();
        }
    }
    memcpy(buf, frame, len);
    if (len < 60) {                       /* ছোট ফ্রেম শূন্য দিয়ে ভরা (কিছু চিপ নিজে করে না) */
        memset(buf + len, 0, 60 - len);
        len = 60;
    }
    d->opts2 = 0;
    d->addr = t->tx_buf_phys + (uint64_t)i * BUF_SIZE;
    opts = D_OWN | D_FS | D_LS | (uint32_t)len;
    if (i == NUM_TX - 1) {
        opts |= D_EOR;
    }
    barrier();
    d->opts1 = opts;
    barrier();
    t->tx_next = (i + 1) % NUM_TX;
    w8(t, R_TXPOLL, TXPOLL_NPQ);
    dev->tx_packets++;
    dev->tx_bytes += len;
    return 0;
}

/* ---------- পাওয়া ---------- */
/* ধাপ ২৪ক: ইথারনেট হেডারের ১৪ বাইট সব শূন্য? (আসল ফ্রেমে গন্তব্য MAC কখনো শূন্য নয়) */
static int rx_hdr_zero(const uint8_t *b)
{
    int k;
    for (k = 0; k < 14; k++) {
        if (b[k]) return 0;
    }
    return 1;
}

/* ~১ মাইক্রোসেকেন্ড: port 0x80-তে লেখা (প্রতিটা ~১µs) আর মেমোরি fence */
static void rx_delay(void)
{
    __asm__ volatile("mfence; outb %%al, $0x80" : : "a"(0) : "memory");
}

static void rx_give_back(struct rtl *t, unsigned i)
{
    struct rtl_desc *d = &t->rx[i];
    uint32_t opts = D_OWN | BUF_SIZE;
    if (i == NUM_RX - 1) {
        opts |= D_EOR;
    }
    d->opts2 = 0;
    d->addr = t->rx_buf_phys + (uint64_t)i * BUF_SIZE;
    barrier();
    d->opts1 = opts;
}

/* ধাপ ২২গ: RX বন্ধ → ring-এর ঠিকানা আবার লেখা → সব ঘর চিপকে → RX চালু, RxConfig আবার */
static void rx_restart(struct rtl *t)
{
    unsigned k;
    w8(t, R_CHIPCMD, CMD_TX_EN);
    wait8(t, R_CHIPCMD, CMD_RX_EN, 0, 10);
    for (k = 0; k < NUM_RX; k++) {
        rx_give_back(t, k);
    }
    t->rx_next = 0;
    w8(t, R_CFG9346, CFG9346_UNLOCK);
    w32(t, R_RXDESC_HI, (uint32_t)(t->rx_phys >> 32));
    w32(t, R_RXDESC_LO, (uint32_t)t->rx_phys);
    w8(t, R_CFG9346, CFG9346_LOCK);
    w8(t, R_CHIPCMD, CMD_TX_EN | CMD_RX_EN);
    if (t->rxcfg) {
        w32(t, R_RXCONFIG, t->rxcfg);
    }
}

/* চিপের গণনা মেমরিতে আনা; 1 = হয়েছে */
static int tally_dump(struct rtl *t)
{
    if (!(r8(t, R_CHIPCMD) & CMD_RX_EN)) {
        return 0;
    }
    w32(t, R_COUNTER_HI, (uint32_t)(t->tally_phys >> 32));
    w32(t, R_COUNTER_LO, (uint32_t)t->tally_phys);
    w32(t, R_COUNTER_LO, (uint32_t)t->tally_phys | COUNTER_DUMP);
    return wait32(t, R_COUNTER_LO, COUNTER_DUMP, 0, 10);
}

static size_t rtl_poll(struct net_device *dev, void *buf, size_t max)
{
    struct rtl *t = (struct rtl *)dev->priv;
    uint16_t is = r16(t, R_INTRSTATUS);

    /* poll-মোডে স্ট্যাটাস বিট জমতে না দেওয়া (RxOverflow ইত্যাদি) */
    if (is) {
        t->last_istatus = is;
        t->istatus_seen |= is;
        w16(t, R_INTRSTATUS, is);
        /* ধাপ ২২খ: descriptor নেই বলে চিপ থেমেছে অথচ সব descriptor চিপের হাতে */
        if ((is & IS_RX_DESC_UNAV) && (t->rx[t->rx_next].opts1 & D_OWN)) {
            t->rx_restarts++;
            rx_restart(t);
            if (t->rx_restarts <= 5) {
                tlog(t, "RX stalled (IntrStatus %04x): ring restarted #%d", is, t->rx_restarts);
            }
        }
    }
    /* ধাপ ২২গ: ২ সেকেন্ড পরপর চিপের গণনা — চিপ নতুন ফ্রেম পেয়েছে অথচ আমাদের ring-এ
     * কিছু আসেনি (পরের ঘর এখনো চিপের হাতে) হলে RX আটকে গেছে: আবার চালু */
    if (t->tally && timer_uptime_ms() - t->wd_ms >= 2000) {
        t->wd_ms = timer_uptime_ms();
        if (tally_dump(t)) {
            uint64_t chip_rx = t->tally->rx_packets;
            if (chip_rx > t->wd_chip_rx && dev->rx_packets == t->wd_our_rx &&
                (t->rx[t->rx_next].opts1 & D_OWN)) {
                t->stall_restarts++;
                if (t->stall_restarts <= 5) {
                    tlog(t, "RX stall: chip counted %llu new frames, ring got none (rx next %u):"
                         " restart #%d", (unsigned long long)(chip_rx - t->wd_chip_rx),
                         t->rx_next, t->stall_restarts);
                }
                rx_restart(t);
            }
            t->wd_chip_rx = chip_rx;
            t->wd_our_rx = dev->rx_packets;
        }
    }
    for (;;) {
        unsigned i = t->rx_next;
        struct rtl_desc *d = &t->rx[i];
        uint32_t st = d->opts1;
        size_t len;

        if (st & D_OWN) {
            return 0;
        }
        barrier();
        __asm__ volatile("lfence" ::: "memory");   /* ধাপ ২৪ক: OWN দেখার পরে ডেটা পড়া */
        len = st & D_LEN_MASK;
        if ((st & D_RX_RES) || (st & (D_FS | D_LS)) != (D_FS | D_LS) || len < 4 ||
            len - 4 > max) {
            t->rx_errors++;
            dev->rx_dropped++;
            len = 0;
        } else {
            len -= 4;                     /* শেষের CRC বাদ */
            memcpy(buf, t->rx_buf + (size_t)i * BUF_SIZE, len);
            /* ধাপ ২৪ক: প্রথম ১৪ বাইট (ইথারনেট হেডার) সব শূন্য হলে ডেটা হয়তো এখনো
             * মেমোরিতে পৌঁছায়নি — একটু অপেক্ষা করে আবার পড়া */
            {
                int tries;
                for (tries = 0; tries < 64 && len >= 14 && rx_hdr_zero(buf); tries++) {
                    rx_delay();
                    memcpy(buf, t->rx_buf + (size_t)i * BUF_SIZE, len);
                }
                if (tries) {
                    t->rx_zero_late++;
                    if (len >= 14 && rx_hdr_zero(buf)) {
                        t->rx_zero_drop++;
                        t->rx_errors++;
                        dev->rx_dropped++;
                        rx_give_back(t, i);
                        t->rx_next = (i + 1) % NUM_RX;
                        continue;
                    }
                }
            }
            dev->rx_packets++;
            dev->rx_bytes += len;
        }
        rx_give_back(t, i);
        t->rx_next = (i + 1) % NUM_RX;
        if (len) {
            return len;
        }
    }
}

/* ---------- লিংক ---------- */
static int rtl_link(struct net_device *dev)
{
    struct rtl *t = (struct rtl *)dev->priv;
    uint8_t ps = r8(t, R_PHYSTATUS);
    int up = (ps & PHY_LINK) != 0;

    /* লিংক না থাকলে প্রতি ৮ সেকেন্ডে একবার autoneg আবার (সর্বোচ্চ ৪ বার) */
    if (!up && t->an_restarts < 4 && timer_uptime_ms() - t->link_check_ms > 8000) {
        t->link_check_ms = timer_uptime_ms();
        phy_restart_an(t);
        tlog(t, "link down: autoneg restart #%d", t->an_restarts);
    }
    if (up && !dev->link_up) {
        tlog(t, "link up: %s %s-duplex", (ps & PHY_1000F) ? "1000" : (ps & PHY_100) ? "100" :
             (ps & PHY_10) ? "10" : "?", (ps & PHY_FULL) ? "full" : "half");
    }
    return up;
}

/* ---------- kmon `nic` ---------- */
static void rtl_diag(struct net_device *dev)
{
    struct rtl *t = (struct rtl *)dev->priv;
    uint8_t ps = r8(t, R_PHYSTATUS);
    int bmcr = phy_read(t, MII_BMCR), bmsr = phy_read(t, MII_BMSR);
    unsigned own = 0, town = 0, i;

    for (i = 0; i < NUM_RX; i++) {
        if (t->rx[i].opts1 & D_OWN) own++;
    }
    for (i = 0; i < NUM_TX; i++) {
        if (t->tx[i].opts1 & D_OWN) town++;
    }
    kprintf("     chip %s  PCI %02x:%02x.%u rev %02x  XID %03x  %s\n", t->chip, t->pci->bus,
            t->pci->slot, t->pci->func, t->pci->revision, t->xid,
            t->modern ? "(8168G+ path)" : "(classic path)");
    kprintf("     ChipCmd %02x  TxConfig %08x  RxConfig %08x  CPlusCmd %04x  MISC %08x\n",
            r8(t, R_CHIPCMD), r32(t, R_TXCONFIG), r32(t, R_RXCONFIG), r16(t, R_CPLUSCMD),
            r32(t, R_MISC));
    kprintf("     PHYstatus %02x (link %s, %s)  BMCR %04x  BMSR %04x  autoneg restarts %d\n", ps,
            (ps & PHY_LINK) ? "up" : "down",
            (ps & PHY_1000F) ? "1000" : (ps & PHY_100) ? "100" : (ps & PHY_10) ? "10" : "-",
            bmcr & 0xFFFF, bmsr & 0xFFFF, t->an_restarts);
    kprintf("     rx next %u (chip owns %u/%u)  tx next %u (chip owns %u/%u)  rx errors %llu\n",
            t->rx_next, own, NUM_RX, t->tx_next, town, NUM_TX, (unsigned long long)t->rx_errors);
    kprintf("     zero-header frames: re-read %llu, dropped %llu\n",
            (unsigned long long)t->rx_zero_late, (unsigned long long)t->rx_zero_drop);
    kprintf("     IntrStatus last %04x, seen %04x (01 RxOK 04 TxOK 08 TxErr 10 RxDescUnavail)"
            "  rx restarts %d\n", t->last_istatus, t->istatus_seen, t->rx_restarts);
    kprintf("     DLLPR %02x  MISC_1 %02x  ANAR %04x  GBCR %04x\n", r8(t, R_DLLPR),
            r8(t, R_MISC_1), phy_read(t, MII_ANAR) & 0xFFFF, phy_read(t, MII_GBCR) & 0xFFFF);
    /* ধাপ ২২গ */
    if (t->tally && tally_dump(t)) {
        struct rtl_tally *c = t->tally;
        kprintf("     chip tally: rx %llu (unicast %llu bcast %llu mcast %u) missed %u rx-err %u"
                " align %u\n", (unsigned long long)c->rx_packets,
                (unsigned long long)c->rx_unicast, (unsigned long long)c->rx_broadcast,
                c->rx_multicast, c->rx_missed, c->rx_errors, c->align_errors);
        kprintf("                 tx %llu tx-err %llu aborted %u underrun %u collisions %u/%u\n",
                (unsigned long long)c->tx_packets, (unsigned long long)c->tx_errors,
                c->tx_aborted, c->tx_underrun, c->tx_one_collision, c->tx_multi_collision);
    } else {
        kprintf("     chip tally: not available\n");
    }
    kprintf("     stall restarts %d   desc: ", t->stall_restarts);
    {
        unsigned idx[4];
        int k;
        idx[0] = (t->rx_next + NUM_RX - 1) % NUM_RX;
        idx[1] = t->rx_next;
        idx[2] = (t->rx_next + 1) % NUM_RX;
        idx[3] = NUM_RX - 1;
        for (k = 0; k < 4; k++) {
            kprintf("[%u] %08x/%08x  ", idx[k], t->rx[idx[k]].opts1,
                    (uint32_t)t->rx[idx[k]].addr);
        }
        kprintf("\n     ring %llx bufs %llx  RxDesc reg %08x\n", (unsigned long long)t->rx_phys,
                (unsigned long long)t->rx_buf_phys, r32(t, R_RXDESC_LO));
    }
    if (t->log_len) {
        kprintf("     init log:\n%s", t->log);
    }
}

/* ---------- চালু করা ---------- */
static int setup_rings(struct rtl *t)
{
    /* ring: এক পেজ করে; বাফার: পাশাপাশি পেজ (256×2 KiB = 128 পেজ, 64×2 KiB = 32 পেজ) */
    uint64_t lim = 0x100000000ull;
    unsigned i;

    t->rx_phys = pmm_alloc_contiguous(1, lim);
    t->tx_phys = pmm_alloc_contiguous(1, lim);
    t->rx_buf_phys = pmm_alloc_contiguous(NUM_RX * BUF_SIZE / 4096, lim);
    t->tx_buf_phys = pmm_alloc_contiguous(NUM_TX * BUF_SIZE / 4096, lim);
    if (!t->rx_phys || !t->tx_phys || !t->rx_buf_phys || !t->tx_buf_phys) {
        tlog(t, "out of DMA memory below 4 GiB");
        return 0;
    }
    t->tally_phys = pmm_alloc_contiguous(1, lim);          /* ধাপ ২২গ (না পেলে tally বাদ) */
    if (t->tally_phys) {
        t->tally = (struct rtl_tally *)pmm_phys_to_virt(t->tally_phys);
        memset(t->tally, 0, 4096);
    }
    t->rx = (struct rtl_desc *)pmm_phys_to_virt(t->rx_phys);
    t->tx = (struct rtl_desc *)pmm_phys_to_virt(t->tx_phys);
    t->rx_buf = (uint8_t *)pmm_phys_to_virt(t->rx_buf_phys);
    t->tx_buf = (uint8_t *)pmm_phys_to_virt(t->tx_buf_phys);
    memset(t->rx, 0, 4096);
    memset(t->tx, 0, 4096);
    for (i = 0; i < NUM_RX; i++) {
        rx_give_back(t, i);
    }
    for (i = 0; i < NUM_TX; i++) {
        t->tx[i].opts1 = (i == NUM_TX - 1) ? D_EOR : 0;
    }
    t->rx_next = 0;
    t->tx_next = 0;
    return 1;
}

static int setup_chip(struct rtl *t)
{
    uint16_t cp;
    uint32_t rxcfg, txcfg;
    int i;

    w16(t, R_INTRMASK, 0);
    w16(t, R_INTRSTATUS, 0xFFFF);

    t->xid = (r32(t, R_TXCONFIG) >> 20) & 0xFCF;
    if (t->xid == 0xFCF || t->xid == 0) {
        tlog(t, "TxConfig reads %08x: chip not responding (power state?)", r32(t, R_TXCONFIG));
        return 0;
    }
    t->chip = chip_name(t->xid);
    /* 8168G (0x4c0) ও তার পরের সব: RXDV gate, OCP PHY, ERI FIFO */
    t->modern = (t->xid & 0x7C8) >= 0x4C0 && (t->xid & 0x7C8) != 0x7C8;
    tlog(t, "XID %03x -> %s", t->xid, t->chip);

    /* PLL/PHY পাওয়ার (8168G+: PMCH-এর উপরের দুই বিট) */
    if (t->modern) {
        w8(t, R_PMCH, r8(t, R_PMCH) | 0xC0);
    }

    /* রিসেট: নতুন চিপে আগে RXDV gate বসিয়ে RX বন্ধ */
    if (t->modern) {
        w32(t, R_MISC, r32(t, R_MISC) | MISC_RXDV_GATE);
    }
    w8(t, R_CHIPCMD, CMD_RESET);
    if (!wait8(t, R_CHIPCMD, CMD_RESET, 0, 100)) {
        tlog(t, "reset did not finish (ChipCmd %02x)", r8(t, R_CHIPCMD));
        return 0;
    }
    tlog(t, "reset ok");

    /* MAC: firmware IDR0-5-এ রেখে দেয় */
    for (i = 0; i < 6; i++) {
        t->dev.mac[i] = r8(t, R_MAC0 + (uint32_t)i);
    }
    tlog(t, "MAC %02x:%02x:%02x:%02x:%02x:%02x", t->dev.mac[0], t->dev.mac[1], t->dev.mac[2],
         t->dev.mac[3], t->dev.mac[4], t->dev.mac[5]);

    w8(t, R_CFG9346, CFG9346_UNLOCK);

    /* ASPM আর CLKREQ বন্ধ (ঘুমিয়ে গিয়ে DMA আটকে যাওয়া এড়াতে) */
    w8(t, R_CONFIG2, r8(t, R_CONFIG2) & ~CONFIG2_CLKREQ);
    w8(t, R_CONFIG5, r8(t, R_CONFIG5) & ~CONFIG5_ASPM);

    cp = r16(t, R_CPLUSCMD);
    cp &= (uint16_t)~(CP_RXCHKSUM | CP_RXVLAN);
    w16(t, R_CPLUSCMD, cp);

    w8(t, R_MAXTXPKT, 0x3F);              /* TxPacketMax = 8064 >> 7 */

    if (t->modern) {
        /* Linux rtl_hw_start_8168h_1-এর মূল অংশ */
        eri_write(t, 0xC8, ERIAR_MASK_1111, (0x08u << 16) | 0x10);   /* FIFO মাপ */
        eri_write(t, 0xE8, ERIAR_MASK_1111, (0x02u << 16) | 0x06);
        eri_write(t, 0xDC, ERIAR_MASK_1111, eri_read(t, 0xDC) & ~1u); /* packet filter রিসেট */
        eri_write(t, 0xDC, ERIAR_MASK_1111, eri_read(t, 0xDC) | 1u);
        w32(t, R_MISC, r32(t, R_MISC) & ~MISC_RXDV_GATE);           /* RXDV gate বন্ধ */
        eri_write(t, 0xC0, ERIAR_MASK_0011, 0);
        eri_write(t, 0xB8, ERIAR_MASK_0011, 0);
        /* ধাপ ২২খ: rtl_hw_start_8168h_1-এর বাকিটা (EEE আর rg_saw_cnt বাদে) */
        eri_write(t, 0xDC, ERIAR_MASK_1111, eri_read(t, 0xDC) | 0x1C);
        eri_write(t, 0x5F0, ERIAR_MASK_0011, 0x4F87);
        w8(t, R_DLLPR, r8(t, R_DLLPR) & ~DLLPR_PFM_EN);
        w8(t, R_MISC_1, r8(t, R_MISC_1) & ~MISC1_PFM_D3COLD);
        w8(t, R_DLLPR, r8(t, R_DLLPR) & ~DLLPR_TX_10M_PS);         /* 10M power save বন্ধ */
        eri_write(t, 0x1B0, ERIAR_MASK_1111, eri_read(t, 0x1B0) & ~(1u << 12));
        mac_ocp_modify(t, 0xE056, 0x00F0, 0x0070);
        mac_ocp_modify(t, 0xE052, 0x6000, 0x8008);
        mac_ocp_modify(t, 0xE0D6, 0x01FF, 0x017F);
        mac_ocp_modify(t, 0xD420, 0x0FFF, 0x047F);
        mac_ocp_write(t, 0xE63E, 0x0001);
        mac_ocp_write(t, 0xE63E, 0x0000);
        mac_ocp_write(t, 0xC094, 0x0000);
        mac_ocp_write(t, 0xC09E, 0x0000);
        tlog(t, "8168H extra: DLLPR %02x MISC_1 %02x (power saving off), MAC OCP", r8(t, R_DLLPR),
             r8(t, R_MISC_1));
        tlog(t, "8168G+ setup: FIFO, packet filter, RXDV gate off (MISC %08x)", r32(t, R_MISC));
    }

    w16(t, R_INTRMITIGATE, 0);
    w16(t, R_RXMAXSIZE, BUF_SIZE);

    /* ঠিকানা: আগে উপরের অর্ধেক */
    w32(t, R_TXDESC_HI, (uint32_t)(t->tx_phys >> 32));
    w32(t, R_TXDESC_LO, (uint32_t)t->tx_phys);
    w32(t, R_RXDESC_HI, (uint32_t)(t->rx_phys >> 32));
    w32(t, R_RXDESC_LO, (uint32_t)t->rx_phys);

    w8(t, R_CFG9346, CFG9346_LOCK);
    (void)r8(t, R_CHIPCMD);               /* PCI commit */

    w8(t, R_CHIPCMD, CMD_TX_EN | CMD_RX_EN);

    if (t->modern) {
        rxcfg = RX_128_INT_EN | RX_MULTI_EN | RX_DMA_BURST | RX_EARLY_OFF;
    } else {
        rxcfg = RX_FIFO_THRESH | RX_DMA_BURST;
    }
    txcfg = TX_DMA_BURST | TX_IFG;
    if (t->modern) {
        txcfg |= TX_AUTO_FIFO;
    }
    w32(t, R_TXCONFIG, txcfg);

    /* সব multicast গ্রহণ (lwIP-এর IGMP সরল রাখতে) */
    w32(t, R_MAR0 + 4, 0xFFFFFFFF);
    w32(t, R_MAR0, 0xFFFFFFFF);
    t->rxcfg = (rxcfg & ~RX_ACCEPT_MASK) | RX_ACCEPT_BCAST | RX_ACCEPT_MULTI | RX_ACCEPT_PHYS;
    w32(t, R_RXCONFIG, t->rxcfg);

    w16(t, R_INTRMASK, 0);                /* ইন্টারাপ্ট নয়, netd poll করে */
    w16(t, R_INTRSTATUS, 0xFFFF);

    tlog(t, "rx/tx on: ChipCmd %02x RxConfig %08x TxConfig %08x", r8(t, R_CHIPCMD),
         r32(t, R_RXCONFIG), r32(t, R_TXCONFIG));

    /* PHY: পাওয়ার-ডাউনে থাকলে জাগানো; নইলে BIOS-এর autoneg চলতে থাকে */
    {
        int bmcr = phy_read(t, MII_BMCR);
        tlog(t, "PHY BMCR %04x BMSR %04x PHYstatus %02x", bmcr & 0xFFFF,
             phy_read(t, MII_BMSR) & 0xFFFF, r8(t, R_PHYSTATUS));
        /* ধাপ ২২খ: সবসময় সব গতি ঘোষণা করে autoneg আবার (আগে শুধু PDOWN/autoneg বন্ধ
         * থাকলে) — Windows-এর WoL অবস্থায় ANAR-এ শুধু 10M থাকে, লিংক 10M-এ আসত */
        if (bmcr >= 0) {
            tlog(t, "PHY ANAR %04x GBCR %04x -> advertise 10/100/1000, autoneg restart",
                 phy_read(t, MII_ANAR) & 0xFFFF, phy_read(t, MII_GBCR) & 0xFFFF);
            phy_restart_an(t);
            t->an_restarts = 0;           /* শুরুরটা গোনা হয় না */
        }
    }
    t->link_check_ms = timer_uptime_ms();
    return 1;
}

int rtl8168_init(void)
{
    static const uint16_t ids[] = { 0x8168 };
    size_t k, n;
    int found = 0;

    for (k = 0; k < sizeof(ids) / sizeof(ids[0]); k++) {
        for (n = 0; ; n++) {
            struct pci_device *pd = pci_find_id(0x10EC, ids[k], n);
            struct rtl *t;
            uint64_t mmio = 0;
            int b;

            if (!pd) {
                break;
            }
            /* প্রথম memory BAR (সাধারণত BAR2, ৬৪-বিট); BAR0 I/O */
            for (b = 0; b < 6; b++) {
                if (pd->bar[b] && !pd->bar_is_io[b]) {
                    mmio = pd->bar[b];
                    break;
                }
            }
            t = (struct rtl *)kzalloc(sizeof(*t));
            if (!t) {
                return found;
            }
            t->pci = pd;
            t->chip = "RTL8168";
            tlog(t, "found 10ec:%04x rev %02x at %02x:%02x.%u, MMIO BAR%d %llx", ids[k],
                 pd->revision, pd->bus, pd->slot, pd->func, b, (unsigned long long)mmio);
            if (!mmio) {
                tlog(t, "no memory BAR - skipped");
                kfree(t);
                continue;
            }
            pci_set_power_d0(pd);
            pci_disable_aspm(pd);
            {
                /* ধাপ ২২গ: PCIe Device Control-এ No Snoop বন্ধ (চিপের DMA লেখা যেন CPU-র
                 * cache-এর সাথে মেলে); Linux কিছু 8168-এ একই কাজ করে */
                uint8_t pe = pci_find_cap(pd, 0x10);
                if (pe) {
                    uint16_t dc = pci_read16(pd, (uint8_t)(pe + 8));
                    /* ধাপ ২৪ক: Relaxed Ordering (বিট ৪)-ও বন্ধ — চালু থাকলে descriptor-এর OWN
                     * মোছা বাফারের ডেটার আগে পৌঁছাতে পারে, তখন আমরা শূন্য-ভরা ফ্রেম পড়ি
                     * (আসল পিসিতে DHCP OFFER এভাবেই হারাচ্ছিল) */
                    tlog(t, "PCIe DevCtl %04x%s%s", dc, (dc & 0x0800) ? " -> No Snoop off" : "",
                         (dc & 0x0010) ? " -> Relaxed Ordering off" : "");
                    if (dc & 0x0810) {
                        pci_write16(pd, (uint8_t)(pe + 8), (uint16_t)(dc & ~0x0810u));
                    }
                }
            }
            pci_enable_device(pd);
            t->regs = (volatile uint8_t *)vmm_map_mmio(mmio, 0x1000);
            if (!t->regs || !setup_rings(t) || !setup_chip(t)) {
                kprintf("[WARN] rtl8168: setup failed (see messages above)\n");
                /* DMA মেমোরি ফেরত নয় — চিপ হয়তো এখনো ঠিকানাগুলো জানে */
                kfree(t);
                continue;
            }
            t->dev.driver = "rtl8168";
            if (!strcmp(t->chip, "RTL8168H/8111H")) {
                t->dev.driver = "rtl8168 (8168H)";
            }
            t->dev.transmit = rtl_transmit;
            t->dev.poll = rtl_poll;
            t->dev.link_status = rtl_link;
            t->dev.diag = rtl_diag;
            t->dev.priv = t;
            t->dev.link_up = (r8(t, R_PHYSTATUS) & PHY_LINK) != 0;
            if (net_register_device(&t->dev) == 0) {
                found++;
            }
        }
    }
    return found;
}
