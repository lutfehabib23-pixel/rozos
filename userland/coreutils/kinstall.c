/* rozOS — kinstall: নতুন kernel boot partition-এ বসানো আর boot মেনু লেখা (ধাপ ১১খ)
 *
 *   kinstall [FILE]          FILE (ডিফল্ট /usr/src/rozos/build/rozos.elf) → /boot/kernels/
 *                            rozos-<build>.elf, ডিফল্ট করা, মেনু নতুন করে লেখা
 *   kinstall -l              ইনস্টল করা kernel-গুলো
 *   kinstall -d ID           ID (যেমন 42.3, বা "factory") ডিফল্ট
 *   kinstall -r ID           একটা মোছা (ডিফল্টটা নয়)
 *   kinstall -m              শুধু /boot/limine.conf আবার লেখা
 *   kinstall --resolution WxH   ধাপ ১৩: পর্দার রেজোলিউশন (যেমন 1024x768, 1920x1080);
 *                            "auto" = Limine নিজে বাছবে। পরের reboot থেকে। সব এন্ট্রিতে
 *                            "resolution:" লাইন (/boot/kernels/resolution-এ মনে রাখা)
 *
 * boot partition (FAT) rozOS-এ /boot:
 *   /boot/limine.conf          Limine-এর মেনু — kinstall লেখে
 *   /boot/rozos-factory.elf    PC-তে gcc দিয়ে বানানো, কখনো মোছা হয় না
 *   /boot/initrd.tar           rescue initrd (সব kernel একই)
 *   /boot/kernels/list         ইনস্টল করা build, নতুনটা আগে (প্রতি লাইনে একটা)
 *   /boot/kernels/default      ডিফল্ট build ("factory" হতে পারে)
 * শেষ KEEP টা kernel রাখা হয়; ডিফল্টটা কখনো মোছা হয় না।
 * kernel-এর পরিচয়: ফাইলের ভেতরের "ROZOS-BUILD:<id>:<compiler>:<where>:<date>" (buildinfo.h)। */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

#define BOOT       "/boot"
#define KDIR       BOOT "/kernels"
#define LIST_FILE  KDIR "/list"
#define DEF_FILE   KDIR "/default"
#define RES_FILE   KDIR "/resolution"      /* ধাপ ১৩ */
#define CONF_FILE  BOOT "/limine.conf"
#define FACTORY    BOOT "/rozos-factory.elf"
#define DEFAULT_SRC "/usr/src/rozos/build/rozos.elf"
#define KEEP       3
#define MAX_IDS    32

struct tag {
    char id[32], compiler[16], where[32], date[40];
};

static char ids[MAX_IDS][32];
static int nids;
static char def_id[32];

/* ---------- ফাইল ---------- */

static unsigned char *read_file(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf = NULL;
    long cap = 0, n = 0;
    size_t r;
    if (!f) return NULL;
    for (;;) {
        if (n + 65536 > cap) {
            unsigned char *nb;
            cap = cap ? cap * 2 : 1 << 20;
            nb = (unsigned char *)realloc(buf, (size_t)cap);
            if (!nb) { free(buf); fclose(f); return NULL; }
            buf = nb;
        }
        r = fread(buf + n, 1, 65536, f);
        if (r == 0) break;
        n += (long)r;
    }
    fclose(f);
    *size = n;
    return buf;
}

static int write_file(const char *path, const void *data, long size)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (size && fwrite(data, 1, (size_t)size, f) != (size_t)size) { fclose(f); return -1; }
    return fclose(f) == 0 ? 0 : -1;
}

static int find_tag(const unsigned char *buf, long size, struct tag *t)
{
    static const char key[] = "ROZOS-BUILD:";
    long i;
    memset(t, 0, sizeof(*t));
    for (i = 0; i + 12 < size; i++) {
        if (buf[i] == 'R' && !memcmp(buf + i, key, 12)) {
            const char *p = (const char *)buf + i + 12;
            char *fields[4] = { t->id, t->compiler, t->where, t->date };
            size_t lens[4] = { sizeof(t->id), sizeof(t->compiler), sizeof(t->where), sizeof(t->date) };
            int k;
            for (k = 0; k < 4; k++) {
                size_t n = 0;
                while (*p && (k == 3 || *p != ':') && (const unsigned char *)p < buf + size) {
                    if (n + 1 < lens[k]) fields[k][n++] = *p;
                    p++;
                }
                fields[k][n] = '\0';
                if (*p == ':') p++;
            }
            return t->id[0] ? 0 : -1;
        }
    }
    return -1;
}

static int id_ok(const char *id)
{
    if (!*id || strlen(id) > 24) return 0;
    for (; *id; id++) if (!((*id >= '0' && *id <= '9') || *id == '.')) return 0;
    return 1;
}

static void kernel_path(const char *id, char *out, size_t size)
{
    if (!strcmp(id, "factory")) snprintf(out, size, "%s", FACTORY);
    else snprintf(out, size, KDIR "/rozos-%s.elf", id);
}

static int file_tag(const char *path, struct tag *t, long *size)
{
    long n = 0;
    unsigned char *b = read_file(path, &n);
    int r;
    if (!b) return -1;
    r = find_tag(b, n, t);
    free(b);
    if (size) *size = n;
    return r;
}

/* ---------- list / default ---------- */

static void load_state(void)
{
    FILE *f = fopen(LIST_FILE, "r");
    char line[64];
    nids = 0;
    if (f) {
        while (nids < MAX_IDS && fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n ")] = '\0';
            if (id_ok(line)) snprintf(ids[nids++], sizeof(ids[0]), "%s", line);
        }
        fclose(f);
    }
    def_id[0] = '\0';
    f = fopen(DEF_FILE, "r");
    if (f) {
        if (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n ")] = '\0';
            snprintf(def_id, sizeof(def_id), "%s", line);
        }
        fclose(f);
    }
    if (!def_id[0]) snprintf(def_id, sizeof(def_id), "%s", nids ? ids[0] : "factory");
}

static int save_state(void)
{
    FILE *f = fopen(LIST_FILE, "w");
    int i;
    if (!f) return -1;
    for (i = 0; i < nids; i++) fprintf(f, "%s\n", ids[i]);
    fclose(f);
    f = fopen(DEF_FILE, "w");
    if (!f) return -1;
    fprintf(f, "%s\n", def_id);
    fclose(f);
    return 0;
}

static int has_id(const char *id)
{
    int i;
    if (!strcmp(id, "factory")) return 1;
    for (i = 0; i < nids; i++) if (!strcmp(ids[i], id)) return 1;
    return 0;
}

/* ---------- limine.conf ---------- */

static char resolution[32];               /* ধাপ ১৩: "" = auto */

static void load_resolution(void)
{
    FILE *f = fopen(RES_FILE, "r");
    resolution[0] = '\0';
    if (f) {
        if (fgets(resolution, sizeof(resolution), f)) resolution[strcspn(resolution, "\r\n ")] = '\0';
        fclose(f);
    }
}

static void entry(FILE *f, const char *title, const char *path, const char *extra)
{
    fprintf(f, "/%s\n", title);
    fprintf(f, "    protocol: limine\n");
    if (resolution[0]) fprintf(f, "    resolution: %s\n", resolution);
    fprintf(f, "    path: boot():%s\n", path);
    fprintf(f, "    cmdline: console=fb,serial%s\n", extra);
    fprintf(f, "    module_path: boot():/initrd.tar\n");
    fprintf(f, "    module_cmdline: initrd\n\n");
}

static int write_menu(void)
{
    FILE *f = fopen(CONF_FILE ".new", "w");
    struct tag t;
    char title[160], path[128];
    int i, n = 0, def = 1;
    if (!f) {
        fprintf(stderr, "kinstall: %s: %s\n", CONF_FILE, strerror(errno));
        return -1;
    }
    fprintf(f, "# rozOS boot menu — kinstall লেখে (হাতে বদলালে পরের kinstall-এ নতুন করে লেখা হবে)\n");
    fprintf(f, "# help kinstall, help boot-menu\n\n");
    fprintf(f, "timeout: 3\n");
    load_resolution();
    for (i = 0; i < nids; i++) if (!strcmp(ids[i], def_id)) def = i + 1;
    if (!strcmp(def_id, "factory")) def = nids + 1;
    fprintf(f, "default_entry: %d\n\n", def);
    for (i = 0; i < nids; i++) {
        kernel_path(ids[i], path, sizeof(path));
        if (file_tag(path, &t, NULL) < 0) snprintf(t.compiler, sizeof(t.compiler), "?");
        snprintf(title, sizeof(title), "rozOS build %s (%s, %s)%s", ids[i], t.compiler, t.date,
                 !strcmp(ids[i], def_id) ? " - default" : (n == 0 ? " - newest" : ""));
        entry(f, title, path + strlen(BOOT), "");
        n++;
    }
    if (file_tag(FACTORY, &t, NULL) < 0) snprintf(t.id, sizeof(t.id), "?");
    snprintf(title, sizeof(title), "rozOS factory (build %s, gcc)%s", t.id, !strcmp(def_id, "factory") ? " - default" : "");
    entry(f, title, "/rozos-factory.elf", "");
    snprintf(title, sizeof(title), "rozOS factory (text mode, no desktop)");   /* ধাপ ১৪ */
    entry(f, title, "/rozos-factory.elf", " text");
    snprintf(title, sizeof(title), "rozOS factory (no init: kernel monitor)");
    entry(f, title, "/rozos-factory.elf", " noinit");
    fclose(f);
    unlink(CONF_FILE);
    {
        long sz;
        unsigned char *b = read_file(CONF_FILE ".new", &sz);
        if (!b || write_file(CONF_FILE, b, sz) < 0) {
            free(b);
            fprintf(stderr, "kinstall: cannot write %s\n", CONF_FILE);
            return -1;
        }
        free(b);
    }
    unlink(CONF_FILE ".new");
    return 0;
}

/* ---------- কমান্ড ---------- */

static int check_boot(void)
{
    struct stat st;
    if (stat(FACTORY, &st) < 0 && stat(CONF_FILE, &st) < 0) {
        fprintf(stderr, "kinstall: the boot partition is not mounted on /boot\n"
                        "  (old disk without partitions? on the PC: make disk-migrate — see help kinstall)\n");
        return -1;
    }
    mkdir(KDIR, 0755);
    return 0;
}

static int cmd_install(const char *src)
{
    long size = 0;
    unsigned char *buf = read_file(src, &size);
    struct tag t;
    char path[128];
    int i, j;

    if (!buf) {
        fprintf(stderr, "kinstall: %s: %s\n", src, strerror(errno));
        if (!strcmp(src, DEFAULT_SRC)) fprintf(stderr, "  build it first: cd /usr/src/rozos && make\n");
        return 1;
    }
    /* ELF64, x86-64, executable, entry ঠিকানা kernel-এর অর্ধেকে */
    if (size < 64 || memcmp(buf, "\177ELF", 4) || buf[4] != 2 || buf[18] != 62 || buf[16] != 2 ||
        *(unsigned long *)(buf + 24) < 0xffffffff80000000ul) {
        fprintf(stderr, "kinstall: %s is not a rozOS kernel (x86-64 ELF at 0xffffffff80000000)\n", src);
        free(buf);
        return 1;
    }
    if (find_tag(buf, size, &t) < 0 || !id_ok(t.id)) {
        fprintf(stderr, "kinstall: %s has no build number (ROZOS-BUILD tag)\n", src);
        free(buf);
        return 1;
    }
    load_state();
    kernel_path(t.id, path, sizeof(path));
    printf("installing build %s (%s on %s, %s), %ld KiB\n", t.id, t.compiler, t.where, t.date, size / 1024);
    if (write_file(path, buf, size) < 0) {
        fprintf(stderr, "kinstall: %s: %s\n", path, strerror(errno));
        free(buf);
        return 1;
    }
    free(buf);
    /* তালিকায় সবার আগে (আগে থাকলে সরিয়ে) */
    for (i = 0, j = 0; i < nids; i++) if (strcmp(ids[i], t.id)) memmove(ids[j++], ids[i], sizeof(ids[0]));
    nids = j;
    if (nids == MAX_IDS) nids--;
    memmove(ids[1], ids[0], sizeof(ids[0]) * (size_t)nids);
    snprintf(ids[0], sizeof(ids[0]), "%s", t.id);
    nids++;
    snprintf(def_id, sizeof(def_id), "%s", t.id);
    /* পুরোনো বাড়তিগুলো মোছা */
    while (nids > KEEP) {
        char old[128];
        nids--;
        kernel_path(ids[nids], old, sizeof(old));
        unlink(old);
        printf("  removed old build %s\n", ids[nids]);
    }
    if (save_state() < 0 || write_menu() < 0) return 1;
    printf("  %s\n  boot menu updated: build %s is the default. Now: reboot\n", path, t.id);
    printf("  (if it does not boot: choose an older entry in the menu, then kinstall -d ID)\n");
    return 0;
}

static int cmd_list(void)
{
    struct tag t;
    char path[128];
    long size;
    int i;
    load_state();
    printf("  %-10s %-6s %-12s %-22s %8s\n", "build", "cc", "built on", "date", "KiB");
    for (i = 0; i <= nids; i++) {
        const char *id = i < nids ? ids[i] : "factory";
        kernel_path(id, path, sizeof(path));
        if (file_tag(path, &t, &size) < 0) {
            printf("  %-10s (missing or unreadable: %s)\n", id, path);
            continue;
        }
        {
            /* ধাপ ১২ক: factory-র পাশে PC-র বিল্ড নম্বরও (যেমন "factory#7") */
            char shown[40];
            if (i < nids) snprintf(shown, sizeof(shown), "%s", t.id);
            else snprintf(shown, sizeof(shown), "factory#%s", t.id);
            printf("%s %-10s %-6s %-12s %-22s %8ld%s\n", !strcmp(id, def_id) ? "*" : " ",
                   shown, t.compiler, t.where, t.date, size / 1024,
                   i < nids ? "" : "  (never removed)");
        }
    }
    load_resolution();
    printf("  * = default.  Boot menu: %s   Screen: %s\n", CONF_FILE,
           resolution[0] ? resolution : "auto (kinstall --resolution WxH)");
    return 0;
}

static int cmd_default(const char *id)
{
    load_state();
    if (!has_id(id)) {
        fprintf(stderr, "kinstall: no build %s (see kinstall -l)\n", id);
        return 1;
    }
    snprintf(def_id, sizeof(def_id), "%s", id);
    if (save_state() < 0 || write_menu() < 0) return 1;
    printf("build %s is now the default\n", id);
    return 0;
}

static int cmd_remove(const char *id)
{
    char path[128];
    int i, j;
    load_state();
    if (!strcmp(id, "factory")) {
        fprintf(stderr, "kinstall: the factory kernel is never removed\n");
        return 1;
    }
    if (!has_id(id)) {
        fprintf(stderr, "kinstall: no build %s\n", id);
        return 1;
    }
    if (!strcmp(id, def_id)) {
        fprintf(stderr, "kinstall: build %s is the default — choose another first (kinstall -d)\n", id);
        return 1;
    }
    kernel_path(id, path, sizeof(path));
    unlink(path);
    for (i = 0, j = 0; i < nids; i++) if (strcmp(ids[i], id)) memmove(ids[j++], ids[i], sizeof(ids[0]));
    nids = j;
    if (save_state() < 0 || write_menu() < 0) return 1;
    printf("removed build %s\n", id);
    return 0;
}

/* ধাপ ১৩: WxH বা WxHxBPP যাচাই */
static int res_ok(const char *r)
{
    unsigned w = 0, h = 0, b = 32;
    char tail;
    int n = sscanf(r, "%ux%ux%u%c", &w, &h, &b, &tail);
    if (n != 2 && n != 3) return 0;
    return w >= 320 && w <= 7680 && h >= 200 && h <= 4320 && (b == 32 || b == 24 || b == 16);
}

static int cmd_resolution(const char *r)
{
    FILE *f;
    load_state();
    if (!strcmp(r, "auto")) {
        unlink(RES_FILE);
    } else {
        if (!res_ok(r)) {
            fprintf(stderr, "kinstall: '%s' - use WIDTHxHEIGHT like 1024x768 or 1920x1080 (or auto)\n", r);
            return 2;
        }
        f = fopen(RES_FILE, "w");
        if (!f) {
            fprintf(stderr, "kinstall: %s: %s\n", RES_FILE, strerror(errno));
            return 1;
        }
        fprintf(f, "%s\n", r);
        fclose(f);
    }
    if (write_menu() < 0) return 1;
    printf("screen resolution: %s - from the next boot (reboot). If the screen stays black,\n"
           "the firmware cannot do it: Limine then picks another mode by itself.\n",
           strcmp(r, "auto") ? r : "auto (Limine chooses)");
    return 0;
}

static void usage(void)
{
    fprintf(stderr, "usage: kinstall [FILE]     install a kernel (default " DEFAULT_SRC ")\n"
                    "       kinstall -l         list installed kernels\n"
                    "       kinstall -d ID      make build ID (or factory) the default\n"
                    "       kinstall -r ID      remove build ID\n"
                    "       kinstall -m         rewrite " CONF_FILE "\n"
                    "       kinstall --resolution WxH|auto   screen size from the next boot\n"
                    "see: help kinstall\n");
}

int main(int argc, char **argv)
{
    if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        usage();
        return 0;
    }
    if (check_boot() < 0) return 1;
    if (argc == 1) return cmd_install(DEFAULT_SRC);
    if (!strcmp(argv[1], "-l") || !strcmp(argv[1], "--list")) return cmd_list();
    if (!strcmp(argv[1], "-m") || !strcmp(argv[1], "--menu")) { load_state(); return write_menu() < 0; }
    if ((!strcmp(argv[1], "-d") || !strcmp(argv[1], "--default")) && argc == 3) return cmd_default(argv[2]);
    if ((!strcmp(argv[1], "-r") || !strcmp(argv[1], "--remove")) && argc == 3) return cmd_remove(argv[2]);
    if ((!strcmp(argv[1], "--resolution") || !strcmp(argv[1], "-R")) && argc == 3) return cmd_resolution(argv[2]);
    if (argv[1][0] != '-' && argc == 2) return cmd_install(argv[1]);
    usage();
    return 2;
}
