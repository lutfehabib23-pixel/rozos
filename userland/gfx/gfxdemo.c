/* rozOS — gfxdemo: librozgfx-এর সব কিছু একসাথে (ধাপ ১৩)
 *   gfxdemo        আকৃতি, রঙের ঢাল, PSF আর TrueType লেখা, ছবি, চলন্ত বল, মাউস-তীর
 *   q / Esc        বের হওয়া (মাউসের বাঁ-ক্লিক: বলটা সেখানে)
 * কোডটা /usr/src/gfx/gfxdemo.c-তে — নিজে বদলে tcc gfxdemo.c -lrozgfx দিয়ে চালাও। */

#include <stdio.h>
#include <rozgfx.h>

int main(void)
{
    gfx_surface *s, *img;
    gfx_ttf *ttf;
    gfx_event ev;
    int W, H, bx, by, vx = 5, vy = 4, r = 22, running = 1, frames = 0;
    long t0, frame_at;
    char buf[96];

    if (gfx_init_window("Graphics demo", 1000, 680) < 0) {
        perror("gfxdemo: /dev/fb0");
        return 1;
    }
    s = gfx_screen();
    W = gfx_width();
    H = gfx_height();
    ttf = gfx_ttf_load("/usr/share/fonts/DejaVuSans.ttf");
    img = gfx_load_image("/usr/share/images/rozos-sunset.bmp");
    bx = W / 2;
    by = H / 2;
    t0 = gfx_ticks_ms();
    frame_at = t0;

    while (running) {
        int mx, my;
        /* পটভূমি */
        gfx_gradient_v(s, 0, 0, W, H, GFX_RGB(18, 24, 48), GFX_RGB(70, 40, 90));
        /* শিরোনাম */
        if (ttf) {
            gfx_ttf_text(s, ttf, 44, 30, 20, "rozOS graphics", GFX_WHITE);
            gfx_ttf_text(s, ttf, 20, 32, 72, "librozgfx: shapes, images, fonts, mouse - press q to quit",
                         GFX_RGBA(255, 255, 255, 200));
        } else {
            gfx_text(s, NULL, 30, 30, "rozOS graphics (DejaVuSans.ttf not found)", GFX_WHITE);
        }
        /* আকৃতি */
        gfx_fill_round_rect(s, 30, 120, 220, 140, 18, GFX_RGB(40, 120, 200));
        gfx_round_rect(s, 30, 120, 220, 140, 18, GFX_WHITE);
        gfx_text(s, NULL, 50, 140, "round rect", GFX_WHITE);
        gfx_fill_circle(s, 360, 190, 70, GFX_RGB(240, 180, 40));
        gfx_circle(s, 360, 190, 76, GFX_WHITE);
        gfx_fill_triangle(s, 470, 260, 560, 120, 650, 260, GFX_RGB(60, 190, 110));
        gfx_ellipse(s, 760, 190, 90, 50, GFX_RGB(255, 120, 160));
        gfx_thick_line(s, 30, 300, 860, 300, 4, GFX_RGBA(255, 255, 255, 255));
        gfx_fill_rect_alpha(s, 120, 160, 300, 80, GFX_RGBA(255, 0, 0, 90));   /* আধা-স্বচ্ছ */
        /* ছবি */
        if (img) {
            gfx_blit(s, 30, 330, img, 0, 0, -1, -1);
            gfx_text(s, NULL, 30, 330 + img->h + 6, "/usr/share/images/rozos-sunset.bmp", GFX_WHITE);
            gfx_blit_scaled(s, 60 + img->w, 330, img->w / 2, img->h / 2, img);
            gfx_text(s, NULL, 60 + img->w, 336 + img->h / 2, "scaled 50%", GFX_WHITE);
        }
        /* PSF লেখা */
        gfx_text_bg(s, NULL, W - 330, 330, " bitmap font: rozos-mono-16 ", GFX_BLACK, GFX_YELLOW);
        snprintf(buf, sizeof(buf), "%dx%d, frame %d, %ld ms", W, H, frames, gfx_ticks_ms() - t0);
        gfx_text(s, NULL, W - 330, 356, buf, GFX_WHITE);
        /* চলন্ত বল */
        bx += vx; by += vy;
        if (bx < r || bx > W - r) { vx = -vx; bx += vx; }
        if (by < 320 + r || by > H - r) { vy = -vy; by += vy; }
        gfx_fill_circle(s, bx, by, r, GFX_RGB(255, 80, 60));
        gfx_fill_circle(s, bx - 7, by - 7, 6, GFX_RGBA(255, 255, 255, 255));
        /* মাউস */
        gfx_mouse_pos(&mx, &my);
        gfx_cursor_draw(s, mx, my);
        gfx_present();
        frames++;

        /* ধাপ ১৪খ: প্রতি ফ্রেম ~16 ms — মাউস নড়লেও; জমা ঘটনা সব একসাথে নিয়ে পরের ফ্রেমে যাওয়া
         * (আগে প্রতিটা মাউস-ঘটনায় আবার ১৬ ms অপেক্ষা হত, তাই নড়াচড়ায় বল আটকে যেত) */
        {
            long due = frame_at + 16, now;
            for (;;) {
                now = gfx_ticks_ms();
                if (now > due + 40) break;                 /* ঘটনার বন্যাতেও ছবি চলবে */
                if (!gfx_poll_event(&ev, now >= due ? 0 : (int)(due - now))) {
                    if (gfx_ticks_ms() >= due) break;
                    continue;
                }
                if (ev.type == GFX_EV_KEY && (ev.key == 'q' || ev.key == GFX_KEY_ESC)) running = 0;
                if (ev.type == GFX_EV_CLOSE) running = 0;          /* ধাপ ১৪: উইন্ডোর [X] */
                if (ev.type == GFX_EV_RESIZE) { W = gfx_width(); H = gfx_height(); }
                if (ev.type == GFX_EV_MOUSE_DOWN && ev.button == GFX_BUTTON_LEFT && ev.y > 320) { bx = ev.x; by = ev.y; }
            }
            frame_at = gfx_ticks_ms();
        }
    }
    gfx_quit();
    printf("gfxdemo: %d frames\n", frames);
    return 0;
}
