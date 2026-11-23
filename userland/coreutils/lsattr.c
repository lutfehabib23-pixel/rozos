/* rozOS — lsattr: ফাইলের flag দেখা (ধাপ ১৯ছ)
 *
 *   lsattr FILE...       "----i---- FILE" (i = immutable) বা "--------- FILE"
 *   lsattr               এই ফোল্ডারের সব কিছু */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>

static int show(const char *p)
{
    int fd = open(p, O_RDONLY), fl = 0;
    if (fd < 0 || ioctl(fd, FS_IOC_GETFLAGS, &fl) < 0) {
        fprintf(stderr, "lsattr: %s: %s\n", p, strerror(errno));
        if (fd >= 0) close(fd);
        return 1;
    }
    close(fd);
    printf("%s %s\n", (fl & FS_IMMUTABLE_FL) ? "----i----" : "---------", p);
    return 0;
}

int main(int argc, char **argv)
{
    int i, rc = 0;
    if (argc < 2) {
        DIR *d = opendir(".");
        struct dirent *e;
        while (d && (e = readdir(d)))
            if (e->d_name[0] != '.') rc |= show(e->d_name);
        if (d) closedir(d);
        return rc;
    }
    for (i = 1; i < argc; i++) rc |= show(argv[i]);
    return rc;
}
