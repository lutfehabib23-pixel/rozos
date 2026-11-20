/* rozOS — mkbuildinfo [DIR]: rozOS-এর ভেতরের kernel বিল্ডের জন্য buildinfo.c (ধাপ ১১খ)
 *
 * stdout-এ C ফাইল লেখে (rozos/buildinfo.h-এর প্রতীকগুলো)। বিল্ড নম্বর = "<base>.<n>":
 *   DIR/.build-base  PC-র শেষ বিল্ড নম্বর (make disk-update লেখে)
 *   DIR/.build-self  rozOS-এর ভেতরে কতবার বানানো হলো — প্রতিবার +১ করে লেখা হয়
 * /usr/src/rozos/Makefile লিংকের আগে চালায়:  mkbuildinfo . > build/buildinfo.c */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static long read_num(const char *path)
{
    FILE *f = fopen(path, "r");
    long v = 0;
    char line[32];
    if (!f) return 0;
    if (fgets(line, sizeof(line), f)) v = atol(line);
    fclose(f);
    return v < 0 ? 0 : v;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    char base_path[512], self_path[512], date[40];
    long base, self;
    time_t now = time(NULL);
    FILE *f;

    snprintf(base_path, sizeof(base_path), "%s/.build-base", dir);
    snprintf(self_path, sizeof(self_path), "%s/.build-self", dir);
    base = read_num(base_path);
    self = read_num(self_path) + 1;
    f = fopen(self_path, "w");
    if (!f) {
        perror(self_path);
        return 1;
    }
    fprintf(f, "%ld\n", self);
    fclose(f);
    strftime(date, sizeof(date), "%Y-%m-%d %H:%M UTC", gmtime(&now));
    printf("/* mkbuildinfo তৈরি করে — বদলাবেন না */\n");
    printf("const char rozos_build_id[] = \"%ld.%ld\";\n", base, self);
    printf("const char rozos_build_compiler[] = \"tcc\";\n");
    printf("const char rozos_build_where[] = \"rozOS\";\n");
    printf("const char rozos_build_date[] = \"%s\";\n", date);
    printf("const char rozos_build_tag[] = \"ROZOS-BUILD:%ld.%ld:tcc:rozOS:%s\";\n", base, self, date);
    fprintf(stderr, "build %ld.%ld\n", base, self);
    return 0;
}
