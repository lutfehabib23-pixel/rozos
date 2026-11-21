/* rozOS — rmmod NAME: কার্নেল মডিউল বের করা (ধাপ ১৫ক); নাম lsmod-এ দেখা যায় */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <rozos.h>

int main(int argc, char **argv)
{
    int i, rc = 0;
    if (argc < 2) { fprintf(stderr, "usage: rmmod NAME...\n"); return 2; }
    for (i = 1; i < argc; i++) {
        if (rozos_delete_module(argv[i]) < 0) {
            fprintf(stderr, "rmmod: %s: %s\n", argv[i], errno == ENOENT ? "not loaded (see lsmod)" : strerror(errno));
            rc = 1;
        }
    }
    return rc;
}
