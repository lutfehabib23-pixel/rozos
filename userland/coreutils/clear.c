/* rozOS — clear: স্ক্রিন মোছা (ANSI) */

#include <unistd.h>

int main(void)
{
    write(1, "\x1b[0m\x1b[2J\x1b[H", 11);
    return 0;
}
