/* rozOS — touch file...: না থাকলে খালি ফাইল তৈরি */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

int main(int argc, char **argv)
{
    int i, r = 0;
    if (argc < 2) { fprintf(stderr, "usage: touch <file>...\n"); return 2; }
    for (i = 1; i < argc; i++) {
        int fd = open(argv[i], O_WRONLY | O_CREAT, 0644);
        if (fd < 0) { fprintf(stderr, "touch: %s: %s\n", argv[i], strerror(errno)); r = 1; }
        else close(fd);
    }
    return r;
}
