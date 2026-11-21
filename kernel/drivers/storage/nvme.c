/* rozOS — NVMe ড্রাইভার (ধাপ ১৫ক), rozos/nvme.h দেখুন
 *
 * ধাপ: কন্ট্রোলার বন্ধ → admin queue বসানো → চালু → IDENTIFY (controller, namespace 1) →
 * I/O completion queue আর submission queue তৈরি → READ/WRITE/FLUSH কমান্ড।
 * প্রতিটা কমান্ড: submission queue-তে ৬৪ বাইট লিখে doorbell; completion queue-তে phase
 * বিট বদলানো পর্যন্ত অপেক্ষা (polling), তারপর head doorbell। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/nvme.h>
#include <rozos/block.h>
#include <rozos/pci.h>
#include <rozos/pmm.h>
#include <rozos/vmm.h>
#include <rozos/heap.h>
#include <rozos/timer.h>
#include <rozos/errno.h>
#include <rozos/string.h>
#include <rozos/sched.h>
#include <arch/x86_64/cpu.h>

#define REG_CAP    0x00
#define REG_VS     0x08
#define REG_CC     0x14
#define REG_CSTS   0x1C
#define REG_AQA    0x24
#define REG_ASQ    0x28
#define REG_ACQ    0x30

#define QDEPTH        64
#define BOUNCE_PAGES  32                 /* ১২৮ KiB — এক কমান্ডে সর্বোচ্চ */

struct nvme_cmd {
    uint32_t cdw0, nsid, rsv[2];
    uint64_t mptr, prp1, prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
};
struct nvme_cpl {
    uint32_t dw0, dw1;
    uint16_t sq_head, sq_id, cid, status;
};

struct nvme_queue {
    struct nvme_cmd *sq;
    volatile struct nvme_cpl *cq;
    uint64_t sq_phys, cq_phys;
    uint16_t tail, head, phase, qid;
};

struct nvme_ctrl {
    volatile uint8_t *regs;
    uint32_t dstrd;                      /* doorbell-এর ফাঁক (বাইট) */
    uint64_t timeout_ms;
    struct nvme_queue admin, io;
    uint16_t cid;
    uint8_t *bounce;
    uint64_t bounce_phys, prp_list_phys;
    uint64_t *prp_list;
    uint32_t nsid, max_xfer_pages;
    struct block_device bdev;
    struct kmutex lock;
};

static struct nvme_ctrl ctrls[NVME_MAX_CTRL];
static struct nvme_info info;

static inline uint32_t rd32(struct nvme_ctrl *c, uint32_t r) { return *(volatile uint32_t *)(c->regs + r); }
static inline void wr32(struct nvme_ctrl *c, uint32_t r, uint32_t v) { *(volatile uint32_t *)(c->regs + r) = v; }
static inline uint64_t rd64(struct nvme_ctrl *c, uint32_t r) { return (uint64_t)rd32(c, r) | ((uint64_t)rd32(c, r + 4) << 32); }
static inline void wr64(struct nvme_ctrl *c, uint32_t r, uint64_t v) { wr32(c, r, (uint32_t)v); wr32(c, r + 4, (uint32_t)(v >> 32)); }

static void sq_doorbell(struct nvme_ctrl *c, struct nvme_queue *q)
{
    wr32(c, 0x1000 + (2 * q->qid) * c->dstrd, q->tail);
}

static void cq_doorbell(struct nvme_ctrl *c, struct nvme_queue *q)
{
    wr32(c, 0x1000 + (2 * q->qid + 1) * c->dstrd, q->head);
}

static int wait_ready(struct nvme_ctrl *c, int want)
{
    uint64_t end = timer_uptime_ms() + c->timeout_ms;
    while (((rd32(c, REG_CSTS) & 1) != (uint32_t)want)) {
        if (rd32(c, REG_CSTS) & 2) return -EIO;          /* fatal status */
        if (timer_uptime_ms() > end) return -ETIMEDOUT;
        cpu_pause();
    }
    return 0;
}

/* একটা কমান্ড পাঠিয়ে উত্তরের অপেক্ষা; ফেরত: 0 বা -EIO (status-সহ dw0 out-এ) */
static int submit(struct nvme_ctrl *c, struct nvme_queue *q, struct nvme_cmd *cmd, uint32_t *dw0)
{
    uint64_t end;
    uint16_t cid = ++c->cid;
    volatile struct nvme_cpl *e;
    cmd->cdw0 = (cmd->cdw0 & 0xFFFF) | ((uint32_t)cid << 16);
    q->sq[q->tail] = *cmd;
    q->tail = (uint16_t)((q->tail + 1) % QDEPTH);
    __asm__ __volatile__("" : : : "memory");
    sq_doorbell(c, q);
    end = timer_uptime_ms() + c->timeout_ms + 1000;
    for (;;) {
        e = &q->cq[q->head];
        if ((e->status & 1) == q->phase) break;
        if (timer_uptime_ms() > end) return -ETIMEDOUT;
        cpu_pause();
    }
    if (dw0) *dw0 = e->dw0;
    {
        uint16_t st = (uint16_t)(e->status >> 1);
        q->head = (uint16_t)((q->head + 1) % QDEPTH);
        if (q->head == 0) q->phase ^= 1;
        cq_doorbell(c, q);
        return st ? -EIO : 0;
    }
}

static uint64_t dma_pages(uint64_t n)
{
    uint64_t p = pmm_alloc_contiguous(n, 0);
    if (p) memset(pmm_phys_to_virt(p), 0, (size_t)(n * VMM_PAGE_SIZE));
    return p;
}

/* bounce buffer-এর প্রথম `bytes` বাইটের জন্য PRP1/PRP2 */
static void set_prps(struct nvme_ctrl *c, struct nvme_cmd *cmd, uint64_t bytes)
{
    uint64_t pages = (bytes + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE, i;
    cmd->prp1 = c->bounce_phys;
    if (pages <= 1) cmd->prp2 = 0;
    else if (pages == 2) cmd->prp2 = c->bounce_phys + VMM_PAGE_SIZE;
    else {
        for (i = 1; i < pages; i++) c->prp_list[i - 1] = c->bounce_phys + i * VMM_PAGE_SIZE;
        cmd->prp2 = c->prp_list_phys;
    }
}

static int rw(struct nvme_ctrl *c, int write, uint64_t lba, uint32_t count, uint8_t *buf)
{
    uint32_t ss = c->bdev.sector_size, max = c->max_xfer_pages * VMM_PAGE_SIZE / ss;
    int err = 0;
    kmutex_lock(&c->lock);
    while (count && !err) {
        uint32_t n = count < max ? count : max;
        struct nvme_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.cdw0 = write ? 0x01 : 0x02;
        cmd.nsid = c->nsid;
        cmd.cdw10 = (uint32_t)lba;
        cmd.cdw11 = (uint32_t)(lba >> 32);
        cmd.cdw12 = n - 1;
        set_prps(c, &cmd, (uint64_t)n * ss);
        if (write) memcpy(c->bounce, buf, (size_t)n * ss);
        err = submit(c, &c->io, &cmd, NULL);
        if (!err && !write) memcpy(buf, c->bounce, (size_t)n * ss);
        buf += (size_t)n * ss;
        lba += n;
        count -= n;
    }
    kmutex_unlock(&c->lock);
    return err;
}

static int nvme_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buf)
{
    return rw((struct nvme_ctrl *)dev->driver_data, 0, lba, count, (uint8_t *)buf);
}

static int nvme_write(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf)
{
    return rw((struct nvme_ctrl *)dev->driver_data, 1, lba, count, (uint8_t *)(uintptr_t)buf);
}

static int nvme_flush(struct block_device *dev)
{
    struct nvme_ctrl *c = (struct nvme_ctrl *)dev->driver_data;
    struct nvme_cmd cmd;
    int err;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0 = 0x00;
    cmd.nsid = c->nsid;
    kmutex_lock(&c->lock);
    err = submit(c, &c->io, &cmd, NULL);
    kmutex_unlock(&c->lock);
    return err;
}

static const struct block_ops nvme_ops = { nvme_read, nvme_write, nvme_flush };

static void copy_trim(char *dst, const uint8_t *src, size_t n)
{
    size_t i;
    memcpy(dst, src, n);
    dst[n] = '\0';
    for (i = n; i > 0 && (dst[i - 1] == ' ' || dst[i - 1] == '\0'); i--) dst[i - 1] = '\0';
}

static int alloc_queue(struct nvme_queue *q, uint16_t qid)
{
    memset(q, 0, sizeof(*q));
    q->sq_phys = dma_pages(1);
    q->cq_phys = dma_pages(1);
    if (!q->sq_phys || !q->cq_phys) return -ENOMEM;
    q->sq = (struct nvme_cmd *)pmm_phys_to_virt(q->sq_phys);
    q->cq = (volatile struct nvme_cpl *)pmm_phys_to_virt(q->cq_phys);
    q->phase = 1;
    q->qid = qid;
    return 0;
}

static int setup(struct nvme_ctrl *c, struct pci_device *d)
{
    uint64_t cap;
    uint32_t mpsmin, mdts;
    struct nvme_cmd cmd;
    uint8_t *id;
    uint32_t flbas, lbads;
    uint64_t nsze;

    pci_enable_device(d);
    c->regs = (volatile uint8_t *)vmm_map_mmio(d->bar[0], 0x2000);
    if (!c->regs) return -EIO;
    cap = rd64(c, REG_CAP);
    c->dstrd = 4u << ((cap >> 32) & 0xF);
    c->timeout_ms = ((cap >> 24) & 0xFF) * 500 + 500;
    mpsmin = (uint32_t)((cap >> 48) & 0xF);
    if (mpsmin != 0) return -ENODEV;             /* 4 KiB পেজ চলে না এমন কন্ট্রোলার (খুব বিরল) */
    info.version = rd32(c, REG_VS);
    if (c->dstrd * 4 + 0x1000 > 0x2000) c->regs = (volatile uint8_t *)vmm_map_mmio(d->bar[0], 0x1000 + c->dstrd * 8);

    /* ১) বন্ধ */
    wr32(c, REG_CC, rd32(c, REG_CC) & ~1u);
    if (wait_ready(c, 0) < 0) return -EIO;
    /* ২) admin queue */
    if (alloc_queue(&c->admin, 0) < 0) return -ENOMEM;
    wr32(c, REG_AQA, ((QDEPTH - 1) << 16) | (QDEPTH - 1));
    wr64(c, REG_ASQ, c->admin.sq_phys);
    wr64(c, REG_ACQ, c->admin.cq_phys);
    /* ৩) চালু: IOSQES 6 (৬৪ বাইট), IOCQES 4 (১৬ বাইট), NVM command set, ৪ KiB পেজ */
    wr32(c, REG_CC, (6u << 16) | (4u << 20) | 1u);
    if (wait_ready(c, 1) < 0) return -EIO;

    c->bounce_phys = dma_pages(BOUNCE_PAGES);
    c->prp_list_phys = dma_pages(1);
    if (!c->bounce_phys || !c->prp_list_phys) return -ENOMEM;
    c->bounce = (uint8_t *)pmm_phys_to_virt(c->bounce_phys);
    c->prp_list = (uint64_t *)pmm_phys_to_virt(c->prp_list_phys);
    id = c->bounce;

    /* ৪) IDENTIFY controller */
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0 = 0x06;
    cmd.prp1 = c->bounce_phys;
    cmd.cdw10 = 1;
    if (submit(c, &c->admin, &cmd, NULL) < 0) return -EIO;
    copy_trim(c->bdev.model, id + 24, 40);
    if (!info.model[0]) memcpy(info.model, c->bdev.model, sizeof(info.model));
    mdts = id[77];                               /* সর্বোচ্চ transfer = 2^mdts পেজ (0 = সীমা নেই) */
    c->max_xfer_pages = BOUNCE_PAGES;
    if (mdts && (1u << mdts) < c->max_xfer_pages) c->max_xfer_pages = 1u << mdts;
    if (c->max_xfer_pages > 512) c->max_xfer_pages = 512;

    /* ৫) IDENTIFY namespace 1 */
    c->nsid = 1;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0 = 0x06;
    cmd.nsid = c->nsid;
    cmd.prp1 = c->bounce_phys;
    cmd.cdw10 = 0;
    if (submit(c, &c->admin, &cmd, NULL) < 0) return -EIO;
    memcpy(&nsze, id, 8);
    flbas = id[26] & 0xF;
    lbads = id[128 + 4 * flbas + 2];
    if (nsze == 0 || lbads < 9 || lbads > 12) return -ENODEV;

    /* ৬) I/O queue: আগে completion, তারপর submission */
    if (alloc_queue(&c->io, 1) < 0) return -ENOMEM;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0 = 0x05;
    cmd.prp1 = c->io.cq_phys;
    cmd.cdw10 = ((QDEPTH - 1) << 16) | 1;
    cmd.cdw11 = 1;                               /* physically contiguous, ইন্টারাপ্ট বন্ধ */
    if (submit(c, &c->admin, &cmd, NULL) < 0) return -EIO;
    memset(&cmd, 0, sizeof(cmd));
    cmd.cdw0 = 0x01;
    cmd.prp1 = c->io.sq_phys;
    cmd.cdw10 = ((QDEPTH - 1) << 16) | 1;
    cmd.cdw11 = (1u << 16) | 1;                  /* CQ 1, contiguous */
    if (submit(c, &c->admin, &cmd, NULL) < 0) return -EIO;

    c->lock = (struct kmutex)KMUTEX_INIT("nvme");
    c->bdev.sector_size = 1u << lbads;
    c->bdev.sector_count = nsze;
    c->bdev.ops = &nvme_ops;
    c->bdev.driver_data = c;
    if (!c->bdev.model[0]) strcpy(c->bdev.model, "NVMe SSD");
    if (block_register(&c->bdev, "nvme") < 0) return -ENOMEM;
    return 0;
}

int nvme_init(void)
{
    size_t n;
    int disks = 0;
    memset(&info, 0, sizeof(info));
    for (n = 0; n < NVME_MAX_CTRL; n++) {
        struct pci_device *d = pci_find_class(0x01, 0x08, 0x02, n);
        if (!d) break;
        if (!d->bar[0] || d->bar_is_io[0]) continue;
        info.controllers++;
        if (setup(&ctrls[n], d) == 0) disks++;
    }
    return disks;
}

void nvme_get_info(struct nvme_info *out)
{
    *out = info;
}
