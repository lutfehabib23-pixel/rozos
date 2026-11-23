/* rozOS — devfs (rozos/devfs.h দেখুন)
 *
 * একটাই instance: root ডিরেক্টরি আর নিবন্ধিত ডিভাইসের সমতল তালিকা।
 * ডিভাইস vnode কখনো মুক্ত হয় না (nlink = 1 সবসময়)। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/devfs.h>
#include <rozos/vfs.h>
#include <rozos/errno.h>
#include <rozos/sched.h>
#include <rozos/random.h>
#include <rozos/string.h>
#include <rozos/console.h>
#include <rozos/serial.h>
#include <rozos/keyboard.h>
#include <rozos/timer.h>
#include <rozos/mouse.h>
#include <rozos/ttyctl.h>
#include <arch/x86_64/cpu.h>

struct devfs_device {
    struct vnode vn;
    char name[32];
    devfs_read_fn read;
    devfs_write_fn write;
    devfs_poll_fn poll;          /* ধাপ ১৩ */
    void *ctx;
};

static struct devfs_device devices[DEVFS_MAX_DEVICES];
static size_t device_count;
static struct vnode root;
static int root_ready;
static const struct vnode_ops dev_ops;
static const struct vnode_ops root_ops;

/* ---------- vnode অপারেশন ---------- */

static int64_t dev_read(struct vnode *vn, uint64_t off, void *buf, size_t len)
{
    struct devfs_device *d = (struct devfs_device *)vn;
    if (!d->name[0]) return -ENODEV;             /* ধাপ ১৫ক: সরানো ডিভাইস */
    return d->read ? d->read(d->ctx, off, buf, len) : -EINVAL;
}

static int64_t dev_write(struct vnode *vn, uint64_t off, const void *buf, size_t len)
{
    struct devfs_device *d = (struct devfs_device *)vn;
    if (!d->name[0]) return -ENODEV;
    return d->write ? d->write(d->ctx, off, buf, len) : -EINVAL;
}

/* ধাপ ১৩ */
static int dev_poll(struct vnode *vn, int events)
{
    struct devfs_device *d = (struct devfs_device *)vn;
    return d->poll ? d->poll(d->ctx, events) : (VFS_POLLIN | VFS_POLLOUT);
}

static int root_lookup(struct vnode *dir, const char *name, struct vnode **out)
{
    size_t i;
    (void)dir;
    for (i = 0; i < device_count; i++) {
        if (!strcmp(devices[i].name, name)) {
            vnode_ref(&devices[i].vn);
            *out = &devices[i].vn;
            return 0;
        }
    }
    return -ENOENT;
}

static int root_readdir(struct vnode *dir, uint64_t index, struct vfs_dirent *out)
{
    size_t i;
    (void)dir;
    for (i = 0; i < device_count; i++) {             /* ধাপ ১৫ক: সরানো (নামহীন) বাদ */
        if (!devices[i].name[0]) continue;
        if (index-- == 0) break;
    }
    if (i >= device_count) {
        return 0;
    }
    strncpy(out->name, devices[i].name, VFS_NAME_MAX);
    out->name[VFS_NAME_MAX] = '\0';
    out->type = VNODE_CHARDEV;
    out->inode = devices[i].vn.inode;
    out->size = 0;
    return 1;
}

static const struct vnode_ops dev_ops = {
    dev_read, dev_write, NULL, NULL, NULL, NULL, NULL, NULL, dev_poll, NULL /* rename (ধাপ ১৫ক) */, NULL /* setflags (ধাপ ১৯ছ) */
};

/* create/unlink নেই — devfs-এ ফাইল বানানো বা মোছা যায় না */
static const struct vnode_ops root_ops = {
    NULL, NULL, NULL, root_lookup, NULL, NULL, root_readdir, NULL, NULL, NULL /* rename (ধাপ ১৫ক) */, NULL /* setflags (ধাপ ১৯ছ) */
};

int devfs_register(const char *name, devfs_read_fn read, devfs_write_fn write, void *ctx)
{
    struct devfs_device *d = NULL;
    size_t i, idx = device_count;
    if (strlen(name) >= sizeof(devices[0].name)) {
        return -ENAMETOOLONG;
    }
    /* ধাপ ১৫ক: সরানো ডিভাইসের খালি ঘর (কেউ খুলে না রাখলে) আবার ব্যবহার */
    for (i = 0; i < device_count; i++) {
        if (!devices[i].name[0] && devices[i].vn.refcount == 0) { idx = i; break; }
    }
    if (idx == device_count && device_count >= DEVFS_MAX_DEVICES) {
        return -ENOMEM;
    }
    d = &devices[idx];
    memset(d, 0, sizeof(*d));
    strcpy(d->name, name);
    d->read = read;
    d->write = write;
    d->ctx = ctx;
    d->vn.type = VNODE_CHARDEV;
    d->vn.mode = 0666;
    d->vn.inode = 1000 + idx;
    d->vn.nlink = 1;
    d->vn.ops = &dev_ops;
    d->vn.fs_data = d;
    d->vn.mount = root.mount;
    if (idx == device_count) device_count++;
    root.size = device_count;
    return 0;
}

int devfs_unregister(const char *name)
{
    size_t i;
    if (!name || !name[0]) return -EINVAL;
    for (i = 0; i < device_count; i++) {
        if (!strcmp(devices[i].name, name)) {
            devices[i].name[0] = '\0';
            devices[i].read = NULL;
            devices[i].write = NULL;
            devices[i].poll = NULL;
            return 0;
        }
    }
    return -ENOENT;
}

int devfs_set_poll(const char *name, devfs_poll_fn poll)
{
    size_t i;
    for (i = 0; i < device_count; i++) {
        if (!strcmp(devices[i].name, name)) {
            devices[i].poll = poll;
            return 0;
        }
    }
    return -ENOENT;
}

const char *devfs_vnode_name(struct vnode *vn)
{
    size_t i;
    for (i = 0; i < device_count; i++) {
        if (&devices[i].vn == vn) return devices[i].name;
    }
    return NULL;
}

static int devfs_mount(const void *arg, struct vnode **out)
{
    (void)arg;
    if (!root_ready) {
        return -ENODEV;
    }
    *out = &root;
    return 0;
}

static int devfs_statfs(struct vnode *r, struct vfs_statfs *out)
{
    (void)r;
    out->total_inodes = DEVFS_MAX_DEVICES;
    out->free_inodes = DEVFS_MAX_DEVICES - device_count;
    out->read_only = 1;
    out->source[0] = 'd'; out->source[1] = 'e'; out->source[2] = 'v'; out->source[3] = '\0';
    return 0;
}

const struct filesystem devfs_filesystem = { "devfs", devfs_mount, devfs_statfs, NULL, NULL };

/* ---------- ডিফল্ট ডিভাইস ---------- */

static int64_t null_read(void *ctx, uint64_t off, void *buf, size_t len)
{
    (void)ctx; (void)off; (void)buf; (void)len;
    return 0;
}

static int64_t sink_write(void *ctx, uint64_t off, const void *buf, size_t len)
{
    (void)ctx; (void)off; (void)buf;
    return (int64_t)len;
}

static int64_t zero_read(void *ctx, uint64_t off, void *buf, size_t len)
{
    (void)ctx; (void)off;
    memset(buf, 0, len);
    return (int64_t)len;
}

/* ধাপ ১২: মাপ খোঁজার সময় serial-এ আসা অন্য বাইট (ব্যবহারকারীর কী) এখানে জমে থাকে */
static char serial_back[64];
static size_t serial_back_len, serial_back_pos;

/* ---------- ধাপ ১৪: কনসোলের Ctrl+C → foreground গ্রুপে SIGINT ---------- */
static struct ttyctl console_tc;
static struct ttyctl serial_tc;               /* /dev/serial0-এর shell (ডেস্কটপ মোডে) */
static volatile int serial_querying;          /* console_query_size নিজে serial পড়ছে */

struct ttyctl *console_ttyctl(void) { return &console_tc; }
struct ttyctl *serial_ttyctl(void) { return &serial_tc; }

static int kbd_intr(void)
{
    return ttyctl_input(&console_tc, 3);
}

/* প্রতি টিকে: serial-এ আসা বাইট serial_back-এ তুলে রাখা (হারায় না), Ctrl+C দেখা —
 * যাতে CPU-খেকো প্রোগ্রামকেও (যে stdin পড়ে না) serial থেকে থামানো যায় */
static void console_tick(uint64_t t)
{
    (void)t;
    if (serial_querying || !serial_is_ready()) return;
    while (serial_back_len < sizeof(serial_back)) {
        char c;
        if (!serial_try_getc(&c)) break;
        /* serial0-তে কোনো shell থাকলে (ডেস্কটপ মোড) সেটাই serial-এর টার্মিনাল */
        if (c == 3 && ttyctl_input(serial_tc.fg_pgid > 0 ? &serial_tc : &console_tc, 3)) continue;
        if (serial_back_pos == serial_back_len) serial_back_pos = serial_back_len = 0;
        serial_back[serial_back_len++] = c;
    }
}

static int serial_next(char *c)
{
    if (serial_back_pos < serial_back_len) {
        *c = serial_back[serial_back_pos++];
        if (serial_back_pos == serial_back_len) serial_back_pos = serial_back_len = 0;
        return 1;
    }
    return serial_try_getc(c);
}

/* কিবোর্ড বা serial থেকে একটা কী — না থাকলে 0 */
static int try_any_key(int *key)
{
    char c;
    if (keyboard_try_getkey(key)) {
        return 1;
    }
    if (serial_next(&c)) {
        *key = (c == '\r') ? '\n' : (c == 0x7F) ? '\b' : (unsigned char)c;
        return 1;
    }
    return 0;
}

/* ধাপ ৭: বিশেষ কী → ANSI/VT100 escape sequence (serial টার্মিনাল যেমন পাঠায়), যাতে
 * shell ও এডিটর কিবোর্ড আর serial দুটো থেকেই একইভাবে তীর-কী ইত্যাদি পায় */
static const char *key_sequence(int key)
{
    switch (key) {
    case KEY_UP:        return "\x1b[A";
    case KEY_DOWN:      return "\x1b[B";
    case KEY_RIGHT:     return "\x1b[C";
    case KEY_LEFT:      return "\x1b[D";
    case KEY_HOME:      return "\x1b[H";
    case KEY_END:       return "\x1b[F";
    case KEY_INSERT:    return "\x1b[2~";
    case KEY_DELETE:    return "\x1b[3~";
    case KEY_PAGE_UP:   return "\x1b[5~";
    case KEY_PAGE_DOWN: return "\x1b[6~";
    /* ধাপ ১৪খ: F-কী — xterm-এর মতো (এডিটরের F10 মেনু, F1 সাহায্য) */
    case KEY_F1:        return "\x1bOP";
    case KEY_F2:        return "\x1bOQ";
    case KEY_F3:        return "\x1bOR";
    case KEY_F4:        return "\x1bOS";
    case KEY_F5:        return "\x1b[15~";
    case KEY_F6:        return "\x1b[17~";
    case KEY_F7:        return "\x1b[18~";
    case KEY_F8:        return "\x1b[19~";
    case KEY_F9:        return "\x1b[20~";
    case KEY_F10:       return "\x1b[21~";
    case KEY_F11:       return "\x1b[23~";
    case KEY_F12:       return "\x1b[24~";
    default:            return NULL;
    }
}

/* ধাপ ১২: Shift/Ctrl/Alt সহ বিশেষ কী — xterm-এর মতো ESC[1;<m>A বা ESC[3;<m>~
 * (m = 1 + Shift 1 + Alt 2 + Ctrl 4); modifier না থাকলে আগের সাধারণ রূপ */
static int key_sequence_mod(int key, char *out, size_t size)
{
    const char *base = key_sequence(key);
    uint8_t mods = keyboard_modifiers();
    int m = 1 + ((mods & KEYMOD_SHIFT) ? 1 : 0) + ((mods & KEYMOD_ALT) ? 2 : 0) + ((mods & KEYMOD_CTRL) ? 4 : 0);
    size_t n;
    if (!base) return 0;
    n = strlen(base);
    if (m == 1 || n < 3 || n + 4 >= size) {
        memcpy(out, base, n);
        return (int)n;
    }
    if (base[n - 1] == '~') {                    /* ESC[3~ → ESC[3;5~ */
        memcpy(out, base, n - 1);
        out[n - 1] = ';';
        out[n] = (char)('0' + m);
        out[n + 1] = '~';
        return (int)n + 2;
    }
    /* ESC[A → ESC[1;5A */
    out[0] = 0x1B; out[1] = '['; out[2] = '1'; out[3] = ';'; out[4] = (char)('0' + m);
    out[5] = base[n - 1];
    return 6;
}

/* আগের read-এ জায়গা না পাওয়া sequence-এর বাকি অংশ (ধাপ ১২: মাউসের জন্য বড়) */
static char pending_seq[128];
static size_t pending_len, pending_pos;

static void pending_add(const char *s, size_t n)
{
    if (pending_pos == pending_len) pending_pos = pending_len = 0;
    if (pending_len + n > sizeof(pending_seq)) return;          /* ভরা — বাদ */
    memcpy(pending_seq + pending_len, s, n);
    pending_len += n;
}

/* ---------- ধাপ ১২: PS/2 মাউস → মাউস-কার্সর আর xterm SGR ঘটনা ---------- */

static uint8_t mouse_last_buttons;

static size_t put_num(char *p, uint32_t v)
{
    char tmp[12];
    size_t n = 0, i;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (i = 0; i < n; i++) p[i] = tmp[n - 1 - i];
    return n;
}

static void sgr_event(uint32_t b, uint32_t col, uint32_t row, int release)
{
    char buf[32];
    size_t n = 0;
    buf[n++] = 0x1B; buf[n++] = '['; buf[n++] = '<';
    n += put_num(buf + n, b);
    buf[n++] = ';';
    n += put_num(buf + n, col + 1);
    buf[n++] = ';';
    n += put_num(buf + n, row + 1);
    buf[n++] = release ? 'm' : 'M';
    pending_add(buf, n);
}

static void process_mouse(void)
{
    struct mouse_event e;
    uint64_t w = fb_is_ready() ? fb_width() : 0, h = fb_is_ready() ? fb_height() : 0;
    while (mouse_poll_event(0, &e)) {
        uint32_t col, row;
        int mode = tty_mouse_mode();
        uint8_t changed = (uint8_t)(e.buttons ^ mouse_last_buttons);
        static const uint8_t bit[3] = { MOUSE_BUTTON_LEFT, MOUSE_BUTTON_MIDDLE, MOUSE_BUTTON_RIGHT };
        int i;
        if (!w || !h || !tty_cols() || !tty_rows()) continue;
        col = (uint32_t)((uint64_t)e.x * tty_cols() / w);
        row = (uint32_t)((uint64_t)e.y * tty_rows() / h);
        tty_mouse_pointer(1, col, row);
        if (mode & (TTY_MOUSE_CLICKS | TTY_MOUSE_DRAG | TTY_MOUSE_MOTION)) {
            for (i = 0; i < 3; i++) {                   /* xterm: 0 বাম, 1 মাঝ, 2 ডান */
                if (changed & bit[i]) sgr_event((uint32_t)i, col, row, !(e.buttons & bit[i]));
            }
            if (!changed && (e.dx || e.dy)) {
                if (e.buttons && (mode & (TTY_MOUSE_DRAG | TTY_MOUSE_MOTION))) {
                    uint32_t b = (e.buttons & MOUSE_BUTTON_LEFT) ? 0 : (e.buttons & MOUSE_BUTTON_MIDDLE) ? 1 : 2;
                    sgr_event(32 + b, col, row, 0);      /* টেনে নেওয়া */
                } else if (!e.buttons && (mode & TTY_MOUSE_MOTION)) {
                    sgr_event(35, col, row, 0);
                }
            }
            if (e.wheel) sgr_event(e.wheel < 0 ? 64 : 65, col, row, 0);
        }
        mouse_last_buttons = e.buttons;
    }
}

/* ---------- ধাপ ১২: serial টার্মিনালের মাপ জিজ্ঞেস করা ----------
 * কার্সর অনেক দূরে (ডানে-নিচে) পাঠিয়ে "কার্সর কোথায়?" (ESC[6n) — টার্মিনাল উত্তর দেয়
 * ESC[<row>;<col>R, যা তার মাপ। শুধু serial-এ লেখা হয় (স্ক্রিনের গ্রিডে নয়)। উত্তরের
 * আগে আসা অন্য বাইট serial_back-এ জমে থাকে, হারায় না। ফেরত 0 = পাওয়া গেছে। */
int console_query_size(uint32_t *rows, uint32_t *cols)
{
    static const char q[] = "\0337\033[999;999H\033[6n\0338";
    char buf[32];
    size_t n = 0, i;
    uint64_t end;
    if (!serial_is_ready()) {
        return -1;
    }
    serial_querying = 1;                          /* ধাপ ১৪: টিক এখন serial পড়বে না */
    serial_write(q, sizeof(q) - 1);
    end = timer_uptime_ms() + 300;
    while (timer_uptime_ms() < end) {
        char c;
        if (!serial_try_getc(&c)) {
            sched_relax();
            continue;
        }
        if (n == 0 && c != 0x1B) {                   /* উত্তরের আগের ব্যবহারকারীর কী */
            if (serial_back_len < sizeof(serial_back)) serial_back[serial_back_len++] = c;
            continue;
        }
        if (n < sizeof(buf) - 1) buf[n++] = c;
        if (c == 'R') {
            uint32_t r = 0, cc = 0, *p = &r;
            if (n < 6 || buf[1] != '[') break;
            for (i = 2; i < n - 1; i++) {
                if (buf[i] == ';') { p = &cc; continue; }
                if (buf[i] < '0' || buf[i] > '9') { r = 0; break; }
                *p = *p * 10 + (uint32_t)(buf[i] - '0');
            }
            if (r >= 5 && cc >= 20 && r < 1000 && cc < 1000) {
                *rows = r;
                *cols = cc;
                serial_querying = 0;
                return 0;
            }
            break;
        }
        if (n > 1 && buf[1] != '[') {                /* ESC কিন্তু উত্তর নয় — ফেরত রাখা */
            for (i = 0; i < n && serial_back_len < sizeof(serial_back); i++) serial_back[serial_back_len++] = buf[i];
            n = 0;
        }
    }
    for (i = 0; i < n && serial_back_len < sizeof(serial_back); i++) serial_back[serial_back_len++] = buf[i];
    serial_querying = 0;
    return -1;
}

/* অন্তত একটা অক্ষর না আসা পর্যন্ত অপেক্ষা, তারপর যা জমে আছে তা */
static int64_t console_read(void *ctx, uint64_t off, void *buf, size_t len)
{
    char *out = (char *)buf;
    size_t n = 0;
    int key;
    (void)ctx; (void)off;

    if (len == 0) {
        return 0;
    }
    while (n == 0) {
        cpu_cli();
        process_mouse();                          /* ধাপ ১২ */
        while (n < len && pending_pos < pending_len) {
            out[n++] = pending_seq[pending_pos++];
        }
        while (n < len && try_any_key(&key)) {
            if (key < 0x100) {
                out[n++] = (char)key;
            } else {
                char seq[16];
                int sl = key_sequence_mod(key, seq, sizeof(seq));
                if (sl > 0) {
                    pending_add(seq, (size_t)sl);
                    while (n < len && pending_pos < pending_len) {
                        out[n++] = pending_seq[pending_pos++];
                    }
                }
            }
        }
        if (n == 0) {
            if (sched_should_abort()) {       /* ধাপ ৬: প্রসেস kill হয়েছে */
                cpu_sti();
                return -EINTR;
            }
            sched_relax();
        } else {
            cpu_sti();
        }
    }
    return (int64_t)n;
}

/* ধাপ ১৩: poll — কী/মাউস-ঘটনা থাকলে সেটা pending-এ তুলে রাখা (হারায় না; পরের
 * read সেখান থেকেই দেয়), তারপর pending খালি কিনা দেখা */
static int console_poll(void *ctx, int events)
{
    int key, r = VFS_POLLOUT;
    uint64_t flags;
    (void)ctx; (void)events;
    flags = cpu_irq_save();
    process_mouse();
    if (pending_pos >= pending_len && try_any_key(&key)) {
        if (key < 0x100) {
            char ch = (char)key;
            pending_add(&ch, 1);
        } else {
            char seq[16];
            int sl = key_sequence_mod(key, seq, sizeof(seq));
            if (sl > 0) pending_add(seq, (size_t)sl);
        }
    }
    if (pending_pos < pending_len) r |= VFS_POLLIN;
    cpu_irq_restore(flags);
    return r;
}

static int kbd_poll(void *ctx, int events)
{
    (void)ctx; (void)events;
    return keyboard_has_key() ? VFS_POLLIN : 0;
}

static int mouse_dev_poll(void *ctx, int events)
{
    (void)ctx; (void)events;
    return mouse_event_pending(1) ? VFS_POLLIN : 0;
}

static int serial_dev_poll(void *ctx, int events)
{
    (void)ctx; (void)events;
    return VFS_POLLOUT | ((serial_back_pos < serial_back_len || serial_rx_ready()) ? VFS_POLLIN : 0);
}

static int64_t console_write(void *ctx, uint64_t off, const void *buf, size_t len)
{
    (void)ctx; (void)off;
    tty_write((const char *)buf, len);
    return (int64_t)len;
}

static int64_t serial_dev_read(void *ctx, uint64_t off, void *buf, size_t len)
{
    char *out = (char *)buf;
    size_t n = 0;
    char c;
    (void)ctx; (void)off;

    if (len == 0 || !serial_is_ready()) {
        return 0;
    }
    while (n == 0) {
        uint64_t fl = cpu_irq_save();             /* টিকের সাথে serial_back ভাগ */
        while (n < len && serial_next(&c)) {      /* ধাপ ১৪: টিকে জমা বাইটও */
            out[n++] = c;
        }
        cpu_irq_restore(fl);
        if (n == 0) {
            if (sched_should_abort()) {
                return -EINTR;
            }
            timer_sleep_ms(1);           /* serial-এ এখনো ইন্টারাপ্ট নেই */
        }
    }
    return (int64_t)n;
}

static int64_t serial_dev_write(void *ctx, uint64_t off, const void *buf, size_t len)
{
    (void)ctx; (void)off;
    serial_write((const char *)buf, len);
    return (int64_t)len;
}

/* ধাপ ১০: /dev/random, /dev/urandom — দুটোই কখনো আটকায় না (ChaCha20 CSPRNG);
 * লিখলে সেই বাইটগুলো entropy হিসেবে মেশে */
static int64_t urandom_read(void *ctx, uint64_t off, void *buf, size_t len)
{
    (void)ctx; (void)off;
    random_get(buf, len);
    return (int64_t)len;
}

static int64_t urandom_write(void *ctx, uint64_t off, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t i;
    (void)ctx; (void)off;
    for (i = 0; i + 8 <= len; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        random_add_entropy(v);
    }
    return (int64_t)len;
}

/* /dev/kbd: প্রতিটি কী int32 হিসেবে (ASCII বা KEY_*); len অন্তত 4 হতে হবে */
/* ধাপ ১২: /dev/mouse — struct mouse_event (rozos/mouse.h, ২০ বাইট) এর তালিকা;
 * অন্তত একটা ঘটনা না আসা পর্যন্ত অপেক্ষা (গ্রাফিক্স প্রোগ্রামের জন্য, ধাপ ১৩) */
static int64_t mouse_dev_read(void *ctx, uint64_t off, void *buf, size_t len)
{
    struct mouse_event *out = (struct mouse_event *)buf;
    size_t n = 0, max = len / sizeof(struct mouse_event);
    (void)ctx; (void)off;
    if (max == 0) {
        return -EINVAL;
    }
    while (n == 0) {
        while (n < max && mouse_poll_event(1, &out[n])) {
            n++;
        }
        if (n == 0) {
            if (sched_should_abort()) {
                return -EINTR;
            }
            sched_relax();
        }
    }
    return (int64_t)(n * sizeof(struct mouse_event));
}

static int64_t kbd_read(void *ctx, uint64_t off, void *buf, size_t len)
{
    int32_t *out = (int32_t *)buf;
    size_t n = 0, max = len / sizeof(int32_t);
    int key;
    (void)ctx; (void)off;

    if (max == 0) {
        return -EINVAL;
    }
    while (n == 0) {
        cpu_cli();
        while (n < max && keyboard_try_getkey(&key)) {
            /* ধাপ ১৪: বিশেষ কী-র সাথে modifier: বিট 16-18 = Shift/Ctrl/Alt (KEYMOD_*) */
            if (key >= 0x100) key |= (int)(keyboard_modifiers() & 7) << 16;
            out[n++] = key;
        }
        if (n == 0) {
            if (sched_should_abort()) {
                cpu_sti();
                return -EINTR;
            }
            sched_relax();
        } else {
            cpu_sti();
        }
    }
    return (int64_t)(n * sizeof(int32_t));
}

/* ধাপ ১৮ক: /dev/keyev — কাঁচা কী-ঘটনা int32 (rozos/keyboard.h-এর KEYEV_*): চাপা ও ছাড়া
 * দুটোই। সাধারণ কী-বাফার (console, /dev/kbd) এতে বদলায় না। */
static int64_t keyev_read(void *ctx, uint64_t off, void *buf, size_t len)
{
    int32_t *out = (int32_t *)buf;
    size_t n = 0, max = len / sizeof(int32_t);
    int ev;
    (void)ctx; (void)off;

    if (max == 0) {
        return -EINVAL;
    }
    while (n == 0) {
        cpu_cli();
        while (n < max && keyboard_try_getraw(&ev)) {
            out[n++] = ev;
        }
        cpu_sti();
        if (n == 0) {
            if (sched_should_abort()) {
                return -EINTR;
            }
            sched_relax();
        }
    }
    return (int64_t)(n * sizeof(int32_t));
}

static int keyev_poll(void *ctx, int events)
{
    (void)ctx; (void)events;
    return keyboard_has_raw() ? VFS_POLLIN : 0;
}

int devfs_init(void)
{
    /* ধাপ ৫: একবারই চলে, আর আগে নিবন্ধিত ডিভাইস (যদি থাকে) মোছে না */
    if (root_ready) {
        return 0;
    }
    memset(&root, 0, sizeof(root));
    root.type = VNODE_DIR;
    root.mode = 0755;
    root.inode = 1;
    root.nlink = 1;
    root.ops = &root_ops;
    root.size = device_count;
    root_ready = 1;

    devfs_register("null", null_read, sink_write, NULL);
    devfs_register("zero", zero_read, sink_write, NULL);
    devfs_register("console", console_read, console_write, NULL);
    devfs_register("serial0", serial_dev_read, serial_dev_write, NULL);
    devfs_register("kbd", kbd_read, NULL, NULL);
    devfs_register("mouse", mouse_dev_read, NULL, NULL);    /* ধাপ ১২ */
    devfs_register("keyev", keyev_read, NULL, NULL);        /* ধাপ ১৮ক */
    devfs_register("random", urandom_read, urandom_write, NULL);
    devfs_register("urandom", urandom_read, urandom_write, NULL);
    /* ধাপ ১৩: poll() */
    devfs_set_poll("console", console_poll);
    devfs_set_poll("kbd", kbd_poll);
    devfs_set_poll("mouse", mouse_dev_poll);
    devfs_set_poll("keyev", keyev_poll);
    devfs_set_poll("serial0", serial_dev_poll);
    /* ধাপ ১৪: Ctrl+C */
    ttyctl_init(&console_tc);
    ttyctl_init(&serial_tc);
    keyboard_set_intr_hook(kbd_intr);
    timer_add_tick_hook(console_tick);
    return vfs_register_filesystem(&devfs_filesystem);
}
