/* rozOS — shared memory (ধাপ ১৩, rozos/shm.h দেখুন)
 *
 * একটা shm বস্তু = নাম (বা নামহীন), মাপ, আর তার ফিজিক্যাল পেজগুলোর তালিকা। দুটো প্রসেস
 * একই বস্তু mmap(MAP_SHARED) করলে দুজনের ঠিকানা-স্থানে একই ফ্রেম বসে (VMM_SHARED) —
 * একজন লিখলে অন্যজন সাথে সাথে দেখে। ধাপ ১৪-এর window server-এর উইন্ডো-বাফার এর ওপর।
 *
 * কে কতদিন বাঁচে (POSIX-এর মতো):
 *   - নাম থাকে shm_unlink পর্যন্ত (প্রসেস শেষ হলেও বস্তু থাকে — অন্যরা পরে খুলতে পারে)
 *   - পেজ মুক্ত হয় শুধু যখন: নাম মোছা হয়েছে, কোনো fd খোলা নেই, কোথাও ম্যাপ নেই
 *   - ম্যাপের হিসাব পেজ-ধরে: vmm-এর shared hook প্রতিটা ম্যাপ/আনম্যাপে ±1 দেয়
 * MAP_SHARED|MAP_ANONYMOUS: নামহীন বস্তু (fork-এর পর মা-সন্তান ভাগ করে)।
 * fd দিয়ে read/write/lseek/fstat/ftruncate-ও চলে (সাধারণ ফাইলের মতো)। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/shm.h>
#include <rozos/vfs.h>
#include <rozos/vmm.h>
#include <rozos/pmm.h>
#include <rozos/heap.h>
#include <rozos/errno.h>
#include <rozos/string.h>
#include <rozos/printf.h>
#include <arch/x86_64/cpu.h>

struct shm_obj {
    struct vnode vn;                 /* fd-এর জন্য (nlink = 0 — refcount 0 হলে release) */
    char name[SHM_NAME_MAX + 1];
    int named;                       /* নামের তালিকায় আছে (unlink হয়নি) */
    int vn_open;                     /* vnode-এর ref আছে (কোনো fd খোলা) */
    uint64_t *pages;                 /* ফিজিক্যাল ঠিকানা, npages টা */
    size_t npages, cap;
    uint64_t maps;                   /* মোট কয়টা পেজ-ম্যাপিং আছে (সব প্রসেস মিলিয়ে) */
    uint64_t min_phys, max_phys;     /* দ্রুত খোঁজার জন্য */
    struct shm_obj *next;
};

static struct shm_obj *objects;
static uint64_t next_ino = 1;
static const struct vnode_ops shm_ops;

static void update_range(struct shm_obj *o)
{
    size_t i;
    o->min_phys = ~0ULL;
    o->max_phys = 0;
    for (i = 0; i < o->npages; i++) {
        if (o->pages[i] < o->min_phys) o->min_phys = o->pages[i];
        if (o->pages[i] > o->max_phys) o->max_phys = o->pages[i];
    }
}

static void obj_free(struct shm_obj *o)
{
    struct shm_obj **pp;
    size_t i;
    for (pp = &objects; *pp; pp = &(*pp)->next) {
        if (*pp == o) {
            *pp = o->next;
            break;
        }
    }
    for (i = 0; i < o->npages; i++) pmm_free_page(o->pages[i]);
    kfree(o->pages);
    kfree(o);
}

static void maybe_free(struct shm_obj *o)
{
    if (!o->named && !o->vn_open && o->maps == 0) obj_free(o);
}

/* মাপ বদলানো: বাড়ালে নতুন শূন্য পেজ, কমালে শেষের পেজ মুক্ত (ম্যাপ থাকলে কমানো যায় না) */
static int obj_resize(struct shm_obj *o, uint64_t size)
{
    size_t want = (size_t)((size + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE);
    if (size > SHM_MAX_SIZE) return -EFBIG;
    if (want < o->npages) {
        if (o->maps) return -EBUSY;
        while (o->npages > want) pmm_free_page(o->pages[--o->npages]);
    }
    if (want > o->cap) {
        uint64_t *np = (uint64_t *)kmalloc(want * sizeof(uint64_t));
        if (!np) return -ENOMEM;
        if (o->npages) memcpy(np, o->pages, o->npages * sizeof(uint64_t));
        kfree(o->pages);
        o->pages = np;
        o->cap = want;
    }
    while (o->npages < want) {
        uint64_t ph = pmm_alloc_zeroed_page();
        if (!ph) {
            update_range(o);
            o->vn.size = (uint64_t)o->npages * VMM_PAGE_SIZE;
            return -ENOMEM;
        }
        o->pages[o->npages++] = ph;
    }
    o->vn.size = size;
    update_range(o);
    return 0;
}

static struct shm_obj *obj_new(const char *name)
{
    struct shm_obj *o = (struct shm_obj *)kzalloc(sizeof(*o));
    if (!o) return NULL;
    if (name) {
        strncpy(o->name, name, SHM_NAME_MAX);
        o->named = 1;
    }
    o->vn.type = VNODE_FILE;
    o->vn.mode = 0600;
    o->vn.inode = 0x60000000ULL + next_ino++;
    o->vn.refcount = 1;
    o->vn.nlink = 0;
    o->vn.ops = &shm_ops;
    o->vn.fs_data = o;
    o->vn_open = 1;
    o->min_phys = ~0ULL;
    o->next = objects;
    objects = o;
    return o;
}

static struct shm_obj *find_name(const char *name)
{
    struct shm_obj *o;
    for (o = objects; o; o = o->next) {
        if (o->named && !strcmp(o->name, name)) return o;
    }
    return NULL;
}

/* ---------- vnode ops (fd দিয়ে পড়া/লেখা) ---------- */

static int64_t shm_rw(struct vnode *vn, uint64_t off, void *rbuf, const void *wbuf, size_t len)
{
    struct shm_obj *o = (struct shm_obj *)vn->fs_data;
    size_t done = 0;
    if (off >= o->vn.size) return 0;
    if (len > o->vn.size - off) len = (size_t)(o->vn.size - off);
    while (done < len) {
        uint64_t pos = off + done;
        size_t pi = (size_t)(pos / VMM_PAGE_SIZE), po = (size_t)(pos % VMM_PAGE_SIZE);
        size_t chunk = VMM_PAGE_SIZE - po;
        uint8_t *page;
        if (chunk > len - done) chunk = len - done;
        if (pi >= o->npages) break;
        page = (uint8_t *)pmm_phys_to_virt(o->pages[pi]);
        if (rbuf) memcpy((uint8_t *)rbuf + done, page + po, chunk);
        else memcpy(page + po, (const uint8_t *)wbuf + done, chunk);
        done += chunk;
    }
    return (int64_t)done;
}

static int64_t shm_read(struct vnode *vn, uint64_t off, void *buf, size_t len)
{
    return shm_rw(vn, off, buf, NULL, len);
}

/* লেখা মাপ বাড়ায় না (Linux-এর মতো ftruncate দিয়ে মাপ ঠিক করতে হয়) */
static int64_t shm_write(struct vnode *vn, uint64_t off, const void *buf, size_t len)
{
    return shm_rw(vn, off, NULL, buf, len);
}

static int shm_truncate(struct vnode *vn, uint64_t size)
{
    uint64_t fl = cpu_irq_save();
    int r = obj_resize((struct shm_obj *)vn->fs_data, size);
    cpu_irq_restore(fl);
    return r;
}

static void shm_release(struct vnode *vn)
{
    struct shm_obj *o = (struct shm_obj *)vn->fs_data;
    uint64_t fl = cpu_irq_save();
    o->vn_open = 0;
    maybe_free(o);
    cpu_irq_restore(fl);
}

static const struct vnode_ops shm_ops = {
    shm_read, shm_write, shm_truncate, NULL, NULL, NULL, NULL, shm_release, NULL, NULL /* rename (ধাপ ১৫ক) */
};

/* ---------- vmm hook: ম্যাপ/আনম্যাপের হিসাব ---------- */

static void shm_hook(uint64_t phys, int delta)
{
    struct shm_obj *o;
    size_t i;
    uint64_t fl = cpu_irq_save();          /* এক CPU: তালিকা বদলানোর মাঝে preemption নয় */
    for (o = objects; o; o = o->next) {
        if (phys < o->min_phys || phys > o->max_phys) continue;
        for (i = 0; i < o->npages; i++) {
            if (o->pages[i] == phys) {
                if (delta > 0) o->maps++;
                else if (o->maps) o->maps--;
                if (!o->maps) maybe_free(o);
                cpu_irq_restore(fl);
                return;
            }
        }
    }
    /* কোনো বস্তুর নয় (যেমন /dev/fb0-এর ফ্রেমবাফার) — কিছু করার নেই */
    cpu_irq_restore(fl);
}

void shm_init(void)
{
    vmm_set_shared_hook(shm_hook);
}

/* ---------- syscall-এর জন্য ---------- */

static int name_ok(const char *name)
{
    size_t n = strlen(name);
    if (name[0] == '/') { name++; n--; }
    if (n == 0 || n > SHM_NAME_MAX) return 0;
    for (; *name; name++) if (*name == '/') return 0;
    return 1;
}

static int do_shm_open(const char *name, int flags);

int shm_open_fd(const char *name, int flags)
{
    uint64_t fl = cpu_irq_save();
    int r = do_shm_open(name, flags);
    cpu_irq_restore(fl);
    return r;
}

static int do_shm_open(const char *name, int flags)
{
    struct shm_obj *o;
    int fd, acc = flags & O_ACCMODE;
    if (!name_ok(name)) return -EINVAL;
    if (name[0] == '/') name++;
    o = find_name(name);
    if (o && (flags & O_CREAT) && (flags & O_EXCL)) return -EEXIST;
    if (!o) {
        if (!(flags & O_CREAT)) return -ENOENT;
        o = obj_new(name);
        if (!o) return -ENOMEM;
    } else {
        if (!o->vn_open) {                 /* আগের সব fd বন্ধ ছিল — নতুন ref */
            o->vn.refcount = 0;
            o->vn_open = 1;
        }
        vnode_ref(&o->vn);
    }
    if ((flags & O_TRUNC) && acc != O_RDONLY) {
        int r = obj_resize(o, 0);
        if (r < 0) {
            vnode_unref(&o->vn);
            return r;
        }
    }
    fd = vfs_install_vnode(&o->vn, acc | (flags & O_APPEND));
    if (fd < 0) vnode_unref(&o->vn);
    return fd;
}

int shm_unlink_name(const char *name)
{
    struct shm_obj *o;
    uint64_t fl;
    if (!name_ok(name)) return -EINVAL;
    if (name[0] == '/') name++;
    fl = cpu_irq_save();
    o = find_name(name);
    if (!o) {
        cpu_irq_restore(fl);
        return -ENOENT;
    }
    o->named = 0;
    maybe_free(o);
    cpu_irq_restore(fl);
    return 0;
}

struct shm_obj *shm_from_vnode(struct vnode *vn)
{
    return (vn && vn->ops == &shm_ops) ? (struct shm_obj *)vn->fs_data : NULL;
}

struct shm_obj *shm_anon_create(uint64_t size)
{
    uint64_t fl = cpu_irq_save();
    struct shm_obj *o = obj_new(NULL);
    if (o && obj_resize(o, size) < 0) {
        o->vn_open = 0;
        maybe_free(o);
        o = NULL;
    }
    cpu_irq_restore(fl);
    return o;
}

/* নামহীন বস্তুর জন্য: mmap হয়ে গেলে vnode আর লাগে না */
void shm_anon_done(struct shm_obj *o)
{
    uint64_t fl = cpu_irq_save();
    o->vn_open = 0;
    maybe_free(o);
    cpu_irq_restore(fl);
}

int shm_map(struct shm_obj *o, struct vmm_space *sp, uint64_t va, uint64_t off,
            uint64_t len, uint32_t flags)
{
    uint64_t i, n = (len + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE, first = off / VMM_PAGE_SIZE;
    uint64_t fl;
    if (first + n > o->npages) return -ENXIO;
    fl = cpu_irq_save();
    o->maps++;                               /* ম্যাপের মাঝে মুক্ত না হয় */
    for (i = 0; i < n; i++) {
        if (!vmm_map_page(sp, va + i * VMM_PAGE_SIZE, o->pages[first + i], flags | VMM_SHARED)) {
            uint64_t j;
            for (j = 0; j < i; j++) vmm_unmap_page(sp, va + j * VMM_PAGE_SIZE);   /* hook −1 */
            o->maps--;
            maybe_free(o);
            cpu_irq_restore(fl);
            return -ENOMEM;
        }
        o->maps++;
    }
    o->maps--;
    cpu_irq_restore(fl);
    return 0;
}

void shm_dump(void)
{
    struct shm_obj *o;
    int n = 0;
    for (o = objects; o; o = o->next) {
        kprintf("  %-24s %8llu bytes  %4llu pages mapped  %s%s\n",
                o->named ? o->name : "(anonymous)", (unsigned long long)o->vn.size,
                (unsigned long long)o->maps, o->vn_open ? "open" : "closed",
                o->named ? "" : ", unlinked");
        n++;
    }
    if (!n) kprintf("  (no shared memory)\n");
}
