/* rozOS — date: এখনকার তারিখ ও সময় (UTC) */

#include <stdio.h>
#include <time.h>

int main(void)
{
    char buf[64];
    time_t t = time(NULL);
    strftime(buf, sizeof(buf), "%a %Y-%m-%d %H:%M:%S UTC", gmtime(&t));
    printf("%s\n", buf);
    return 0;
}
