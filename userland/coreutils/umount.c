/* rozOS — umount FOLDER (ধাপ ১৫ক): জোড়া ডিস্ক খোলা; আগে সব লেখা ডিস্কে যায়।
 * কোনো ফাইল খোলা থাকলে (বা ভেতরে আরেকটা mount) "busy" — বন্ধ করে আবার চেষ্টা করুন। */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/mount.h>

int main(int argc, char **argv)
{
    int i, rc = 0;
    if (argc < 2) { fprintf(stderr, "usage: umount FOLDER...\n"); return 2; }
    for (i = 1; i < argc; i++) {
        if (umount(argv[i]) != 0) {
            fprintf(stderr, "umount: %s: %s\n", argv[i],
                    errno == EBUSY ? "busy (a file is open there, or it is / )" :
                    errno == EINVAL ? "nothing is mounted there" : strerror(errno));
            rc = 1;
        } else {
            printf("umount: %s - safe to remove\n", argv[i]);
        }
    }
    return rc;
}
