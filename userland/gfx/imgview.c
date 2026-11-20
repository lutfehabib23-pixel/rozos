/* rozOS — imgview: ছবি দেখা (ধাপ ১৩)
 *   imgview FILE...        .bmp বা .ppm (না দিলে /usr/share/images-এর ছবি)
 *   ← →  / Space           আগের / পরের ছবি
 *   + -  0                 বড় / ছোট / পর্দায় মাপমতো
 *   মাউসের চাকা           বড়-ছোট;  q / Esc  বের হওয়া */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <rozgfx.h>

#define MAX_FILES 64

static char *files[MAX_FILES];
static int nfiles;

static void add_dir(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d)) && nfiles < MAX_FILES) {
        size_t n = strlen(e->d_name);
        if (n > 4 && (!strcmp(e->d_name + n - 4, ".bmp") || !strcmp(e->d_name + n - 4, ".ppm"))) {
            char *p = (char *)malloc(strlen(dir) + n + 2);
            if (!p) break;
            sprintf(p, "%s/%s", dir, e->d_name);
            files[nfiles++] = p;
        }
    }
    closedir(d);
}

int main(int argc, char **argv)
{
    gfx_surface *s, *img = NULL;
    gfx_event ev;
    int i, cur = 0, loaded = -1, zoom = 0;     /* zoom 0 = মাপমতো, নইলে শতাংশ */
    int running = 1;

    for (i = 1; i < argc && nfiles < MAX_FILES; i++) files[nfiles++] = argv[i];
    if (nfiles == 0) add_dir("/usr/share/images");
    if (nfiles == 0) {
        fprintf(stderr, "usage: imgview FILE.bmp|FILE.ppm ...\n");
        return 2;
    }
    if (gfx_init_window("Images", 0, 0) < 0) {
        perror("imgview: /dev/fb0");
        return 1;
    }
    s = gfx_screen();
    while (running) {
        char info[256];
        int W = gfx_width(), H = gfx_height(), dw, dh, pct;
        if (loaded != cur) {
            gfx_surface_free(img);
            img = gfx_load_image(files[cur]);
            loaded = cur;
            zoom = 0;
        }
        gfx_clear(s, GFX_RGB(30, 30, 34));
        if (img) {
            if (zoom == 0) {
                pct = 100;
                if (img->w * pct / 100 > W - 20) pct = (W - 20) * 100 / img->w;
                if (img->h * pct / 100 > H - 60) pct = (H - 60) * 100 / img->h;
                if (pct < 1) pct = 1;
            } else {
                pct = zoom;
            }
            dw = img->w * pct / 100;
            dh = img->h * pct / 100;
            if (pct == 100) gfx_blit(s, (W - dw) / 2, (H - 40 - dh) / 2, img, 0, 0, -1, -1);
            else gfx_blit_scaled(s, (W - dw) / 2, (H - 40 - dh) / 2, dw, dh, img);
            snprintf(info, sizeof(info), " %s  %dx%d  %d%%  (%d/%d)   <- -> next   + - 0 zoom   q quit ",
                     files[cur], img->w, img->h, pct, cur + 1, nfiles);
        } else {
            snprintf(info, sizeof(info), " %s: cannot read (only .bmp 24/32-bit and .ppm)   q quit ", files[cur]);
        }
        gfx_fill_rect(s, 0, H - 28, W, 28, GFX_RGB(50, 50, 60));
        gfx_text(s, NULL, 8, H - 22, info, GFX_WHITE);
        gfx_present();

        if (!gfx_wait_event(&ev, -1)) continue;
        if (ev.type == GFX_EV_CLOSE) running = 0;               /* ধাপ ১৪ */
        if (ev.type == GFX_EV_KEY) {
            int k = ev.key;
            if (k == 'q' || k == GFX_KEY_ESC) running = 0;
            else if (k == GFX_KEY_RIGHT || k == ' ' || k == GFX_KEY_PAGE_DOWN) cur = (cur + 1) % nfiles;
            else if (k == GFX_KEY_LEFT || k == GFX_KEY_PAGE_UP) cur = (cur + nfiles - 1) % nfiles;
            else if (k == '+' || k == '=') zoom = (zoom ? zoom : 100) * 5 / 4;
            else if (k == '-') { zoom = (zoom ? zoom : 100) * 4 / 5; if (zoom < 5) zoom = 5; }
            else if (k == '0') zoom = 0;
        } else if (ev.type == GFX_EV_MOUSE_WHEEL) {
            zoom = (zoom ? zoom : 100);
            zoom = ev.wheel < 0 ? zoom * 5 / 4 : zoom * 4 / 5;
            if (zoom < 5) zoom = 5;
        }
    }
    gfx_quit();
    return 0;
}
