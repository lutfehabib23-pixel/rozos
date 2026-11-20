/* rozOS — uname [-a]: সিস্টেমের নাম */

#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>

int main(int argc, char **argv)
{
    struct utsname u;
    if (uname(&u) < 0) { perror("uname"); return 1; }
    if (argc > 1 && !strcmp(argv[1], "-a")) {
        printf("%s %s %s %s %s\n", u.sysname, u.nodename, u.release, u.version, u.machine);
    } else {
        printf("%s\n", u.sysname);
    }
    return 0;
}
