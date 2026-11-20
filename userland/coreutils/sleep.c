/* rozOS — sleep <seconds>[.fraction] */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    char *end;
    long sec, usec = 0;
    if (argc < 2) { fprintf(stderr, "usage: sleep <seconds>\n"); return 2; }
    sec = strtol(argv[1], &end, 10);
    if (*end == '.') {
        long scale = 100000;
        for (end++; *end >= '0' && *end <= '9' && scale; end++, scale /= 10) usec += (*end - '0') * scale;
    }
    usleep((unsigned long)(sec * 1000000 + usec));
    return 0;
}
