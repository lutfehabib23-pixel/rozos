/* rozOS — USB: xHCI কন্ট্রোলার + পেন ড্রাইভ (mass storage) + কিবোর্ড/মাউস (HID) — ধাপ ১৫খ
 * rozos/usb.h দেখুন।
 *
 * xHCI-র মূল কথা: কন্ট্রোলারের সাথে কথা হয় মেমোরির "ring" দিয়ে — প্রতিটা ১৬ বাইটের TRB।
 *   command ring  (আমরা লিখি)  : Enable Slot, Address Device, Configure Endpoint ...
 *   transfer ring (প্রতি endpoint): Setup/Data/Status (control), Normal (bulk/interrupt)
 *   event ring    (কন্ট্রোলার লেখে): কমান্ড শেষ, transfer শেষ, port বদল
 * প্রতিটা TRB-র "cycle" বিট দিয়ে বোঝা যায় কোনটা নতুন। doorbell লিখে কন্ট্রোলারকে জানানো।
 * সব polling-এ: ঘটনা পড়ে endpoint-এর "done" চিহ্ন, অপেক্ষাকারী সেটাই দেখে।
 *
 * ডিভাইস চেনা (enumeration): port reset → Enable Slot → Address Device → descriptor পড়া →
 * SET_CONFIGURATION → Configure Endpoint (bulk/interrupt) → ধরন অনুযায়ী:
 *   mass storage (class 8, SCSI, Bulk-Only): CBW → data → CSW; INQUIRY, READ CAPACITY,
 *     READ(10)/WRITE(10) → ব্লক ডিভাইস "sd?" (removable), partition, /mnt/usb-তে mount
 *   HID boot keyboard/mouse (class 3): interrupt IN রিপোর্ট → PS/2 scancode / মাউস-ঘটনা */

#include <stdint.h>
#include <stddef.h>
#include <rozos/usb.h>
#include <rozos/pci.h>
#include <rozos/pmm.h>
#include <rozos/vmm.h>
#include <rozos/heap.h>
#include <rozos/timer.h>
#include <rozos/errno.h>
#include <rozos/string.h>
#include <rozos/printf.h>
#include <rozos/sched.h>
#include <rozos/block.h>
#include <rozos/vfs.h>
#include <rozos/fat.h>
#include <rozos/keyboard.h>
#include <rozos/mouse.h>
#include <rozos/devfs.h>
#include <arch/x86_64/cpu.h>

#define MAX_HC      4
#define MAX_DEVS    32
#define RING_TRBS   256
#define EV_TRBS     256
#define MSC_PAGES   16                   /* ৬৪ KiB এক কমান্ডে */

/* ---------- রেজিস্টার ---------- */
#define CAP_CAPLENGTH 0x00
#define CAP_HCSP1     0x04
#define CAP_HCSP2     0x08
#define CAP_HCCP1     0x10
#define CAP_DBOFF     0x14
#define CAP_RTSOFF    0x18
#define OP_USBCMD     0x00
#define OP_USBSTS     0x04
#define OP_CRCR       0x18
#define OP_DCBAAP     0x30
#define OP_CONFIG     0x38
#define OP_PORTSC(n)  (0x400 + 0x10 * ((n) - 1))
#define PORTSC_CCS    (1u << 0)
#define PORTSC_PED    (1u << 1)
#define PORTSC_PR     (1u << 4)
#define PORTSC_PP     (1u << 9)
#define PORTSC_CHANGE 0x00FE0000u        /* CSC PEC WRC OCC PRC PLC CEC — লিখে ১ = মোছা */
#define PORTSC_PRC    (1u << 21)

/* TRB ধরন */
enum {
    TRB_NORMAL = 1, TRB_SETUP = 2, TRB_DATA = 3, TRB_STATUS = 4, TRB_LINK = 6,
    TRB_ENABLE_SLOT = 9, TRB_DISABLE_SLOT = 10, TRB_ADDRESS = 11, TRB_CONFIG_EP = 12,
    TRB_EVAL_CTX = 13, TRB_RESET_EP = 14, TRB_SET_DEQ = 16,
    TRB_EV_TRANSFER = 32, TRB_EV_CMD = 33, TRB_EV_PORT = 34
};
#define CC_SUCCESS 1
#define CC_STALL   6
#define CC_SHORT   13

struct trb { uint32_t d0, d1, d2, d3; };

struct ring {
    volatile struct trb *t;
    uint64_t phys;
    uint32_t enq, cycle;
};

struct usb_ep {
    struct ring ring;
    int dci, mps;
    volatile int done;
    uint32_t cc, residual;
};

struct xhci;

struct usb_dev {
    struct xhci *hc;
    int used, gone, slot, port, speed;
    uint8_t *out_ctx, *in_ctx, *ctl;
    uint64_t out_phys, in_phys, ctl_phys;
    struct usb_ep ep0, ep[32];           /* ep[dci] */
    uint16_t vid, pid;
    uint8_t cls, sub, proto, iface;
    char product[48];
    int kind;                            /* 1 পেন ড্রাইভ, 2 কিবোর্ড, 3 মাউস */
    /* mass storage */
    int bin, bout;                       /* dci */
    uint8_t bin_addr, bout_addr;
    uint32_t tag;
    uint8_t *buf;                        /* পেজ ০: CBW/CSW, পেজ ১..: ডেটা */
    uint64_t buf_phys;
    struct block_device bdev;
    int registered, mount_tried;
    int need_scan, need_umount;          /* usbd lock-এর বাইরে করে (deadlock এড়াতে) */
    char mountpoint[24];
    /* HID */
    int iin, queued;
    uint8_t last[8];
    int held;                            /* শেষ চাপা কী (usage) — নিজে থেকে পুনরাবৃত্তি */
    uint64_t held_at, repeat_at;
};

struct xhci {
    volatile uint8_t *base, *op, *rt;
    volatile uint32_t *db;
    uint32_t max_slots, max_ports, csz, ac64;
    uint64_t *dcbaa, dcbaa_phys;
    struct ring cmd;
    volatile struct trb *ev;
    uint64_t ev_phys, erst_phys;
    uint32_t ev_deq, ev_ccs;
    volatile int cmd_done;
    uint32_t cmd_cc, cmd_slot;
    uint64_t cmd_trb;
    struct usb_dev *slot_dev[256];
    struct usb_dev *port_dev[256];
    uint8_t port_failed[256];
    struct kmutex lock;
    int index;
};

static struct xhci hcs[MAX_HC];
static int nhc;
static struct usb_dev devs[MAX_DEVS];
static int booting = 1;                  /* বুটের সময় partition main.c নিজে খোঁজে */
static int fs_ready;                     /* "/" mount হয়েছে — এখন /mnt/usb বানানো যায় */

/* ---------- ছোট সাহায্য ---------- */
static inline uint32_t r32(volatile uint8_t *b, uint32_t o) { return *(volatile uint32_t *)(b + o); }
static inline void w32(volatile uint8_t *b, uint32_t o, uint32_t v) { *(volatile uint32_t *)(b + o) = v; }
static inline void w64(volatile uint8_t *b, uint32_t o, uint64_t v) { w32(b, o, (uint32_t)v); w32(b, o + 4, (uint32_t)(v >> 32)); }

static uint64_t dma(struct xhci *hc, uint64_t pages)
{
    uint64_t p = pmm_alloc_contiguous(pages, hc && !hc->ac64 ? 0x100000000ULL : 0);
    if (p) memset(pmm_phys_to_virt(p), 0, (size_t)(pages * VMM_PAGE_SIZE));
    return p;
}

static void idle_wait(void)
{
    if (sched_can_sleep()) sched_yield();
    else cpu_pause();
}

static int ring_init(struct xhci *hc, struct ring *r)
{
    r->phys = dma(hc, 1);
    if (!r->phys) return -ENOMEM;
    r->t = (volatile struct trb *)pmm_phys_to_virt(r->phys);
    r->enq = 0;
    r->cycle = 1;
    return 0;
}

/* একটা TRB যোগ; ফেরত তার ফিজিক্যাল ঠিকানা */
static uint64_t ring_push(struct ring *r, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3)
{
    volatile struct trb *t = &r->t[r->enq];
    uint64_t at = r->phys + (uint64_t)r->enq * 16;
    t->d0 = d0;
    t->d1 = d1;
    t->d2 = d2;
    __asm__ __volatile__("" : : : "memory");
    t->d3 = (d3 & ~1u) | r->cycle;              /* cycle বিট শেষে — তখনই TRB "তৈরি" */
    r->enq++;
    if (r->enq == RING_TRBS - 1) {               /* শেষ ঘর: Link TRB → শুরুতে, cycle উল্টো */
        volatile struct trb *l = &r->t[r->enq];
        l->d0 = (uint32_t)r->phys;
        l->d1 = (uint32_t)(r->phys >> 32);
        l->d2 = 0;
        __asm__ __volatile__("" : : : "memory");
        l->d3 = (TRB_LINK << 10) | (1u << 1) | r->cycle;
        r->enq = 0;
        r->cycle ^= 1;
    }
    return at;
}

static void ring_doorbell(struct xhci *hc, int slot, int target)
{
    __asm__ __volatile__("mfence" : : : "memory");
    hc->db[slot] = (uint32_t)target;
}

/* ---------- ঘটনা ---------- */
static void handle_events(struct xhci *hc)
{
    int any = 0;
    for (;;) {
        volatile struct trb *e = &hc->ev[hc->ev_deq];
        uint32_t d3 = e->d3, type;
        if ((d3 & 1) != hc->ev_ccs) break;
        type = (d3 >> 10) & 0x3F;
        if (type == TRB_EV_CMD) {
            hc->cmd_trb = (uint64_t)e->d0 | ((uint64_t)e->d1 << 32);
            hc->cmd_cc = e->d2 >> 24;
            hc->cmd_slot = d3 >> 24;
            hc->cmd_done = 1;
        } else if (type == TRB_EV_TRANSFER) {
            uint32_t slot = d3 >> 24, dci = (d3 >> 16) & 0x1F;
            struct usb_dev *d = slot < 256 ? hc->slot_dev[slot] : NULL;
            if (d) {
                struct usb_ep *ep = dci == 1 ? &d->ep0 : &d->ep[dci];
                ep->cc = e->d2 >> 24;
                ep->residual = e->d2 & 0xFFFFFF;
                ep->done = 1;
            }
        }
        /* port-বদল (34) — port গুলো usbd নিজে দেখে */
        hc->ev_deq++;
        if (hc->ev_deq == EV_TRBS) {
            hc->ev_deq = 0;
            hc->ev_ccs ^= 1;
        }
        any = 1;
    }
    if (any) w64(hc->rt, 0x20 + 0x18, (hc->ev_phys + (uint64_t)hc->ev_deq * 16) | (1u << 3));
}

/* কমান্ড পাঠিয়ে উত্তর; ফেরত completion code (১ = ঠিক), <0 = সময় শেষ */
static int command(struct xhci *hc, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3, uint32_t *slot_out)
{
    uint64_t at, end = timer_uptime_ms() + 2000;
    hc->cmd_done = 0;
    at = ring_push(&hc->cmd, d0, d1, d2, d3);
    ring_doorbell(hc, 0, 0);
    for (;;) {
        handle_events(hc);
        if (hc->cmd_done && hc->cmd_trb == at) break;
        if (hc->cmd_done) hc->cmd_done = 0;      /* অন্য (পুরোনো) কমান্ডের উত্তর */
        if (timer_uptime_ms() > end) return -ETIMEDOUT;
        idle_wait();
    }
    if (slot_out) *slot_out = hc->cmd_slot;
    return (int)hc->cmd_cc;
}

static int wait_ep(struct xhci *hc, struct usb_ep *ep, uint64_t ms)
{
    uint64_t end = timer_uptime_ms() + ms;
    while (!ep->done) {
        handle_events(hc);
        if (ep->done) break;
        if (timer_uptime_ms() > end) return -ETIMEDOUT;
        idle_wait();
    }
    return (int)ep->cc;
}

/* ---------- context ---------- */
static uint32_t *in_ctx(struct usb_dev *d, int idx) { return (uint32_t *)(d->in_ctx + (size_t)idx * d->hc->csz); }

static int ep0_mps_for(int speed)
{
    return speed == 2 ? 8 : speed == 1 ? 8 : speed == 3 ? 64 : 512;
}

/* ---------- control transfer (EP0) ---------- */
static int control(struct usb_dev *d, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx, uint16_t len, void *data)
{
    struct xhci *hc = d->hc;
    int in = (rt & 0x80) != 0, cc;
    uint32_t trt = len ? (in ? 3u : 2u) : 0u;
    if (len > 2048) return -EINVAL;
    if (len && !in && data) memcpy(d->ctl, data, len);
    d->ep0.done = 0;
    ring_push(&d->ep0.ring, (uint32_t)rt | ((uint32_t)req << 8) | ((uint32_t)val << 16),
              (uint32_t)idx | ((uint32_t)len << 16), 8, (TRB_SETUP << 10) | (1u << 6) | (trt << 16));
    if (len) ring_push(&d->ep0.ring, (uint32_t)d->ctl_phys, (uint32_t)(d->ctl_phys >> 32), len,
                       (TRB_DATA << 10) | (in ? (1u << 16) : 0));
    ring_push(&d->ep0.ring, 0, 0, 0, (TRB_STATUS << 10) | (1u << 5) | ((len && in) ? 0 : (1u << 16)));
    ring_doorbell(hc, d->slot, 1);
    cc = wait_ep(hc, &d->ep0, 2000);
    if (cc != CC_SUCCESS && cc != CC_SHORT) return cc < 0 ? cc : -EIO;
    if (len && in && data) memcpy(data, d->ctl, len);
    return 0;
}

static int get_desc(struct usb_dev *d, uint8_t type, uint8_t index, uint16_t lang, void *buf, uint16_t len)
{
    return control(d, 0x80, 6, (uint16_t)((type << 8) | index), lang, len, buf);
}

/* ---------- bulk / interrupt (Normal TRB, পেজ ধরে ভাগ) ---------- */
static int transfer(struct usb_dev *d, int dci, uint64_t phys, uint32_t len, uint64_t ms)
{
    struct usb_ep *ep = &d->ep[dci];
    uint32_t off = 0;
    ep->done = 0;
    do {
        uint32_t n = (uint32_t)(VMM_PAGE_SIZE - ((phys + off) & (VMM_PAGE_SIZE - 1)));
        int last;
        if (n > len - off) n = len - off;
        last = off + n >= len;
        ring_push(&ep->ring, (uint32_t)(phys + off), (uint32_t)((phys + off) >> 32), n,
                  (TRB_NORMAL << 10) | (1u << 2) | (last ? (1u << 5) : (1u << 4)));
        off += n;
    } while (off < len);
    ring_doorbell(d->hc, d->slot, dci);
    if (!ms) return 0;                           /* HID: পরে দেখা হবে */
    return wait_ep(d->hc, ep, ms);
}

/* endpoint আটকে গেলে (STALL): Reset Endpoint + TR Dequeue + CLEAR_FEATURE(HALT) */
static void clear_halt(struct usb_dev *d, int dci, uint8_t addr)
{
    struct usb_ep *ep = &d->ep[dci];
    command(d->hc, 0, 0, 0, (TRB_RESET_EP << 10) | ((uint32_t)d->slot << 24) | ((uint32_t)dci << 16), NULL);
    command(d->hc, (uint32_t)((ep->ring.phys + ep->ring.enq * 16) | ep->ring.cycle),
            (uint32_t)((ep->ring.phys + ep->ring.enq * 16) >> 32), 0,
            (TRB_SET_DEQ << 10) | ((uint32_t)d->slot << 24) | ((uint32_t)dci << 16), NULL);
    control(d, 0x02, 1, 0, addr, 0, NULL);
}

/* ---------- endpoint চালু (Configure Endpoint) ---------- */
static int add_endpoint(struct usb_dev *d, uint8_t addr, uint8_t attr, uint16_t mps, uint8_t interval)
{
    int dci = (addr & 0xF) * 2 + ((addr & 0x80) ? 1 : 0), type, ivl = 0;
    int xfer = attr & 3;
    uint32_t *icc, *slot, *ep;
    if (dci < 2 || dci > 31) return -EINVAL;
    if (ring_init(d->hc, &d->ep[dci].ring) < 0) return -ENOMEM;
    d->ep[dci].dci = dci;
    d->ep[dci].mps = mps & 0x7FF;
    type = xfer == 2 ? ((addr & 0x80) ? 6 : 2) : xfer == 3 ? ((addr & 0x80) ? 7 : 3) : 0;
    if (!type) return -EINVAL;
    if (xfer == 3) {
        if (d->speed >= 3) ivl = interval ? interval - 1 : 0;
        else {                                   /* FS/LS: ms → 125 µs-এর ঘাত */
            int v = (interval ? interval : 1) * 8;
            while ((1 << (ivl + 1)) <= v && ivl < 10) ivl++;
            if (ivl < 3) ivl = 3;
        }
    }
    memset(d->in_ctx, 0, VMM_PAGE_SIZE);
    icc = in_ctx(d, 0);
    icc[1] = 1u | (1u << dci);                   /* add: slot + এই endpoint */
    slot = in_ctx(d, 1);
    memcpy(slot, d->out_ctx, d->hc->csz);         /* বর্তমান slot context */
    {
        uint32_t entries = (slot[0] >> 27) & 0x1F;
        if ((uint32_t)dci > entries) entries = (uint32_t)dci;
        slot[0] = (slot[0] & ~(0x1Fu << 27)) | (entries << 27);
    }
    slot[3] = 0;                                 /* slot state ইত্যাদি (output-only) মোছা */
    ep = in_ctx(d, dci + 1);
    ep[0] = (uint32_t)ivl << 16;
    ep[1] = (3u << 1) | ((uint32_t)type << 3) | ((uint32_t)(mps & 0x7FF) << 16);
    ep[2] = (uint32_t)d->ep[dci].ring.phys | 1u;
    ep[3] = (uint32_t)(d->ep[dci].ring.phys >> 32);
    ep[4] = (xfer == 3 ? ((uint32_t)(mps & 0x7FF) << 16) : 0) | (uint32_t)(xfer == 3 ? (mps & 0x7FF) : 3072);
    if (command(d->hc, (uint32_t)d->in_phys, (uint32_t)(d->in_phys >> 32), 0,
                (TRB_CONFIG_EP << 10) | ((uint32_t)d->slot << 24), NULL) != CC_SUCCESS) {
        return -EIO;
    }
    return dci;
}

/* ================= mass storage (পেন ড্রাইভ) ================= */

static int bot(struct usb_dev *d, const uint8_t *cdb, int cdblen, int in, uint32_t len)
{
    uint8_t *cbw = d->buf, *csw = d->buf + 512;
    uint64_t data_phys = d->buf_phys + VMM_PAGE_SIZE;
    int cc, tries;
    if (d->gone) return -ENODEV;
    memset(cbw, 0, 31);
    cbw[0] = 'U'; cbw[1] = 'S'; cbw[2] = 'B'; cbw[3] = 'C';
    d->tag++;
    memcpy(cbw + 4, &d->tag, 4);
    memcpy(cbw + 8, &len, 4);
    cbw[12] = in ? 0x80 : 0x00;
    cbw[13] = 0;                                 /* LUN 0 */
    cbw[14] = (uint8_t)cdblen;
    memcpy(cbw + 15, cdb, (size_t)cdblen);
    cc = transfer(d, d->bout, d->buf_phys, 31, 5000);
    if (cc == CC_STALL) { clear_halt(d, d->bout, d->bout_addr); return -EIO; }
    if (cc != CC_SUCCESS) return -EIO;
    if (len) {
        cc = transfer(d, in ? d->bin : d->bout, data_phys, len, 10000);
        if (cc == CC_STALL) clear_halt(d, in ? d->bin : d->bout, in ? d->bin_addr : d->bout_addr);
        else if (cc != CC_SUCCESS && cc != CC_SHORT) return -EIO;
    }
    for (tries = 0; tries < 2; tries++) {
        memset(csw, 0, 13);
        cc = transfer(d, d->bin, d->buf_phys + 512, 13, 5000);
        if (cc == CC_STALL) { clear_halt(d, d->bin, d->bin_addr); continue; }
        break;
    }
    if (cc != CC_SUCCESS && cc != CC_SHORT) return -EIO;
    if (csw[0] != 'U' || csw[1] != 'S' || csw[2] != 'B' || csw[3] != 'S') return -EIO;
    if (csw[12] != 0) return -EIO;              /* ১ = কমান্ড ব্যর্থ, ২ = phase error */
    return 0;
}

static uint8_t *msc_data(struct usb_dev *d) { return d->buf + VMM_PAGE_SIZE; }

static int scsi_rw(struct usb_dev *d, int write, uint64_t lba, uint32_t count, uint8_t *buf)
{
    uint32_t ss = d->bdev.sector_size, max = MSC_PAGES * VMM_PAGE_SIZE / ss;
    struct xhci *hc = d->hc;
    int err = 0;
    kmutex_lock(&hc->lock);
    while (count && !err) {
        uint32_t n = count < max ? count : max;
        uint8_t cdb[10];
        if (d->gone) { err = -ENODEV; break; }
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = write ? 0x2A : 0x28;
        cdb[2] = (uint8_t)(lba >> 24); cdb[3] = (uint8_t)(lba >> 16); cdb[4] = (uint8_t)(lba >> 8); cdb[5] = (uint8_t)lba;
        cdb[7] = (uint8_t)(n >> 8); cdb[8] = (uint8_t)n;
        if (write) memcpy(msc_data(d), buf, (size_t)n * ss);
        err = bot(d, cdb, 10, !write, n * ss);
        if (!err && !write) memcpy(buf, msc_data(d), (size_t)n * ss);
        buf += (size_t)n * ss;
        lba += n;
        count -= n;
    }
    kmutex_unlock(&hc->lock);
    return err;
}

static int msc_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buf)
{
    return scsi_rw((struct usb_dev *)dev->driver_data, 0, lba, count, (uint8_t *)buf);
}

static int msc_write(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf)
{
    return scsi_rw((struct usb_dev *)dev->driver_data, 1, lba, count, (uint8_t *)(uintptr_t)buf);
}

static int msc_flush(struct block_device *dev)
{
    struct usb_dev *d = (struct usb_dev *)dev->driver_data;
    uint8_t cdb[10];
    if (d->gone) return -ENODEV;
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x35;                               /* SYNCHRONIZE CACHE(10) */
    kmutex_lock(&d->hc->lock);
    bot(d, cdb, 10, 0, 0);                       /* না পারলেও সমস্যা নয় (অনেক পেন ড্রাইভ জানে না) */
    kmutex_unlock(&d->hc->lock);
    return 0;
}

static const struct block_ops msc_ops = { msc_read, msc_write, msc_flush };

static int msc_setup(struct usb_dev *d)
{
    uint8_t cdb[16], *r;
    int i, ok = 0;
    uint32_t last, bs;
    d->buf_phys = dma(d->hc, MSC_PAGES + 1);
    if (!d->buf_phys) return -ENOMEM;
    d->buf = (uint8_t *)pmm_phys_to_virt(d->buf_phys);
    r = msc_data(d);
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x12; cdb[4] = 36;                  /* INQUIRY */
    if (bot(d, cdb, 6, 1, 36) == 0) {
        char v[9], p[17];
        int k;
        memcpy(v, r + 8, 8); v[8] = '\0';
        memcpy(p, r + 16, 16); p[16] = '\0';
        for (k = 7; k >= 0 && v[k] == ' '; k--) v[k] = '\0';
        for (k = 15; k >= 0 && p[k] == ' '; k--) p[k] = '\0';
        ksnprintf(d->bdev.model, sizeof(d->bdev.model), "%s %s", v, p);
    }
    for (i = 0; i < 20 && !ok; i++) {            /* TEST UNIT READY — কার্ড/ড্রাইভ জেগে উঠতে সময় নেয় */
        memset(cdb, 0, sizeof(cdb));
        if (bot(d, cdb, 6, 0, 0) == 0) { ok = 1; break; }
        memset(cdb, 0, sizeof(cdb));
        cdb[0] = 0x03; cdb[4] = 18;              /* REQUEST SENSE */
        bot(d, cdb, 6, 1, 18);
        timer_sleep_ms(100);
    }
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x25;                               /* READ CAPACITY(10) */
    if (bot(d, cdb, 10, 1, 8) < 0) return -EIO;
    last = ((uint32_t)r[0] << 24) | ((uint32_t)r[1] << 16) | ((uint32_t)r[2] << 8) | r[3];
    bs = ((uint32_t)r[4] << 24) | ((uint32_t)r[5] << 16) | ((uint32_t)r[6] << 8) | r[7];
    if (bs != 512 && bs != 1024 && bs != 2048 && bs != 4096) return -EIO;
    if (last == 0 || last == 0xFFFFFFFFu) return -EIO;   /* খালি কার্ড রিডার / ২ TiB-এর বেশি */
    d->bdev.sector_size = bs;
    d->bdev.sector_count = (uint64_t)last + 1;
    d->bdev.ops = &msc_ops;
    d->bdev.driver_data = d;
    d->bdev.removable = 1;
    if (!d->bdev.model[0]) strcpy(d->bdev.model, "USB drive");
    if (block_register(&d->bdev, "sd") < 0) return -ENOMEM;
    d->registered = 1;
    kprintf("[usb] /dev/%s: %s, %llu MiB\n", d->bdev.name, d->bdev.model,
            (unsigned long long)(d->bdev.sector_count * bs / (1024 * 1024)));
    if (!booting) d->need_scan = 1;              /* partition পরে, usbd lock ছাড়ার পর */
    return 0;
}

/* প্রথম ext2/FAT (partition, না থাকলে পুরো ডিস্ক) → /mnt/usb (ব্যস্ত হলে /mnt/usb2 ...) */
static void msc_automount(struct usb_dev *d)
{
    size_t i;
    int n;
    d->mount_tried = 1;
    for (i = 0; i < block_device_count(); i++) {
        struct block_device *b = block_device_at(i);
        const char *fs;
        if (b != &d->bdev && b->parent != &d->bdev) continue;
        if (b == &d->bdev && d->bdev.partitions) continue;
        /* ধাপ ২৪খ: স্থায়ী জায়গা (আগেই / হিসেবে mount) বা অন্য কোথাও mount করা ডিভাইস নয় —
         * একই ext2 দুবার mount করলে নষ্ট হবে */
        if (!strcmp(b->part_label, "rozOS persist")) continue;
        {
            size_t k;
            int busy = 0;
            for (k = 0; k < vfs_mount_count(); k++) {
                const struct mount *m = vfs_mount_at(k);
                if (m && m->active && !strcmp(m->source, b->name)) busy = 1;
            }
            if (busy) continue;
        }
        fs = fs_driver_for(b);                   /* ধাপ ২৪গ: ext2, FAT, exFAT, NTFS (শুধু পড়া) */
        if (!fs) continue;
        for (n = 1; n < 10; n++) {
            char mp[24];
            if (n == 1) strcpy(mp, "/mnt/usb"); else ksnprintf(mp, sizeof(mp), "/mnt/usb%d", n);
            vfs_mkdir_p(mp);
            if (vfs_mount(mp, fs, b->name) == 0) {
                strcpy(d->mountpoint, mp);
                kprintf("[usb] /dev/%s (%s) mounted on %s - run `umount %s` before pulling it out\n",
                        b->name, fs, mp, mp);
                return;
            }
        }
        return;
    }
}

/* ================= HID (কিবোর্ড, মাউস) ================= */

/* HID usage → PS/2 set-1 scancode (0xE0xx = E0 আগে) */
static const uint16_t hid2ps2[0x65] = {
    0, 0, 0, 0, 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26,   /* 00-0F a-l */
    0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,   /* 10-1D m-z */
    0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,                           /* 1E-27 1-0 */
    0x1C, 0x01, 0x0E, 0x0F, 0x39, 0x0C, 0x0D, 0x1A, 0x1B, 0x2B, 0x2B, 0x27, 0x28, 0x29,   /* 28-35 */
    0x33, 0x34, 0x35, 0x3A,                                                               /* 36-39 */
    0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58,               /* 3A-45 F1-F12 */
    0, 0x46, 0,                                                                           /* 46-48 */
    0xE052, 0xE047, 0xE049, 0xE053, 0xE04F, 0xE051, 0xE04D, 0xE04B, 0xE050, 0xE048,       /* 49-52 */
    0x45, 0xE035, 0x37, 0x4A, 0x4E, 0xE01C, 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47,     /* 53-5F */
    0x48, 0x49, 0x52, 0x53, 0x56                                                          /* 60-64 */
};
static const uint16_t mod2ps2[8] = { 0x1D, 0x2A, 0x38, 0xE05B, 0xE01D, 0x36, 0xE038, 0xE05C };

static void send_sc(uint16_t sc, int release)
{
    if (!sc) return;
    if (sc & 0xFF00) keyboard_inject_scancode(0xE0);
    keyboard_inject_scancode((uint8_t)((sc & 0x7F) | (release ? 0x80 : 0)));
}

static void kbd_report(struct usb_dev *d, const uint8_t *r)
{
    int i, j;
    uint8_t changed = (uint8_t)(r[0] ^ d->last[0]);
    for (i = 0; i < 8; i++)
        if (changed & (1 << i)) send_sc(mod2ps2[i], !(r[0] & (1 << i)));
    for (i = 2; i < 8; i++) {                    /* ছাড়া */
        int still = 0;
        if (d->last[i] < 4) continue;
        for (j = 2; j < 8; j++) if (r[j] == d->last[i]) still = 1;
        if (!still) {
            if (d->last[i] < 0x65) send_sc(hid2ps2[d->last[i]], 1);
            if (d->held == d->last[i]) d->held = 0;
        }
    }
    for (i = 2; i < 8; i++) {                    /* নতুন চাপা */
        int was = 0;
        if (r[i] < 4 || r[i] >= 0x65) continue;   /* 1 = rollover error */
        for (j = 2; j < 8; j++) if (d->last[j] == r[i]) was = 1;
        if (!was) {
            send_sc(hid2ps2[r[i]], 0);
            d->held = r[i];
            d->held_at = timer_uptime_ms();
            d->repeat_at = d->held_at + 500;
        }
    }
    memcpy(d->last, r, 8);
}

static void hid_service(struct usb_dev *d)
{
    struct usb_ep *ep = &d->ep[d->iin];
    uint8_t *rep = d->ctl + 2048;
    uint64_t now = timer_uptime_ms();
    if (d->kind == 2 && d->held && now >= d->repeat_at) {   /* কী ধরে রাখলে পুনরাবৃত্তি */
        send_sc(hid2ps2[d->held], 0);
        d->repeat_at = now + 33;
    }
    if (!d->queued) return;
    if (!ep->done) return;
    d->queued = 0;
    if (ep->cc == CC_SUCCESS || ep->cc == CC_SHORT) {
        if (d->kind == 2) kbd_report(d, rep);
        else {
            int8_t dx = (int8_t)rep[1], dy = (int8_t)rep[2], wh = ep->mps > 3 ? (int8_t)rep[3] : 0;
            mouse_inject(dx, dy, rep[0] & 7, -wh);
        }
    } else if (ep->cc == CC_STALL) {
        clear_halt(d, d->iin, (uint8_t)(0x80 | (d->iin / 2)));
    }
    memset(rep, 0, 8);
    transfer(d, d->iin, d->ctl_phys + 2048, (uint32_t)(ep->mps > 8 ? 8 : ep->mps), 0);
    d->queued = 1;
}

/* ================= ডিভাইস চেনা ================= */

static const char *speed_name(int s)
{
    return s == 1 ? "12 Mb/s" : s == 2 ? "1.5 Mb/s" : s == 3 ? "480 Mb/s" : s == 4 ? "5 Gb/s" : s >= 5 ? "10 Gb/s" : "?";
}

static int port_reset(struct xhci *hc, int p)
{
    uint32_t v = r32(hc->op, OP_PORTSC(p));
    uint64_t end;
    if (!(v & PORTSC_CCS)) return -ENODEV;
    if (v & PORTSC_PED) return 0;                /* USB 3: নিজেই চালু */
    w32(hc->op, OP_PORTSC(p), (v & ~(PORTSC_CHANGE | PORTSC_PED)) | PORTSC_PR);
    end = timer_uptime_ms() + 500;
    while (!(r32(hc->op, OP_PORTSC(p)) & PORTSC_PRC)) {
        if (timer_uptime_ms() > end) return -ETIMEDOUT;
        cpu_pause();
    }
    v = r32(hc->op, OP_PORTSC(p));
    w32(hc->op, OP_PORTSC(p), (v & ~(PORTSC_CHANGE | PORTSC_PED)) | PORTSC_PRC);
    timer_sleep_ms(20);
    return (r32(hc->op, OP_PORTSC(p)) & PORTSC_PED) ? 0 : -EIO;
}

static void utf16_to_ascii(char *out, size_t size, const uint8_t *s, int bytes)
{
    size_t o = 0;
    int i;
    for (i = 2; i + 1 < bytes && o + 1 < size; i += 2) out[o++] = (s[i + 1] || s[i] < 32 || s[i] > 126) ? '?' : (char)s[i];
    out[o] = '\0';
}

static int enumerate(struct xhci *hc, int port)
{
    struct usb_dev *d = NULL;
    uint32_t slot = 0, *ctx;
    uint8_t desc[18], cfg9[9], *cfg;
    uint16_t total;
    int i, cc, off;
    uint8_t cfgval;

    for (i = 0; i < MAX_DEVS; i++) if (!devs[i].used) { d = &devs[i]; break; }
    if (!d) return -ENOMEM;
    if (port_reset(hc, port) < 0) return -EIO;
    memset(d, 0, sizeof(*d));
    d->hc = hc;
    d->port = port;
    d->speed = (int)((r32(hc->op, OP_PORTSC(port)) >> 10) & 0xF);
    cc = command(hc, 0, 0, 0, TRB_ENABLE_SLOT << 10, &slot);
    if (cc != CC_SUCCESS || !slot || slot > hc->max_slots) return -EIO;
    d->slot = (int)slot;
    d->used = 1;
    d->out_phys = dma(hc, 1);
    d->in_phys = dma(hc, 1);
    d->ctl_phys = dma(hc, 1);
    if (!d->out_phys || !d->in_phys || !d->ctl_phys || ring_init(hc, &d->ep0.ring) < 0) goto fail;
    d->out_ctx = (uint8_t *)pmm_phys_to_virt(d->out_phys);
    d->in_ctx = (uint8_t *)pmm_phys_to_virt(d->in_phys);
    d->ctl = (uint8_t *)pmm_phys_to_virt(d->ctl_phys);
    hc->dcbaa[slot] = d->out_phys;
    hc->slot_dev[slot] = d;
    d->ep0.dci = 1;
    d->ep0.mps = ep0_mps_for(d->speed);

    /* Address Device */
    ctx = in_ctx(d, 0);
    ctx[1] = 3;                                  /* add: slot + EP0 */
    ctx = in_ctx(d, 1);
    ctx[0] = ((uint32_t)d->speed << 20) | (1u << 27);
    ctx[1] = (uint32_t)port << 16;
    ctx = in_ctx(d, 2);
    ctx[1] = (3u << 1) | (4u << 3) | ((uint32_t)d->ep0.mps << 16);
    ctx[2] = (uint32_t)d->ep0.ring.phys | 1u;
    ctx[3] = (uint32_t)(d->ep0.ring.phys >> 32);
    ctx[4] = 8;
    if (command(hc, (uint32_t)d->in_phys, (uint32_t)(d->in_phys >> 32), 0,
                (TRB_ADDRESS << 10) | (slot << 24), NULL) != CC_SUCCESS) goto fail;
    timer_sleep_ms(10);

    /* প্রথম ৮ বাইট — EP0-র আসল packet মাপ */
    if (get_desc(d, 1, 0, 0, desc, 8) < 0) goto fail;
    {
        int mps = d->speed >= 4 ? (1 << desc[7]) : desc[7];
        if (mps >= 8 && mps != d->ep0.mps) {     /* Evaluate Context */
            d->ep0.mps = mps;
            memset(d->in_ctx, 0, VMM_PAGE_SIZE);
            in_ctx(d, 0)[1] = 2;
            ctx = in_ctx(d, 2);
            ctx[1] = (3u << 1) | (4u << 3) | ((uint32_t)mps << 16);
            command(hc, (uint32_t)d->in_phys, (uint32_t)(d->in_phys >> 32), 0, (TRB_EVAL_CTX << 10) | (slot << 24), NULL);
        }
    }
    if (get_desc(d, 1, 0, 0, desc, 18) < 0) goto fail;
    d->vid = (uint16_t)(desc[8] | (desc[9] << 8));
    d->pid = (uint16_t)(desc[10] | (desc[11] << 8));
    if (desc[15]) {                              /* নাম (string descriptor) */
        uint8_t s[64];
        if (get_desc(d, 3, desc[15], 0x0409, s, 2) == 0 && s[0] >= 4 && s[0] <= 64 &&
            get_desc(d, 3, desc[15], 0x0409, s, s[0]) == 0)
            utf16_to_ascii(d->product, sizeof(d->product), s, s[0]);
    }
    if (get_desc(d, 2, 0, 0, cfg9, 9) < 0) goto fail;
    total = (uint16_t)(cfg9[2] | (cfg9[3] << 8));
    if (total > 2048) total = 2048;
    cfg = (uint8_t *)kmalloc(total);
    if (!cfg) goto fail;
    if (get_desc(d, 2, 0, 0, cfg, total) < 0) { kfree(cfg); goto fail; }
    cfgval = cfg[5];

    /* প্রথম চেনা interface খোঁজা (alt 0) */
    {
        for (off = 0; off + 2 <= total && cfg[off] >= 2 && !d->kind; off += cfg[off]) {
            uint8_t *p = cfg + off;
            if (p[1] != 4 || p[3] != 0) continue;  /* interface descriptor, alt 0 */
            if (p[5] == 8 && p[6] == 6 && p[7] == 0x50) d->kind = 1;          /* SCSI, Bulk-Only */
            else if (p[5] == 3 && p[6] == 1 && (p[7] == 1 || p[7] == 2)) d->kind = p[7] == 1 ? 2 : 3;
            if (d->kind) { d->cls = p[5]; d->sub = p[6]; d->proto = p[7]; d->iface = p[2]; }
        }
        if (!d->kind) {
            d->cls = desc[4];
            kfree(cfg);
            control(d, 0x00, 9, cfgval, 0, 0, NULL);
            kprintf("[usb] port %d: %04x:%04x \"%s\" (class %02x) - no driver yet\n", port, d->vid, d->pid,
                    d->product, d->cls);
            hc->port_dev[port] = d;
            return 0;
        }
        if (control(d, 0x00, 9, cfgval, 0, 0, NULL) < 0) { kfree(cfg); goto fail; }
        /* ঐ interface-এর endpoint চালু */
        {
            int in_if = 0;
            for (off = 0; off + 2 <= total && cfg[off] >= 2; off += cfg[off]) {
                uint8_t *p = cfg + off;
                if (p[1] == 4) in_if = p[2] == d->iface && p[3] == 0;
                else if (p[1] == 5 && in_if) {
                    uint8_t addr = p[2], attr = p[3], ivl = p[6];
                    uint16_t mps = (uint16_t)(p[4] | (p[5] << 8));
                    int dci;
                    if (d->kind == 1 && (attr & 3) == 2) {
                        dci = add_endpoint(d, addr, attr, mps, 0);
                        if (dci < 0) { kfree(cfg); goto fail; }
                        if (addr & 0x80) { d->bin = dci; d->bin_addr = addr; }
                        else { d->bout = dci; d->bout_addr = addr; }
                    } else if (d->kind >= 2 && (attr & 3) == 3 && (addr & 0x80) && !d->iin) {
                        dci = add_endpoint(d, addr, attr, mps, ivl);
                        if (dci < 0) { kfree(cfg); goto fail; }
                        d->iin = dci;
                    }
                }
            }
        }
    }
    kfree(cfg);
    hc->port_dev[port] = d;
    if (d->kind == 1) {
        if (!d->bin || !d->bout || msc_setup(d) < 0) {
            kprintf("[usb] port %d: storage %04x:%04x did not answer (no medium?)\n", port, d->vid, d->pid);
            return 0;
        }
    } else {
        control(d, 0x21, 0x0B, 0, d->iface, 0, NULL);   /* SET_PROTOCOL(boot) */
        control(d, 0x21, 0x0A, 0, d->iface, 0, NULL);   /* SET_IDLE(0) */
        if (d->iin) {
            transfer(d, d->iin, d->ctl_phys + 2048, (uint32_t)(d->ep[d->iin].mps > 8 ? 8 : d->ep[d->iin].mps), 0);
            d->queued = 1;
        }
        kprintf("[usb] port %d: USB %s \"%s\" (%s)\n", port, d->kind == 2 ? "keyboard" : "mouse",
                d->product[0] ? d->product : "?", speed_name(d->speed));
    }
    return 0;
fail:
    kprintf("[usb] port %d: device did not set up (speed %s)\n", port, speed_name(d->speed));
    if (d->slot) {
        command(hc, 0, 0, 0, (TRB_DISABLE_SLOT << 10) | ((uint32_t)d->slot << 24), NULL);
        hc->slot_dev[d->slot] = NULL;
        hc->dcbaa[d->slot] = 0;
    }
    d->used = 0;
    return -EIO;
}

static void detach(struct xhci *hc, int port)
{
    struct usb_dev *d = hc->port_dev[port];
    hc->port_dev[port] = NULL;
    if (!d) return;
    d->gone = 1;
    if (d->kind == 1 && d->registered) {
        size_t k;
        int still = 0;
        d->bdev.offline = 1;
        /* /dev থেকে ডিস্ক আর তার partition সরানো */
        for (k = 0; k < block_device_count(); k++) {
            struct block_device *b = block_device_at(k);
            if (b == &d->bdev || b->parent == &d->bdev) devfs_unregister(b->name);
        }
        (void)still;
        d->need_umount = 1;                      /* usbd: lock-এর বাইরে umount */
    } else if (d->kind >= 2) {
        kprintf("[usb] %s removed\n", d->kind == 2 ? "keyboard" : "mouse");
    }
    command(hc, 0, 0, 0, (TRB_DISABLE_SLOT << 10) | ((uint32_t)d->slot << 24), NULL);
    hc->slot_dev[d->slot] = NULL;
    hc->dcbaa[d->slot] = 0;
    /* usb_dev আর bdev থেকে যায় (block স্তর এখনো pointer রাখে) — ঘর আবার ব্যবহার হয় না
     * শুধু HID-এর জন্য */
    if (d->kind != 1) d->used = 0;
}

static void scan_ports(struct xhci *hc)
{
    uint32_t p;
    for (p = 1; p <= hc->max_ports; p++) {
        uint32_t v = r32(hc->op, OP_PORTSC(p));
        if (v & PORTSC_CHANGE) w32(hc->op, OP_PORTSC(p), (v & ~(PORTSC_CHANGE | PORTSC_PED)) | (v & PORTSC_CHANGE));
        if ((v & PORTSC_CCS) && !hc->port_dev[p] && !hc->port_failed[p]) {
            timer_sleep_ms(50);                  /* লাগানোর ঝাঁকুনি থামুক */
            if (enumerate(hc, (int)p) < 0) hc->port_failed[p] = 1;
        } else if (!(v & PORTSC_CCS)) {
            hc->port_failed[p] = 0;
            if (hc->port_dev[p]) detach(hc, (int)p);
        }
    }
}

/* ================= কন্ট্রোলার চালু ================= */

static void bios_handoff(struct xhci *hc)
{
    uint32_t off = (r32(hc->base, CAP_HCCP1) >> 16) * 4;
    while (off) {
        uint32_t v = r32(hc->base, off);
        if ((v & 0xFF) == 1) {                   /* USB Legacy Support */
            uint64_t end = timer_uptime_ms() + 1000;
            w32(hc->base, off, v | (1u << 24));  /* OS owned */
            while ((r32(hc->base, off) & (1u << 16)) && timer_uptime_ms() < end) cpu_pause();
            w32(hc->base, off + 4, 0);           /* BIOS-এর SMI বন্ধ */
            return;
        }
        if (!((v >> 8) & 0xFF)) return;
        off += ((v >> 8) & 0xFF) * 4;
    }
}

static int hc_setup(struct xhci *hc, struct pci_device *pd)
{
    uint32_t hcs1, hcs2, hcc1, nsp, i;
    uint64_t end, sp_arr;
    pci_enable_device(pd);
    hc->base = (volatile uint8_t *)vmm_map_mmio(pd->bar[0], 0x10000);
    if (!hc->base) return -EIO;
    hcs1 = r32(hc->base, CAP_HCSP1);
    hcs2 = r32(hc->base, CAP_HCSP2);
    hcc1 = r32(hc->base, CAP_HCCP1);
    hc->op = hc->base + (r32(hc->base, CAP_CAPLENGTH) & 0xFF);
    hc->rt = hc->base + (r32(hc->base, CAP_RTSOFF) & ~0x1Fu);
    hc->db = (volatile uint32_t *)(hc->base + (r32(hc->base, CAP_DBOFF) & ~0x3u));
    hc->max_slots = hcs1 & 0xFF;
    if (hc->max_slots > 64) hc->max_slots = 64;
    hc->max_ports = (hcs1 >> 24) & 0xFF;
    hc->csz = (hcc1 & 4) ? 64 : 32;
    hc->ac64 = hcc1 & 1;
    bios_handoff(hc);

    /* থামানো, তারপর reset */
    w32(hc->op, OP_USBCMD, r32(hc->op, OP_USBCMD) & ~1u);
    end = timer_uptime_ms() + 1000;
    while (!(r32(hc->op, OP_USBSTS) & 1) && timer_uptime_ms() < end) cpu_pause();
    w32(hc->op, OP_USBCMD, 2);
    end = timer_uptime_ms() + 1000;
    while ((r32(hc->op, OP_USBCMD) & 2) || (r32(hc->op, OP_USBSTS) & (1u << 11))) {
        if (timer_uptime_ms() > end) return -ETIMEDOUT;
        cpu_pause();
    }
    w32(hc->op, OP_CONFIG, hc->max_slots);

    hc->dcbaa_phys = dma(hc, 1);
    if (!hc->dcbaa_phys) return -ENOMEM;
    hc->dcbaa = (uint64_t *)pmm_phys_to_virt(hc->dcbaa_phys);
    nsp = ((hcs2 >> 27) & 0x1F) | (((hcs2 >> 21) & 0x1F) << 5);
    if (nsp) {                                   /* scratchpad: কন্ট্রোলারের নিজের মেমোরি */
        uint64_t *arr;
        sp_arr = dma(hc, 1);
        if (!sp_arr) return -ENOMEM;
        arr = (uint64_t *)pmm_phys_to_virt(sp_arr);
        for (i = 0; i < nsp && i < 512; i++) {
            arr[i] = dma(hc, 1);
            if (!arr[i]) return -ENOMEM;
        }
        hc->dcbaa[0] = sp_arr;
    }
    w64(hc->op, OP_DCBAAP, hc->dcbaa_phys);

    if (ring_init(hc, &hc->cmd) < 0) return -ENOMEM;
    w64(hc->op, OP_CRCR, hc->cmd.phys | 1u);

    hc->ev_phys = dma(hc, 1);
    hc->erst_phys = dma(hc, 1);
    if (!hc->ev_phys || !hc->erst_phys) return -ENOMEM;
    hc->ev = (volatile struct trb *)pmm_phys_to_virt(hc->ev_phys);
    {
        uint32_t *erst = (uint32_t *)pmm_phys_to_virt(hc->erst_phys);
        erst[0] = (uint32_t)hc->ev_phys;
        erst[1] = (uint32_t)(hc->ev_phys >> 32);
        erst[2] = EV_TRBS;
    }
    hc->ev_deq = 0;
    hc->ev_ccs = 1;
    w32(hc->rt, 0x20 + 0x08, 1);                 /* ERSTSZ */
    w64(hc->rt, 0x20 + 0x18, hc->ev_phys);       /* ERDP */
    w64(hc->rt, 0x20 + 0x10, hc->erst_phys);     /* ERSTBA (শেষে) */
    w32(hc->rt, 0x20 + 0x00, 0);                 /* IMAN: ইন্টারাপ্ট বন্ধ — polling */

    w32(hc->op, OP_USBCMD, 1);                   /* চালু */
    end = timer_uptime_ms() + 1000;
    while (r32(hc->op, OP_USBSTS) & 1) {
        if (timer_uptime_ms() > end) return -ETIMEDOUT;
        cpu_pause();
    }
    for (i = 1; i <= hc->max_ports; i++) {       /* port-এ বিদ্যুৎ */
        uint32_t v = r32(hc->op, OP_PORTSC(i));
        if (!(v & PORTSC_PP)) w32(hc->op, OP_PORTSC(i), (v & ~(PORTSC_CHANGE | PORTSC_PED)) | PORTSC_PP);
    }
    hc->lock = (struct kmutex)KMUTEX_INIT("xhci");
    return 0;
}

/* usbd: ২০ ms পরপর (কিবোর্ড/মাউস থাকলে ৮ ms) — রিপোর্ট, লাগানো/খোলা, auto-mount */
static int usbd(void *arg)
{
    uint64_t last_scan = 0;
    (void)arg;
    for (;;) {
        int h, i, hid = 0;
        uint64_t now = timer_uptime_ms();
        for (h = 0; h < nhc; h++) {
            struct xhci *hc = &hcs[h];
            kmutex_lock(&hc->lock);
            handle_events(hc);
            for (i = 0; i < MAX_DEVS; i++) {
                struct usb_dev *d = &devs[i];
                if (!d->used || d->gone || d->hc != hc) continue;
                if (d->kind >= 2) { hid = 1; hid_service(d); }
            }
            if (now - last_scan >= 300) scan_ports(hc);
            kmutex_unlock(&hc->lock);
        }
        if (now - last_scan >= 300) last_scan = now;
        for (i = 0; i < MAX_DEVS; i++) {             /* lock-এর বাইরের কাজ */
            struct usb_dev *d = &devs[i];
            if (d->used && d->need_scan) {
                int parts;
                d->need_scan = 0;
                parts = partition_scan(&d->bdev);
                if (parts > 0) kprintf("[usb] /dev/%s: %d partition(s)\n", d->bdev.name, parts);
            }
            if (d->used && d->need_umount) {
                size_t k;
                int still = 0;
                d->need_umount = 0;
                for (k = 0; d->mountpoint[0] && k < vfs_mount_count(); k++) {
                    const struct mount *m = vfs_mount_at(k);
                    if (m && !strcmp(m->path, d->mountpoint) && !strncmp(m->source, d->bdev.name, strlen(d->bdev.name))) still = 1;
                }
                if (!still) kprintf("[usb] /dev/%s removed\n", d->bdev.name);
                else if (vfs_umount(d->mountpoint) == 0)
                    kprintf("[usb] /dev/%s removed - %s unmounted\n", d->bdev.name, d->mountpoint);
                else
                    kprintf("[usb] /dev/%s removed while files were open on %s! (umount it next time)\n",
                            d->bdev.name, d->mountpoint);
            }
        }
        if (fs_ready) {
            for (i = 0; i < MAX_DEVS; i++) {
                struct usb_dev *d = &devs[i];
                if (d->used && !d->gone && d->kind == 1 && d->registered && !d->mount_tried && !d->need_scan) msc_automount(d);
            }
        }
        timer_sleep_ms(hid ? 8 : 20);
    }
    return 0;
}

void usb_storage_ready(void)
{
    fs_ready = 1;
}

int usb_init(void)
{
    size_t n;
    int i;
    nhc = 0;
    for (n = 0; n < MAX_HC; n++) {
        struct pci_device *pd = pci_find_class(0x0C, 0x03, 0x30, n);
        struct xhci *hc;
        if (!pd) break;
        if (!pd->bar[0] || pd->bar_is_io[0]) continue;
        hc = &hcs[nhc];
        memset(hc, 0, sizeof(*hc));
        hc->index = nhc;
        if (hc_setup(hc, pd) < 0) {
            kprintf("[usb] xHCI controller %u did not start\n", (unsigned)n);
            continue;
        }
        nhc++;
    }
    if (!nhc) return 0;
    timer_sleep_ms(150);                         /* port-এর link তৈরি হতে সময় */
    for (i = 0; i < nhc; i++) {
        kmutex_lock(&hcs[i].lock);
        scan_ports(&hcs[i]);
        kmutex_unlock(&hcs[i].lock);
    }
    booting = 0;
    sched_create_kthread("usbd", usbd, NULL);
    return nhc;
}

int64_t usb_list(char *buf, size_t len)
{
    size_t o = 0;
    int i;
    if (!len) return 0;
    buf[0] = '\0';
    for (i = 0; i < MAX_DEVS && o + 1 < len; i++) {
        struct usb_dev *d = &devs[i];
        const char *k;
        if (!d->used || d->gone) continue;
        k = d->kind == 1 ? "storage" : d->kind == 2 ? "keyboard" : d->kind == 3 ? "mouse" : "other";
        o += (size_t)ksnprintf(buf + o, len - o, "%d %d %04x:%04x %s %s %s %s\n", d->hc->index, d->port, d->vid,
                               d->pid, speed_name(d->speed), k,
                               d->kind == 1 && d->registered ? d->bdev.name : "-",
                               d->product[0] ? d->product : (d->kind == 1 ? d->bdev.model : "?"));
    }
    if (o >= len) o = len - 1;
    return (int64_t)o;
}
