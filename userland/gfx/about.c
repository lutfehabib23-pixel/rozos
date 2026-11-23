/* rozOS — About rozOS (ধাপ ২১)
 *
 *   about          ভার্সন, বিল্ড নম্বর, কার্নেল, CPU, মেমোরি, ডিস্ক, চালু থাকার সময়, লাইসেন্স
 *
 * Start > About rozOS, ডেস্কটপের ডান-ক্লিক, Settings > System থেকে খোলে। "Copy" সব তথ্য
 * ক্লিপবোর্ডে দেয় (বাগ রিপোর্টে কাজে লাগে)। ছবি: /usr/share/images/rozos-boot.jpg। */

#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#include <sys/sysinfo.h>
#include <sys/statvfs.h>
#include <rozgfx.h>
#include <rozui.h>

static void cpu_brand(char *out, size_t n)
{
    unsigned r[12], i;
    unsigned a, b, c, d;
    __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000u), "c"(0));
    if (a < 0x80000004u) { snprintf(out, n, "x86-64 CPU"); return; }
    for (i = 0; i < 3; i++) {
        __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000002u + i), "c"(0));
        r[i * 4] = a; r[i * 4 + 1] = b; r[i * 4 + 2] = c; r[i * 4 + 3] = d;
    }
    {
        const char *s = (const char *)r;
        size_t k = 0;
        while (*s == ' ') s++;
        for (; k + 1 < n && *s && s < (const char *)r + 48; s++) out[k++] = *s;
        while (k && out[k - 1] == ' ') k--;
        out[k] = '\0';
    }
}

static void human(char *out, size_t n, unsigned long long v)
{
    if (v >= (1ULL << 30)) snprintf(out, n, "%llu.%llu GB", v >> 30, (v % (1ULL << 30)) * 10 >> 30);
    else snprintf(out, n, "%llu MB", v >> 20);
}

int main(void)
{
    static const char *const credits1 = "Limine, TinyCC, pdpmake, lwIP, BearSSL,";
    static const char *const credits2 = "Lua, zlib, stb, sbase, DejaVu fonts";
    char lines[8][160], all[1400], cpu[64], a[24], b[24];
    int nl = 0, i;
    struct utsname un;
    struct sysinfo si;
    struct statvfs vf;
    gfx_surface *logo = NULL;
    ui_ctx U;

    memset(&un, 0, sizeof(un));
    uname(&un);
    cpu_brand(cpu, sizeof(cpu));
    snprintf(lines[nl++], 160, "Version:  %s", un.release);
    snprintf(lines[nl++], 160, "Build:    %s", un.version);
    snprintf(lines[nl++], 160, "Machine:  %s, %s", un.machine, cpu);
    if (sysinfo(&si) == 0) {
        human(a, sizeof(a), (unsigned long long)si.totalram * si.mem_unit);
        human(b, sizeof(b), (unsigned long long)si.freeram * si.mem_unit);
        snprintf(lines[nl++], 160, "Memory:   %s (%s free)", a, b);
        snprintf(lines[nl++], 160, "Running:  %ldh %02ldm, %u processes", si.uptime / 3600, si.uptime / 60 % 60, (unsigned)si.procs);
    }
    if (statvfs("/", &vf) == 0) {
        human(a, sizeof(a), (unsigned long long)vf.f_blocks * vf.f_frsize);
        human(b, sizeof(b), (unsigned long long)vf.f_bavail * vf.f_frsize);
        snprintf(lines[nl++], 160, "Disk /:   %s (%s free)", a, b);
    }
    all[0] = '\0';
    strcat(all, "rozOS - a self-hosting 64-bit operating system\n");
    for (i = 0; i < nl; i++) { strcat(all, lines[i]); strcat(all, "\n"); }
    strcat(all, "License: MIT   github.com/lutfehabib23-pixel/rozos\n");

    if (gfx_init_window("About rozOS", 560, 440) < 0) {
        fputs(all, stdout);                      /* ডেস্কটপ ছাড়া: শুধু লেখা */
        return 0;
    }
    {
        gfx_surface *img = gfx_load_image("/usr/share/images/rozos-boot.jpg");
        if (img) {
            logo = gfx_surface_new(160, 120);
            if (logo) gfx_blit_scaled(logo, 0, 0, 160, 120, img);
            gfx_surface_free(img);
        }
    }
    ui_init(&U);
    while (ui_run(&U)) {
        const struct ui_theme *T = ui_theme_get();
        int y = 20;
        gfx_clear(U.s, T->window_bg);
        if (logo) gfx_blit(U.s, 20, 20, logo, 0, 0, -1, -1);
        else ui_draw_icon(U.s, 20, 20, 64, "about", 's');
        gfx_text(U.s, NULL, 200, 26, "rozOS", T->text);
        gfx_text(U.s, NULL, 200, 48, "A self-hosting 64-bit operating system,", T->text_dim);
        gfx_text(U.s, NULL, 200, 66, "built from scratch in C and assembly.", T->text_dim);
        gfx_text(U.s, NULL, 200, 98, un.release, T->text);
        y = 160;
        for (i = 0; i < nl; i++, y += 22) gfx_text(U.s, NULL, 20, y, lines[i], T->text);
        y += 8;
        gfx_text(U.s, NULL, 20, y, "License: MIT    github.com/lutfehabib23-pixel/rozos", T->text_dim);
        gfx_text(U.s, NULL, 20, y + 20, "Thanks to:", T->text_dim);
        gfx_text(U.s, NULL, 20, y + 38, credits1, T->text_dim);
        gfx_text(U.s, NULL, 20, y + 56, credits2, T->text_dim);
        if (ui_do_button(&U, U.w - 200, U.h - 42, 88, 28, "Copy")) ui_clip_set(all, (int)strlen(all));
        if (ui_do_button(&U, U.w - 104, U.h - 42, 88, 28, "Close") || U.key == GFX_KEY_ESC || U.key == '\n')
            ui_quit(&U);
    }
    gfx_quit();
    return 0;
}
