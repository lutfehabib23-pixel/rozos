/* rozOS — imgview: ছবি দেখা (ধাপ ১৩)
 *   imgview FILE...        .bmp বা .ppm (না দিলে /usr/share/images-এর ছবি)
 *   ← →  / Space           আগের / পরের ছবি
 *   + -  0                 বড় / ছোট / পর্দায় মাপমতো
 *   মাউসের চাকা           বড়-ছোট;  q / Esc  বের হওয়া
 *
 * ধাপ ১৯ঙ: থাম্বনেইল (ছোট ছবির গ্রিড) + rozui
 *   imgview                /usr/share/images-এর সব ছবি থাম্বনেইলে (ডেস্কটপের Images আইকন)
 *   imgview FOLDER         সেই ফোল্ডারের থাম্বনেইল
 *   imgview FILE           ছবিটা বড় করে; Back/Esc → তার ফোল্ডারের থাম্বনেইল, ← → সেই ফোল্ডারে
 *   imgview A B C ...      শুধু এই ছবিগুলো (আগের মতো)
 *   ছবি: jpg jpeg png bmp gif tga ppm pgm pnm (stb_image)
 *   থাম্বনেইলে: ক্লিক বা Enter = বড় করে দেখা, তীর = বাছাই সরানো, চাকা/PgUp/PgDn = স্ক্রল,
 *               "Folder..." = অন্য ফোল্ডার, Esc = বন্ধ
 *   বড় ছবিতে: Back বা Esc/Backspace = থাম্বনেইলে ফেরা; q সবসময় বন্ধ
 *   থাম্বনেইল একটা একটা করে লোড হয় (উইন্ডো সাথে সাথে আসে)। */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <rozgfx.h>
#include <rozui.h>

#define MAX_FILES 512
#define CELL_W    150
#define CELL_H    136
#define THUMB_W   128
#define THUMB_H   100
#define TOP_H     34
#define INFO_H    28

static char *files[MAX_FILES];
static gfx_surface *thumbs[MAX_FILES];
static unsigned char thumb_state[MAX_FILES];     /* 0 লোড হয়নি, 1 আছে, 2 পড়া যায়নি */
static int nfiles;
static char folder[512] = "/usr/share/images";

static int is_image(const char *name)
{
    static const char *const ext[] = { ".jpg", ".jpeg", ".png", ".bmp", ".gif", ".tga", ".ppm", ".pgm", ".pnm" };
    const char *dot = strrchr(name, '.');
    size_t i;
    if (!dot) return 0;
    for (i = 0; i < sizeof(ext) / sizeof(ext[0]); i++)
        if (!strcasecmp(dot, ext[i])) return 1;
    return 0;
}

static int cmp_name(const void *a, const void *b)
{
    return strcasecmp(*(char *const *)a, *(char *const *)b);
}

static void clear_files(void)
{
    int i;
    for (i = 0; i < nfiles; i++) {
        free(files[i]);
        gfx_surface_free(thumbs[i]);
        thumbs[i] = NULL;
        thumb_state[i] = 0;
    }
    nfiles = 0;
}

static void add_file(const char *p)
{
    if (nfiles >= MAX_FILES) return;
    files[nfiles] = strdup(p);
    if (files[nfiles]) nfiles++;
}

static void add_dir(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int first = nfiles;
    if (!d) return;
    while ((e = readdir(d)) && nfiles < MAX_FILES) {
        if (is_image(e->d_name)) {
            char *p = (char *)malloc(strlen(dir) + strlen(e->d_name) + 2);
            if (!p) break;
            sprintf(p, "%s%s%s", dir, dir[strlen(dir) - 1] == '/' ? "" : "/", e->d_name);
            files[nfiles++] = p;
        }
    }
    closedir(d);
    qsort(files + first, (size_t)(nfiles - first), sizeof(files[0]), cmp_name);
}

static void open_folder(const char *dir)
{
    clear_files();
    snprintf(folder, sizeof(folder), "%s", dir);
    add_dir(folder);
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* বড় ছবি থেকে থাম্বনেইল: অনুপাত ঠিক রেখে, ক্ষেত্রফলের গড় নিয়ে ছোট করা (ঝকঝকে) */
static gfx_surface *make_thumb(const gfx_surface *img)
{
    int tw = THUMB_W, th = THUMB_H, x, y;
    gfx_surface *t;
    if (img->w * th > img->h * tw) th = img->h * tw / img->w; else tw = img->w * th / img->h;
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    if (img->w <= tw && img->h <= th) { tw = img->w; th = img->h; }
    t = gfx_surface_new(tw, th);
    if (!t) return NULL;
    for (y = 0; y < th; y++) {
        int y0 = y * img->h / th, y1 = (y + 1) * img->h / th;
        if (y1 <= y0) y1 = y0 + 1;
        for (x = 0; x < tw; x++) {
            int x0 = x * img->w / tw, x1 = (x + 1) * img->w / tw, sx, sy, n = 0;
            unsigned long r = 0, g = 0, b = 0;
            if (x1 <= x0) x1 = x0 + 1;
            for (sy = y0; sy < y1; sy += (y1 - y0 > 4 ? (y1 - y0) / 4 : 1))
                for (sx = x0; sx < x1; sx += (x1 - x0 > 4 ? (x1 - x0) / 4 : 1)) {
                    uint32_t c = img->px[(size_t)sy * img->stride + sx];
                    r += GFX_R(c); g += GFX_G(c); b += GFX_B(c);
                    n++;
                }
            t->px[(size_t)y * t->stride + x] = GFX_RGB(r / n, g / n, b / n);
        }
    }
    return t;
}

/* লোড না হওয়া প্রথম থাম্বনেইল লোড করা; আর বাকি থাকলে 1 */
static int load_next_thumb(void)
{
    int i;
    for (i = 0; i < nfiles; i++) {
        if (thumb_state[i] == 0) {
            gfx_surface *img = gfx_load_image(files[i]);
            if (img) {
                thumbs[i] = make_thumb(img);
                gfx_surface_free(img);
            }
            thumb_state[i] = thumbs[i] ? 1 : 2;
            return 1;
        }
    }
    return 0;
}

/* নামটা ঘরের চওড়ায় কাটা */
static void short_name(char *out, size_t size, const char *name, int max_chars)
{
    size_t n = strlen(name);
    if ((int)n <= max_chars) { snprintf(out, size, "%s", name); return; }
    snprintf(out, size, "%.*s..", max_chars - 2, name);
}

int main(int argc, char **argv)
{
    ui_ctx ui;
    gfx_surface *img = NULL;
    int i, cur = 0, loaded = -1, zoom = 0;     /* zoom 0 = মাপমতো, নইলে শতাংশ */
    int viewing = 0, sel = 0, scroll = 0, want_sel_visible = 0;
    struct stat st;

    if (argc == 2 && stat(argv[1], &st) == 0 && S_ISDIR(st.st_mode)) {
        open_folder(argv[1]);                              /* ফোল্ডার → থাম্বনেইল */
    } else if (argc == 2) {                                /* একটা ছবি → বড় করে, ফোল্ডারসহ */
        char dir[512], *sl;
        snprintf(dir, sizeof(dir), "%s", argv[1]);
        sl = strrchr(dir, '/');
        if (sl == dir) dir[1] = '\0';
        else if (sl) *sl = '\0';
        else strcpy(dir, ".");
        open_folder(dir);
        for (i = 0; i < nfiles; i++)
            if (!strcmp(base_name(files[i]), base_name(argv[1]))) break;
        if (i == nfiles) {                                 /* ফোল্ডারে না পেলে (নাম অন্যরকম) */
            clear_files();
            add_file(argv[1]);
            i = 0;
        }
        cur = sel = i;
        viewing = 1;
    } else if (argc > 2) {                                 /* কয়েকটা ফাইল (আগের মতো) */
        for (i = 1; i < argc && nfiles < MAX_FILES; i++) add_file(argv[i]);
        snprintf(folder, sizeof(folder), "(%d files)", nfiles);
        viewing = 1;
    } else {
        open_folder(folder);                               /* /usr/share/images */
    }
    if (gfx_init_window("Images", 0, 0) < 0) {
        perror("imgview: /dev/fb0");
        return 1;
    }
    ui_init(&ui);

    while (ui_run(&ui)) {
        const struct ui_theme *T = ui_theme_get();
        gfx_surface *s = ui.s;
        int W = ui.w, H = ui.h, k = ui.key;
        char info[320];

        if (viewing && nfiles > 0) {
            /* ================= বড় ছবি (আগের ভিউয়ার) ================= */
            int dw, dh, pct, ah = H - TOP_H - INFO_H;
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
                    if (img->h * pct / 100 > ah - 10) pct = (ah - 10) * 100 / img->h;
                    if (pct < 1) pct = 1;
                } else {
                    pct = zoom;
                }
                dw = img->w * pct / 100;
                dh = img->h * pct / 100;
                gfx_set_clip(s, 0, TOP_H, W, ah);
                if (pct == 100) gfx_blit(s, (W - dw) / 2, TOP_H + (ah - dh) / 2, img, 0, 0, -1, -1);
                else gfx_blit_scaled(s, (W - dw) / 2, TOP_H + (ah - dh) / 2, dw, dh, img);
                gfx_reset_clip(s);
                snprintf(info, sizeof(info), " %s  %dx%d  %d%%  (%d/%d)   <- -> next   + - 0 zoom   Esc back   q quit ",
                         base_name(files[cur]), img->w, img->h, pct, cur + 1, nfiles);
            } else {
                snprintf(info, sizeof(info), " %s: cannot read (%s)   Esc back   q quit ", base_name(files[cur]),
                         gfx_image_error() ? gfx_image_error() : "unknown format");
            }
            ui_panel(s, 0, 0, W, TOP_H, T->face, 1);
            if (ui_do_button(&ui, 6, 4, 80, 26, "< Back")) viewing = 0;
            if (ui_do_button(&ui, 92, 4, 32, 26, "<")) cur = (cur + nfiles - 1) % nfiles;
            if (ui_do_button(&ui, 128, 4, 32, 26, ">")) cur = (cur + 1) % nfiles;
            ui_label(s, 172, 10, files[cur], T->text);
            ui_statusbar(s, 0, H - INFO_H, W, INFO_H, info);

            if (k == 'q') ui_quit(&ui);
            else if (k == GFX_KEY_ESC || k == GFX_KEY_BACKSPACE) viewing = 0;
            else if (k == GFX_KEY_RIGHT || k == ' ' || k == GFX_KEY_PAGE_DOWN) cur = (cur + 1) % nfiles;
            else if (k == GFX_KEY_LEFT || k == GFX_KEY_PAGE_UP) cur = (cur + nfiles - 1) % nfiles;
            else if (k == '+' || k == '=') zoom = (zoom ? zoom : 100) * 5 / 4;
            else if (k == '-') { zoom = (zoom ? zoom : 100) * 4 / 5; if (zoom < 5) zoom = 5; }
            else if (k == '0') zoom = 0;
            if (ui.wheel) {
                zoom = (zoom ? zoom : 100);
                zoom = ui.wheel < 0 ? zoom * 5 / 4 : zoom * 4 / 5;
                if (zoom < 5) zoom = 5;
            }
            if (!viewing) { sel = cur; want_sel_visible = 1; }
            continue;
        }

        /* ================= থাম্বনেইল ================= */
        {
            int cols = (W - 24) / CELL_W, rows, gx, gy = TOP_H + 6, gh = H - TOP_H - INFO_H - 6, total, pending;
            if (cols < 1) cols = 1;
            rows = (nfiles + cols - 1) / cols;
            total = rows * CELL_H;
            gx = (W - 16 - cols * CELL_W) / 2;
            if (gx < 4) gx = 4;

            /* একটা থাম্বনেইল লোড (প্রতি ফ্রেমে একটা — উইন্ডো সাড়া দিতে থাকে) */
            pending = load_next_thumb();
            if (pending) ui_redraw(&ui);

            /* কী */
            if (k == 'q' || k == GFX_KEY_ESC) ui_quit(&ui);
            else if (nfiles > 0) {
                if (k == GFX_KEY_RIGHT) sel++;
                else if (k == GFX_KEY_LEFT) sel--;
                else if (k == GFX_KEY_DOWN) sel += cols;
                else if (k == GFX_KEY_UP) sel -= cols;
                else if (k == GFX_KEY_HOME) sel = 0;
                else if (k == GFX_KEY_END) sel = nfiles - 1;
                else if (k == GFX_KEY_PAGE_DOWN) scroll += gh;
                else if (k == GFX_KEY_PAGE_UP) scroll -= gh;
                else if (k == GFX_KEY_ENTER || k == ' ') { cur = sel; viewing = 1; ui_redraw(&ui); }
                if (sel < 0) sel = 0;
                if (sel >= nfiles) sel = nfiles - 1;
                if (k == GFX_KEY_RIGHT || k == GFX_KEY_LEFT || k == GFX_KEY_DOWN || k == GFX_KEY_UP ||
                    k == GFX_KEY_HOME || k == GFX_KEY_END) want_sel_visible = 1;
            }
            if (ui.wheel) scroll += ui.wheel * 60;
            if (want_sel_visible && nfiles > 0) {
                int ry = (sel / cols) * CELL_H;
                if (ry < scroll) scroll = ry;
                if (ry + CELL_H > scroll + gh) scroll = ry + CELL_H - gh;
                want_sel_visible = 0;
            }
            if (scroll > total - gh) scroll = total - gh;
            if (scroll < 0) scroll = 0;

            gfx_clear(s, T->window_bg);
            gfx_set_clip(s, 0, gy, W - 16, gh);
            for (i = 0; i < nfiles; i++) {
                int cx = gx + (i % cols) * CELL_W, cy = gy + (i / cols) * CELL_H - scroll;
                char name[40];
                if (cy + CELL_H < gy || cy > gy + gh) continue;
                if (i == sel) gfx_fill_rect(s, cx + 2, cy + 2, CELL_W - 4, CELL_H - 4, T->select);
                else if (ui_hover(&ui, cx, cy, CELL_W, CELL_H) && ui.my >= gy && ui.my < gy + gh)
                    gfx_fill_rect(s, cx + 2, cy + 2, CELL_W - 4, CELL_H - 4, T->face_hover);
                ui_sunken(s, cx + (CELL_W - THUMB_W) / 2 - 3, cy + 6, THUMB_W + 6, THUMB_H + 6, GFX_RGB(20, 20, 24));
                if (thumb_state[i] == 1) {
                    gfx_surface *t = thumbs[i];
                    gfx_blit(s, cx + (CELL_W - t->w) / 2, cy + 9 + (THUMB_H - t->h) / 2, t, 0, 0, -1, -1);
                } else {
                    ui_label_center(s, cx, cy + 9, CELL_W, THUMB_H, thumb_state[i] == 2 ? "(cannot read)" : "loading...",
                                    T->text_dim);
                }
                short_name(name, sizeof(name), base_name(files[i]), (CELL_W - 8) / 8);
                ui_label_center(s, cx, cy + THUMB_H + 14, CELL_W, 16, name, T->text);
                if (ui.my >= gy && ui.my < gy + gh && ui_clicked(&ui, cx, cy, CELL_W, CELL_H)) {
                    sel = cur = i;
                    viewing = 1;
                    ui_redraw(&ui);
                }
            }
            if (nfiles == 0) ui_label_center(s, 0, gy, W - 16, gh, "No pictures in this folder.", T->text_dim);
            gfx_reset_clip(s);
            {
                int page = gh, maxpos = total - gh > 0 ? total - gh : 0, pos = scroll;
                if (ui_do_vscroll(&ui, W - 16, gy, 16, gh, &pos, total > gh ? total : gh, page)) scroll = pos;
                (void)maxpos;
            }

            ui_panel(s, 0, 0, W, TOP_H, T->face, 1);
            if (ui_do_button(&ui, 6, 4, 90, 26, "Folder...")) {
                char p[512];
                snprintf(p, sizeof(p), "%s/", folder);
                if (ui_filedlg("Choose a picture in the folder", p, sizeof(p), 0)) {
                    char *sl = strrchr(p, '/');
                    if (sl && sl != p) *sl = '\0';
                    else if (sl) sl[1] = '\0';
                    open_folder(p);
                    sel = scroll = 0;
                }
            }
            ui_label(s, 106, 10, folder, T->text);
            {
                int done = 0;
                for (i = 0; i < nfiles; i++) if (thumb_state[i]) done++;
                if (done < nfiles)
                    snprintf(info, sizeof(info), " %d pictures   loading %d/%d ...", nfiles, done, nfiles);
                else
                    snprintf(info, sizeof(info), " %d pictures   click or Enter: open   arrows: choose   Esc / q: quit",
                             nfiles);
            }
            ui_statusbar(s, 0, H - INFO_H, W, INFO_H, info);
        }
    }
    gfx_surface_free(img);
    clear_files();
    gfx_quit();
    return 0;
}
