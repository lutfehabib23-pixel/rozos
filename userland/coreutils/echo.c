/* rozOS — echo [-n] text... (shell-এর built-in echo-ও আছে; এটা স্ক্রিপ্ট/অন্য প্রোগ্রামের জন্য) */

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    int i = 1, nl = 1;
    if (argc > 1 && !strcmp(argv[1], "-n")) { nl = 0; i = 2; }
    for (; i < argc; i++) printf("%s%s", argv[i], i + 1 < argc ? " " : "");
    if (nl) printf("\n");
    return 0;
}
