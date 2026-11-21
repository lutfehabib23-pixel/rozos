/* rozOS — mount (ধাপ ১৫ক)
 *   mount                          কী কোথায় জোড়া আছে
 *   mount /dev/nvmea1 /mnt/nvme    ডিস্ক/পার্টিশন জোড়া (ext2 বা FAT নিজে চেনে; ফোল্ডার না
 *                                  থাকলে বানিয়ে নেয়)
 *   mount -t fat /dev/sdb1 /mnt/usb   ধরন বলে দেওয়া (ext2, fat, tmpfs)
 * খোলা: umount /mnt/nvme। কোন ডিস্ক আছে: ls /dev (sda, nvmea ... আর তাদের পার্টিশন)। */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <rozos.h>

int main(int argc, char **argv)
{
    const char *type = "auto";
    int a = 1;
    struct stat st;
    if (argc == 1) {
        static char buf[8192];
        if (rozos_mounts(buf, sizeof(buf)) < 0) { perror("mount"); return 1; }
        {
            char *line = buf;
            while (*line) {
                char dev[64] = "", path[256] = "", fs[32] = "";
                char *nl = strchr(line, '\n');
                if (nl) *nl = '\0';
                sscanf(line, "%63s %255s %31s", dev, path, fs);
                printf("%-10s on %-16s type %s\n", dev, path, fs);
                if (!nl) break;
                line = nl + 1;
            }
        }
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "-t")) { type = argv[2]; a = 3; }
    if (argc - a != 2) {
        fprintf(stderr, "usage: mount [-t ext2|fat|tmpfs] DEVICE FOLDER   (mount alone lists)\n");
        return 2;
    }
    if (stat(argv[a + 1], &st) != 0) {
        if (mkdir(argv[a + 1], 0755) != 0) { fprintf(stderr, "mount: %s: %s\n", argv[a + 1], strerror(errno)); return 1; }
    }
    if (mount(argv[a], argv[a + 1], type, 0, NULL) != 0) {
        const char *why = errno == ENODEV ? "no such disk (see ls /dev)" :
                          errno == EINVAL ? "no ext2 or FAT file system found" :
                          errno == EBUSY ? "already mounted, or the folder is in use" : strerror(errno);
        fprintf(stderr, "mount: %s on %s: %s\n", argv[a], argv[a + 1], why);
        return 1;
    }
    printf("mount: %s on %s\n", argv[a], argv[a + 1]);
    return 0;
}
