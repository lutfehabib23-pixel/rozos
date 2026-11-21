/* rozOS — lsmod: লোড করা কার্নেল মডিউল (ধাপ ১৫ক)
 *   lsmod       নাম, আকার, বিবরণ
 *   lsmod -k    মডিউল কোন কোন কার্নেল-ফাংশন ব্যবহার করতে পারে (রপ্তানি তালিকা) */
#include <stdio.h>
#include <string.h>
#include <rozos.h>

static char buf[65536];

int main(int argc, char **argv)
{
    int ks = argc > 1 && !strcmp(argv[1], "-k");
    long n = rozos_module_list(buf, sizeof(buf), ks);
    char *line, *save;
    if (n < 0) { perror("lsmod"); return 1; }
    if (ks) { fputs(buf, stdout); return 0; }
    printf("%-16s %8s  %s\n", "Module", "Size", "Description");
    if (n == 0) { printf("(no modules loaded - try: insmod hello)\n"); return 0; }
    for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char name[40] = "", size[24] = "";
        const char *desc = "";
        sscanf(line, "%39s %23s", name, size);
        {
            const char *p = strchr(line, ' ');
            if (p) p = strchr(p + 1, ' ');
            if (p) desc = p + 1;
        }
        printf("%-16s %8s  %s\n", name, size, desc);
    }
    return 0;
}
