/* rozOS — setfont: কনসোলের ফন্ট বদলানো (ধাপ ১৩)
 *
 *   setfont                 কোন ফন্ট আছে তার তালিকা (/usr/share/fonts-এর .psf ফাইল)
 *   setfont rozos-mono-20   /usr/share/fonts/rozos-mono-20.psf (নাম বা পুরো পথ)
 *   setfont -d              বুটের (Limine-এর) ফন্টে ফেরা
 *   setfont -q NAME         চুপচাপ (init বুটের সময় /etc/console-font থেকে চালায়)
 *
 * PSF1 (8 পিক্সেল চওড়া) আর PSF2 (≤ 16 × 32) দুটোই পড়ে; প্রথম 256 গ্লিফ নেয়, VGA/CP437
 * ক্রমে। ফন্ট বদলালে কলাম/সারি বদলায় আর পর্দা মুছে যায়। শুধু ফ্রেমবাফার-কনসোলে কাজ করে
 * (serial টার্মিনালের ফন্ট তোমার PC-র টার্মিনাল ঠিক করে)। */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/ioctl.h>

#define FONT_DIR "/usr/share/fonts"

static unsigned rd32(const unsigned char *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24);
}

static unsigned char *read_all(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    unsigned char *b;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (unsigned char *)malloc(n > 0 ? (size_t)n : 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    *size = n;
    return b;
}

static void list_fonts(void)
{
    DIR *d = opendir(FONT_DIR);
    struct dirent *e;
    printf("Fonts in " FONT_DIR " (setfont NAME, setfont -d = boot font):\n");
    if (!d) return;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n > 4 && !strcmp(e->d_name + n - 4, ".psf")) {
            char path[512];
            long size;
            unsigned char *b;
            snprintf(path, sizeof(path), FONT_DIR "/%s", e->d_name);
            b = read_all(path, &size);
            if (b && size >= 32 && rd32(b) == 0x864AB572) {
                printf("  %-24.*s %ux%u\n", (int)(n - 4), e->d_name, rd32(b + 28), rd32(b + 24));
            } else if (b && size >= 4 && b[0] == 0x36 && b[1] == 0x04) {
                printf("  %-24.*s 8x%u\n", (int)(n - 4), e->d_name, b[3]);
            }
            free(b);
        }
    }
    closedir(d);
}

int main(int argc, char **argv)
{
    struct rozos_font_req req;
    const char *name = NULL;
    char path[512];
    unsigned char *b, *glyphs;
    long size;
    unsigned w, h, count, charsize;
    int quiet = 0, i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-q")) quiet = 1;
        else if (!strcmp(argv[i], "-d")) {
            memset(&req, 0, sizeof(req));
            req.flags = 1;
            if (ioctl(1, TIOCROZSETFONT, &req) < 0) {
                fprintf(stderr, "setfont: %s\n", strerror(errno));
                return 1;
            }
            return 0;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("usage: setfont [-q] NAME|FILE.psf   setfont -d (boot font)   setfont (list)\n");
            return 0;
        } else name = argv[i];
    }
    if (!name) {
        list_fonts();
        return 0;
    }
    if (strchr(name, '/')) snprintf(path, sizeof(path), "%s", name);
    else snprintf(path, sizeof(path), FONT_DIR "/%s%s", name,
                  strlen(name) > 4 && !strcmp(name + strlen(name) - 4, ".psf") ? "" : ".psf");
    b = read_all(path, &size);
    if (!b) {
        if (!quiet) fprintf(stderr, "setfont: %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (size >= 32 && rd32(b) == 0x864AB572) {            /* PSF2 */
        unsigned hdr = rd32(b + 8);
        count = rd32(b + 16);
        charsize = rd32(b + 20);
        h = rd32(b + 24);
        w = rd32(b + 28);
        glyphs = b + hdr;
        if (charsize != h * ((w + 7) / 8) || (long)(hdr + (count < 256 ? count : 256) * charsize) > size) count = 0;
    } else if (size >= 4 && b[0] == 0x36 && b[1] == 0x04) {   /* PSF1 */
        w = 8;
        h = b[3];
        charsize = h;
        count = (b[2] & 1) ? 512 : 256;
        glyphs = b + 4;
        if ((long)(4 + 256 * charsize) > size) count = 0;
    } else {
        if (!quiet) fprintf(stderr, "setfont: %s: not a PSF font\n", path);
        free(b);
        return 1;
    }
    if (count < 256 || w == 0 || w > 16 || h == 0 || h > 32) {
        if (!quiet) fprintf(stderr, "setfont: %s: need 256 glyphs, at most 16x32 (this is %ux%u, %u glyphs)\n",
                            path, w, h, count);
        free(b);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.width = w;
    req.height = h;
    req.data = (unsigned long)glyphs;
    if (ioctl(1, TIOCROZSETFONT, &req) < 0) {
        if (!quiet) fprintf(stderr, "setfont: %s\n", strerror(errno));
        free(b);
        return 1;
    }
    free(b);
    if (!quiet) printf("font %ux%u loaded - the screen now has new columns and rows (stty detect for serial)\n", w, h);
    return 0;
}
