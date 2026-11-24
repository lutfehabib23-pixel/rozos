/* rozOS — fontview: TrueType ফন্ট দেখা (ধাপ ২৩ক)
 *
 *   fontview              বাঁয়ে /usr/share/fonts-এর সব ফন্ট, ডানে বাছাইটা নানা মাপে
 *   fontview FILE         শুধু এই ফন্ট (.ttf / .otf)
 *   fontview -l           ফন্টের তালিকা টার্মিনালে (FreeType আছে কিনা সহ)
 * ওপরে লেখার ঘরে নিজের লেখা বসানো যায়; + / - মাপ বদলায়। ডেস্কটপ না থাকলে তালিকা ছাপে।
 * ধাপ ২৩খ: প্রতিটা ফন্টের নিচে একটা বাংলা লাইন (যুক্তাক্ষর জোড়া লাগে কিনা দেখতে)। */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <rozgfx.h>
#include <rozui.h>

#define FONT_DIR "/usr/share/fonts"
#define MAXF     64

static char *files[MAXF];
static char names[MAXF][64];
static const char *ptrs[MAXF];
static int nf;

static int cmp_str(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }

static void scan(void)
{
    DIR *d = opendir(FONT_DIR);
    struct dirent *e;
    int i;
    if (!d) return;
    while ((e = readdir(d)) != NULL && nf < MAXF) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || (strcasecmp(dot, ".ttf") && strcasecmp(dot, ".otf"))) continue;
        files[nf] = (char *)malloc(strlen(FONT_DIR) + strlen(e->d_name) + 2);
        if (!files[nf]) break;
        sprintf(files[nf], "%s/%s", FONT_DIR, e->d_name);
        nf++;
    }
    closedir(d);
    qsort(files, (size_t)nf, sizeof(files[0]), cmp_str);
    for (i = 0; i < nf; i++) {
        char *dot;
        snprintf(names[i], sizeof(names[i]), "%s", strrchr(files[i], '/') + 1);
        if ((dot = strrchr(names[i], '.')) != NULL) *dot = '\0';
        ptrs[i] = names[i];
    }
}

static void list_text(void)
{
    int i;
    printf("TrueType (FreeType): %s\n", gfx_have_freetype() ? "yes" : "no - text uses the bitmap font");
    for (i = 0; i < nf; i++) {
        struct stat st;
        long kb = stat(files[i], &st) == 0 ? (long)(st.st_size / 1024) : 0;
        printf("  %-28s %5ld KiB\n", names[i], kb);
    }
    printf("%d font(s) in %s - see `help fonts`\n", nf, FONT_DIR);
}

int main(int argc, char **argv)
{
    static const int sizes[] = { 11, 13, 14, 16, 20, 28, 40 };
    char sample[128] = "The quick brown fox jumps over the lazy dog";
    gfx_font *f[7] = { 0 };
    int sel = 0, top = 0, loaded = -1, base = 0, i;
    ui_ctx U;

    if (argc > 1 && !strcmp(argv[1], "-l")) {
        scan();
        list_text();
        return 0;
    }
    if (argc > 1) {
        files[0] = argv[1];
        snprintf(names[0], sizeof(names[0]), "%s", strrchr(argv[1], '/') ? strrchr(argv[1], '/') + 1 : argv[1]);
        ptrs[0] = names[0];
        nf = 1;
    } else {
        scan();
    }
    if (gfx_init_window("Fonts", 900, 600) < 0) {
        list_text();
        return 0;
    }
    ui_init(&U);
    while (ui_run(&U)) {
        const struct ui_theme *T = ui_theme_get();
        int W = U.w, H = U.h, lx = 10, lw = nf > 1 ? 220 : 0, px = lx + lw + (lw ? 12 : 0), y;
        gfx_clear(U.s, T->window_bg);
        if (lw) ui_do_list(&U, lx, 10, lw, H - 20, ptrs, nf, &sel, &top);
        ui_label(U.s, px, 16, "Text:", T->text_dim);
        ui_do_textbox(&U, px + 48, 10, W - px - 48 - 90, sample, (int)sizeof(sample));
        if (ui_do_button(&U, W - 82, 10, 34, 26, "-") && base > -6) { base -= 2; loaded = -1; }
        if (ui_do_button(&U, W - 44, 10, 34, 26, "+") && base < 20) { base += 2; loaded = -1; }
        if (U.key == '+' || U.key == '=') { if (base < 20) base += 2; loaded = -1; U.key = 0; }
        if (U.key == '-') { if (base > -6) base -= 2; loaded = -1; U.key = 0; }
        if (sel != loaded && sel >= 0 && sel < nf) {
            for (i = 0; i < 7; i++) {
                if (f[i]) gfx_font_free(f[i]);
                f[i] = gfx_font_load_ttf(files[sel], sizes[i] + base, 0);
            }
            loaded = sel;
        }
        ui_sunken(U.s, px, 46, W - px - 10, H - 56, T->field);
        gfx_set_clip(U.s, px + 2, 48, W - px - 14, H - 60);
        y = 56;
        if (!f[0]) {
            ui_label(U.s, px + 12, y, gfx_have_freetype() ? "This font cannot be opened."
                                                         : "No TrueType support (FreeType) in this build.", T->text_dim);
        } else {
            char t[32];
            gfx_text(U.s, f[3], px + 12, y, sel >= 0 && sel < nf ? names[sel] : "", GFX_WHITE);
            y += gfx_font_height(f[3]) + 8;
            gfx_text(U.s, f[2], px + 12, y, "ABCDEFGHIJKLMNOPQRSTUVWXYZ  abcdefghijklmnopqrstuvwxyz", T->field_text);
            y += gfx_font_height(f[2]);
            gfx_text(U.s, f[2], px + 12, y, "0123456789  !@#$%^&*()[]{}<>?/\\|+-=_~;:'\",.", T->field_text);
            y += gfx_font_height(f[2]);
            /* ধাপ ২৩খ: বাংলা — যুক্তাক্ষর, রেফ, ি-কার (ফন্টে বাংলা না থাকলে Noto Sans Bengali থেকে) */
            gfx_text(U.s, f[2], px + 12, y + 2, "বাংলা: আমি, কি, ক্ষমা, শিক্ষা, স্ত্রী, রাষ্ট্র, জ্ঞান, সন্ধ্যা, কার্তিক, পৃথিবী",
                     T->field_text);
            y += gfx_font_height(f[2]) + 14;
            for (i = 0; i < 7 && y < H - 20; i++) {
                snprintf(t, sizeof(t), "%d", sizes[i] + base);
                gfx_text(U.s, NULL, px + 12, y + (gfx_font_height(f[i]) - 16) / 2, t, T->text_dim);
                gfx_text(U.s, f[i], px + 44, y, sample, T->field_text);
                y += gfx_font_height(f[i]) + 6;
            }
        }
        gfx_reset_clip(U.s);
        if (U.key == 'q' || U.key == GFX_KEY_ESC) ui_quit(&U);
    }
    gfx_quit();
    return 0;
}
