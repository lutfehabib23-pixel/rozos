/* rozOS — date: এখনকার তারিখ ও সময়
 *
 * ধাপ ১৬ক: সময় অঞ্চল মেনে (TZ বা /etc/TZ; কিছু না থাকলে আগের মতো UTC)।
 *   date            স্থানীয় সময়, যেমন "Sun 2026-11-22 07:31:05 +06"
 *   date -u         UTC-তে
 *   date +FORMAT    strftime-এর ফরম্যাটে, যেমন date +%H:%M */

#include <stdio.h>
#include <string.h>
#include <time.h>

int main(int argc, char **argv)
{
    char buf[256];
    const char *fmt = "%a %Y-%m-%d %H:%M:%S %Z";
    int utc = 0, i;
    time_t t = time(NULL);
    struct tm tm;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-u") || !strcmp(argv[i], "--utc")) utc = 1;
        else if (argv[i][0] == '+') fmt = argv[i] + 1;
        else {
            fprintf(stderr, "usage: date [-u] [+FORMAT]\n");
            return 1;
        }
    }
    if (utc) gmtime_r(&t, &tm);
    else localtime_r(&t, &tm);
    if (!strftime(buf, sizeof(buf), fmt, &tm) && *fmt) {
        fprintf(stderr, "date: format too long\n");
        return 1;
    }
    printf("%s\n", buf);
    return 0;
}
