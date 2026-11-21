/* rozOS — insmod FILE.ko [NAME]: কার্নেল মডিউল ঢোকানো (ধাপ ১৫ক)
 *   insmod hello          → /lib/modules/hello.ko
 *   insmod ./mydrv.ko     → নিজের বানানো (tcc -c -I/usr/src/rozos/kernel/include mydrv.c -o mydrv.ko)
 * ভুল হলে কারণ কার্নেলের বার্তায় (dmesg নেই — কনসোলে/serial-এ দেখা যায়) আর এখানে। */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <rozos.h>

int main(int argc, char **argv)
{
    char path[512], name[32];
    const char *base, *dot;
    FILE *f;
    long n;
    char *buf;
    if (argc < 2) {
        fprintf(stderr, "usage: insmod FILE.ko [NAME]   (or a name from /lib/modules)\n");
        return 2;
    }
    if (strchr(argv[1], '/') || strstr(argv[1], ".ko")) snprintf(path, sizeof(path), "%s", argv[1]);
    else snprintf(path, sizeof(path), "/lib/modules/%s.ko", argv[1]);
    base = strrchr(path, '/');
    base = base ? base + 1 : path;
    dot = strstr(base, ".ko");
    snprintf(name, sizeof(name), "%.*s", dot ? (int)(dot - base) : (int)strlen(base), base);
    if (argc > 2) snprintf(name, sizeof(name), "%s", argv[2]);
    f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "insmod: %s: %s\n", path, strerror(errno)); return 1; }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *)malloc(n > 0 ? (size_t)n : 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "insmod: cannot read %s\n", path); return 1; }
    fclose(f);
    if (rozos_init_module(buf, (unsigned long)n, name) < 0) {
        fprintf(stderr, "insmod: %s: %s (the kernel message above says why)\n", path, strerror(errno));
        return 1;
    }
    printf("insmod: %s loaded (see lsmod)\n", path);
    return 0;
}
