/* rozOS — lsusb: লাগানো USB ডিভাইস (ধাপ ১৫খ)
 * প্রতিটা সারি: কন্ট্রোলার, port, vendor:product, গতি, ধরন, ডিস্কের নাম, ডিভাইসের নাম।
 * পেন ড্রাইভ হলে ডিস্ক (sdb ...) আর কোথায় mount — `mount` দেখায়। */
#include <stdio.h>
#include <string.h>
#include <rozos.h>

static char buf[8192];

int main(void)
{
    long n = rozos_usb_list(buf, sizeof(buf));
    char *line = buf;
    if (n < 0) { perror("lsusb"); return 1; }
    if (n == 0) {
        printf("no USB devices (or no xHCI controller - in QEMU: make run USB=1)\n");
        return 0;
    }
    printf("%-4s %-5s %-10s %-9s %-9s %-6s %s\n", "Bus", "Port", "ID", "Speed", "Type", "Disk", "Name");
    while (*line) {
        int bus = 0, port = 0, used = 0;
        char id[16] = "", sp[3][12], kind[16] = "", disk[16] = "";
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        memset(sp, 0, sizeof(sp));
        /* গতি দুই শব্দ ("480 Mb/s") */
        sscanf(line, "%d %d %15s %11s %11s %15s %15s %n", &bus, &port, id, sp[0], sp[1], kind, disk, &used);
        snprintf(sp[2], sizeof(sp[2]), "%s %s", sp[0], sp[1]);
        printf("%-4d %-5d %-10s %-9s %-9s %-6s %s\n", bus, port, id, sp[2], kind, disk, used ? line + used : "");
        if (!nl) break;
        line = nl + 1;
    }
    return 0;
}
