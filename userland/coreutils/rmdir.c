/* rozOS — rmdir dir...: খালি ডিরেক্টরি মোছা */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

int main(int argc, char **argv)
{
    int i, r = 0;
    if (argc < 2) { fprintf(stderr, "usage: rmdir <dir>...\n"); return 2; }
    for (i = 1; i < argc; i++) {
        if (rmdir(argv[i]) < 0) {
            fprintf(stderr, "rmdir: %s: %s\n", argv[i], strerror(errno));
            r = 1;
        }
    }
    return r;
}
