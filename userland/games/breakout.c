/* rozOS — Breakout (ধাপ ১৮ক): librozgame-এর উদাহরণ গেম
 *
 * খেলা: প্যাডেল নাড়াও (মাউস, বা ← → / A D), Space বা ক্লিকে বল ছাড়ো, সব ইট ভাঙো।
 * বেগুনি ইট দুবার মারতে হয়। P = থামাও, Esc = মেনু। ৩টা জীবন, প্রতি লেভেলে বল দ্রুত।
 *
 * ছবি: /usr/share/games/breakout-*.png|jpg — পটভূমি 1280x960-এর "ফটো", শুরুতে একবার
 * মসৃণভাবে পর্দার মাপে ছোট করা হয় (game_image_resize), তারপর প্রতি ফ্রেমে শুধু কপি।
 *
 * নিজে বদলে চালাতে:  cd /usr/src/games; tcc breakout.c -lrozgame -lrozgfx -lm -o /tmp/bo; /tmp/bo */
#include <stdio.h>
#include <string.h>
#include <rozgame.h>

#define W 640
#define H 480
#define BCOLS 9
#define BROWS 6
#define BW 64
#define BH 24
#define BX0 ((W - BCOLS * BW) / 2)
#define BY0 70
#define PAD_Y 440
#define BALL_R 8
#define MAXP 160

enum { S_MENU, S_SERVE, S_PLAY, S_PAUSE, S_LOST, S_WON };

typedef struct { float x, y, vx, vy, life, max; uint32_t color; int size; } particle;

static struct {
    int state, level, lives;
    long score, best;
    int new_best;
    int bricks[BROWS][BCOLS];           /* 0 নেই, 1 আছে, 2 দুবার লাগবে */
    int left;
    float pad_x, pad_w, pad_v;
    float bx, by, bvx, bvy, speed;
    float flash, shake, msg_t;
    int last_mouse_x;
    particle p[MAXP];
    game_image *bg, *paddle, *ball;
    game_sheet *brick;
    game_menu menu;
} G;

static const char *menu_items[] = { "Play", "Quit" };
static int autoplay;                    /* -p: মেনু ছাড়া সোজা খেলা (পরীক্ষার জন্য) */
static const uint32_t row_colors[BROWS] = {
    GFX_RGB(220, 60, 60), GFX_RGB(235, 140, 40), GFX_RGB(230, 210, 50),
    GFX_RGB(70, 190, 80), GFX_RGB(60, 140, 230), GFX_RGB(160, 80, 220)
};

static void build_level(int level)
{
    int r, c;
    G.left = 0;
    for (r = 0; r < BROWS; r++)
        for (c = 0; c < BCOLS; c++) {
            int on = 1;
            switch ((level - 1) % 3) {
            case 1: on = (r + c) % 2 == 0 || r == 0; break;                     /* দাবার ছক */
            case 2: on = c >= r / 2 && c < BCOLS - r / 2; break;                 /* পিরামিড উল্টো */
            default: break;
            }
            G.bricks[r][c] = on ? (r == BROWS - 1 || (level > 1 && r == 0) ? 2 : 1) : 0;
            if (on) G.left++;
        }
}

static void serve(void)
{
    G.state = S_SERVE;
    G.bx = G.pad_x;
    G.by = PAD_Y - BALL_R - 1;
    G.bvx = G.bvy = 0;
}

static void start_level(int level)
{
    G.level = level;
    G.speed = 300 + (level - 1) * 40.0f;
    if (G.speed > 560) G.speed = 560;
    G.pad_w = level >= 3 ? 88 : 104;
    build_level(level);
    serve();
    G.msg_t = 1.5f;
}

static void new_game(void)
{
    G.score = 0;
    G.lives = 3;
    G.new_best = 0;
    G.pad_x = W / 2;
    start_level(1);
}

static void emit(float x, float y, uint32_t color, int n, float speed)
{
    int i;
    for (i = 0; i < MAXP && n > 0; i++) {
        particle *q = &G.p[i];
        float a, s;
        if (q->life > 0) continue;
        a = game_rand_float(0, 6.2831853f);
        s = game_rand_float(speed * 0.3f, speed);
        q->x = x; q->y = y;
        q->vx = cosf(a) * s; q->vy = sinf(a) * s - 60;
        q->max = q->life = game_rand_float(0.4f, 0.9f);
        q->color = color;
        q->size = game_rand_int(2, 5);
        n--;
    }
}

static void launch(void)
{
    float a = game_rand_float(-0.5f, 0.5f);
    G.bvx = sinf(a) * G.speed;
    G.bvy = -cosf(a) * G.speed;
    G.state = S_PLAY;
}

/* বলকে ছোট ছোট ধাপে সরানো (দ্রুত বল ইট ভেদ করে চলে যায় না) */
static void move_ball(float dt)
{
    int steps = (int)(sqrtf(G.bvx * G.bvx + G.bvy * G.bvy) * dt / 4) + 1, s, r, c;
    float sdt = dt / steps;
    for (s = 0; s < steps && G.state == S_PLAY; s++) {
        game_rect ball;
        G.bx += G.bvx * sdt;
        G.by += G.bvy * sdt;
        /* দেয়াল */
        if (G.bx < BALL_R) { G.bx = BALL_R; G.bvx = fabsf(G.bvx); }
        if (G.bx > W - BALL_R) { G.bx = W - BALL_R; G.bvx = -fabsf(G.bvx); }
        if (G.by < BALL_R + 32) { G.by = BALL_R + 32; G.bvy = fabsf(G.bvy); }
        ball = game_make_rect(G.bx - BALL_R, G.by - BALL_R, BALL_R * 2, BALL_R * 2);
        /* প্যাডেল: কোথায় লাগল তার ওপর কোণ */
        if (G.bvy > 0 && game_rect_overlap(ball, game_make_rect(G.pad_x - G.pad_w / 2, PAD_Y, G.pad_w, 16))) {
            float off = (G.bx - G.pad_x) / (G.pad_w / 2), a;
            off = game_clamp(off + G.pad_v * 0.0008f, -1, 1);
            a = off * 1.05f;                         /* ±60° */
            G.bvx = sinf(a) * G.speed;
            G.bvy = -cosf(a) * G.speed;
            G.by = PAD_Y - BALL_R;
            emit(G.bx, PAD_Y, GFX_RGB(200, 220, 255), 6, 120);
        }
        /* ইট */
        for (r = 0; r < BROWS; r++)
            for (c = 0; c < BCOLS; c++) {
                game_rect b;
                int side;
                if (!G.bricks[r][c]) continue;
                b = game_make_rect(BX0 + c * BW, BY0 + r * BH, BW, BH);
                side = game_rect_hit_side(ball, b);
                if (side == GAME_SIDE_NONE) continue;
                if (side == GAME_SIDE_LEFT || side == GAME_SIDE_RIGHT) G.bvx = -G.bvx;
                else G.bvy = -G.bvy;
                game_rect_push_out(&ball, b);
                G.bx = ball.x + BALL_R;
                G.by = ball.y + BALL_R;
                if (--G.bricks[r][c] == 0) {
                    G.left--;
                    G.score += 10 * G.level;
                    emit(b.x + BW / 2, b.y + BH / 2, row_colors[r], 22, 220);
                    G.shake = 0.12f;
                } else {
                    G.score += 5;
                    emit(G.bx, G.by, GFX_WHITE, 6, 100);
                }
                G.speed += 3;
                goto next_step;                      /* এক ধাপে একটা ইটই */
            }
    next_step:
        if (G.left <= 0) {
            G.state = S_WON;
            G.msg_t = 0;
            G.flash = 0.4f;
            return;
        }
        if (G.by > H + BALL_R) {
            G.lives--;
            G.flash = 0.3f;
            G.shake = 0.3f;
            if (G.lives <= 0) {
                G.new_best = game_score_save("breakout", G.score);
                G.best = game_score_load("breakout");
                G.state = S_LOST;
            } else {
                serve();
            }
            return;
        }
    }
}

static void update_paddle(float dt)
{
    float old = G.pad_x, target;
    int mx = game_mouse_x();
    if (game_key_down(GFX_KEY_LEFT) || game_key_down('a')) G.pad_x -= 520 * dt;
    if (game_key_down(GFX_KEY_RIGHT) || game_key_down('d')) G.pad_x += 520 * dt;
    if (mx != G.last_mouse_x && game_mouse_inside()) {     /* মাউস নড়লে মাউসই চালায় */
        target = (float)mx;
        G.pad_x = target;
    }
    G.last_mouse_x = mx;
    G.pad_x = game_clamp(G.pad_x, G.pad_w / 2, W - G.pad_w / 2);
    G.pad_v = dt > 0 ? (G.pad_x - old) / dt : 0;
}

static void update(void *ud, float dt)
{
    int i;
    (void)ud;
    for (i = 0; i < MAXP; i++) {
        particle *q = &G.p[i];
        if (q->life <= 0) continue;
        q->life -= dt;
        q->vy += 500 * dt;
        q->x += q->vx * dt;
        q->y += q->vy * dt;
    }
    if (G.flash > 0) G.flash -= dt;
    if (G.shake > 0) G.shake -= dt;
    if (G.msg_t > 0) G.msg_t -= dt;

    switch (G.state) {
    case S_MENU: {
        int c = game_menu_update(&G.menu);
        if (c == 0) new_game();
        else if (c == 1 || game_key_pressed(GFX_KEY_ESC)) game_quit();
        break;
    }
    case S_SERVE:
        update_paddle(dt);
        G.bx = G.pad_x;
        if (game_key_pressed(' ') || game_key_pressed(GFX_KEY_UP) || game_mouse_pressed(GFX_BUTTON_LEFT)) launch();
        if (game_key_pressed(GFX_KEY_ESC)) G.state = S_MENU;
        break;
    case S_PLAY:
        update_paddle(dt);
        move_ball(dt);
        if (game_key_pressed('p') || !game_focused()) G.state = S_PAUSE;
        if (game_key_pressed(GFX_KEY_ESC)) G.state = S_MENU;
        break;
    case S_PAUSE:
        if (game_key_pressed('p') || game_key_pressed(' ') || game_key_pressed(GFX_KEY_ENTER)) G.state = S_PLAY;
        if (game_key_pressed(GFX_KEY_ESC)) G.state = S_MENU;
        break;
    case S_WON:
        G.msg_t += dt;
        if (G.msg_t > 1.2f && (game_key_pressed(' ') || game_key_pressed(GFX_KEY_ENTER) || game_mouse_pressed(GFX_BUTTON_LEFT)))
            start_level(G.level + 1);
        break;
    case S_LOST:
        if (game_key_pressed(GFX_KEY_ENTER) || game_key_pressed(' ') || game_mouse_pressed(GFX_BUTTON_LEFT)) new_game();
        if (game_key_pressed(GFX_KEY_ESC)) G.state = S_MENU;
        break;
    }
}

static void draw(void *ud, gfx_surface *scr)
{
    int r, c, i, sx = 0, sy = 0;
    char b[64];
    (void)ud; (void)scr;
    if (G.shake > 0) {
        sx = game_rand_int(-3, 3);
        sy = game_rand_int(-3, 3);
    }
    if (G.bg) game_draw_image(G.bg, 0, 0);
    else game_gradient(0, 0, W, H, GFX_RGB(20, 20, 60), GFX_RGB(5, 5, 15));

    if (G.state != S_MENU) {
        for (r = 0; r < BROWS; r++)
            for (c = 0; c < BCOLS; c++) {
                int x = BX0 + c * BW + sx, y = BY0 + r * BH + sy;
                if (!G.bricks[r][c]) continue;
                if (G.brick) game_draw_frame(G.brick, r, x, y, 0);
                else game_fill_rect(x + 1, y + 1, BW - 2, BH - 2, row_colors[r]);
                if (G.bricks[r][c] == 2) {                   /* শক্ত ইট: ঝিলিক */
                    game_draw_rect(x + 2, y + 2, BW - 4, BH - 4, GFX_RGBA(255, 255, 255, 255));
                    game_draw_rect(x + 3, y + 3, BW - 6, BH - 6, GFX_RGBA(255, 255, 255, 255));
                }
            }
        if (G.paddle) game_draw_image_scaled(G.paddle, 0, 0, G.paddle->w, G.paddle->h,
                                             (int)(G.pad_x - G.pad_w / 2) + sx, PAD_Y + sy, (int)G.pad_w, 18, 0);
        else game_fill_rect((int)(G.pad_x - G.pad_w / 2), PAD_Y, (int)G.pad_w, 16, GFX_WHITE);
        if (G.state != S_LOST) {
            if (G.ball) game_draw_image(G.ball, (int)G.bx - BALL_R + sx, (int)G.by - BALL_R + sy);
            else game_fill_circle((int)G.bx, (int)G.by, BALL_R, GFX_WHITE);
        }
    }
    for (i = 0; i < MAXP; i++) {
        particle *q = &G.p[i];
        uint32_t a;
        if (q->life <= 0) continue;
        a = (uint32_t)(255 * q->life / q->max);
        game_fill_rect((int)q->x, (int)q->y, q->size, q->size, (q->color & 0xFFFFFF) | (a << 24));
    }
    if (G.flash > 0) game_fill_rect(0, 0, W, H, GFX_RGBA(255, 255, 255, (int)(G.flash * 300) > 120 ? 120 : (int)(G.flash * 300)));

    /* ওপরের পট্টি */
    game_fill_rect(0, 0, W, 32, GFX_RGBA(0, 0, 0, 150));
    snprintf(b, sizeof(b), "Score %ld", G.score);
    game_ttf(NULL, 20, 12, 5, b, GFX_WHITE);
    snprintf(b, sizeof(b), "Level %d", G.level);
    game_ttf_center(NULL, 20, 5, b, GFX_RGB(180, 210, 255));
    for (i = 0; i < G.lives && G.state != S_MENU; i++)
        if (G.ball) game_draw_image(G.ball, W - 26 - i * 22, 8);

    switch (G.state) {
    case S_MENU:
        game_fill_rect(0, 0, W, H, GFX_RGBA(0, 0, 0, 110));
        game_ttf_center(NULL, 64, 110, "BREAKOUT", GFX_RGB(255, 200, 90));
        snprintf(b, sizeof(b), "Best %ld", G.best);
        game_ttf_center(NULL, 20, 190, b, GFX_RGB(255, 220, 120));
        game_menu_draw(&G.menu, 240);
        game_ttf_center(NULL, 16, 420, "mouse or arrows / A D, Space to launch, P pause, Esc menu", GFX_RGB(210, 210, 210));
        break;
    case S_SERVE:
        if (G.msg_t > 0) {
            snprintf(b, sizeof(b), "Level %d", G.level);
            game_ttf_center(NULL, 40, 260, b, GFX_WHITE);
        }
        game_ttf_center(NULL, 18, 320, "Space or click to launch", GFX_RGB(220, 220, 220));
        break;
    case S_PAUSE:
        game_fill_rect(0, 0, W, H, GFX_RGBA(0, 0, 0, 140));
        game_ttf_center(NULL, 48, 200, "Paused", GFX_WHITE);
        break;
    case S_WON:
        game_ttf_center(NULL, 48, 230, "Level clear!", GFX_RGB(140, 255, 140));
        if (G.msg_t > 1.2f) game_ttf_center(NULL, 18, 300, "Space for the next level", GFX_RGB(220, 220, 220));
        break;
    case S_LOST:
        game_fill_rect(0, 0, W, H, GFX_RGBA(0, 0, 0, 150));
        game_ttf_center(NULL, 52, 160, "Game over", GFX_RGB(255, 110, 90));
        snprintf(b, sizeof(b), "Score %ld%s", G.score, G.new_best ? "  - new best!" : "");
        game_ttf_center(NULL, 26, 240, b, G.new_best ? GFX_RGB(255, 220, 120) : GFX_WHITE);
        game_ttf_center(NULL, 18, 300, "Enter to play again, Esc for menu", GFX_RGB(220, 220, 220));
        break;
    default:
        break;
    }
}

static int init(void *ud)
{
    game_image *big;
    (void)ud;
    big = game_image_load(game_data_path("breakout-bg.jpg"));
    if (big) {
        /* বড় ফটো → পর্দার মাপ, একবারই, মসৃণভাবে */
        G.bg = (big->w == game_width() && big->h == game_height()) ? big
             : game_image_resize(big, game_width(), game_height(), 1);
        if (G.bg != big) game_image_free(big);
    }
    G.paddle = game_image_load(game_data_path("breakout-paddle.png"));
    G.ball = game_image_load(game_data_path("breakout-ball.png"));
    G.brick = game_sheet_load(game_data_path("breakout-bricks.png"), BW, BH);
    G.best = game_score_load("breakout");
    G.menu.items = menu_items;
    G.menu.count = 2;
    G.menu.size = 30;
    G.menu.y = 240;
    G.pad_x = W / 2;
    G.pad_w = 104;
    G.lives = 3;
    G.level = 1;
    build_level(1);
    G.state = S_MENU;
    if (autoplay) {
        new_game();
        launch();
    }
    return 0;
}

static void quit(void *ud)
{
    (void)ud;
    game_image_free(G.bg);
    game_image_free(G.paddle);
    game_image_free(G.ball);
    game_sheet_free(G.brick);
}

int main(int argc, char **argv)
{
    game_config c = game_default_config("Breakout");
    game_app a;
    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f")) c.show_fps = 1;
        else if (!strcmp(argv[i], "-p")) autoplay = 1;
        else if (!strcmp(argv[i], "-s")) c.smooth = 1;
        else {
            fprintf(stderr, "usage: breakout [-f] [-p] [-s]   (-f fps, -p start playing, -s smooth scaling)\n");
            return 2;
        }
    }
    memset(&a, 0, sizeof(a));
    a.init = init;
    a.update = update;
    a.draw = draw;
    a.quit = quit;
    return game_run(&c, &a);
}
