/* rozOS — Snake (ধাপ ১৮ক): librozgame-এর উদাহরণ গেম
 *
 * খেলা: তীর বা W A S D দিয়ে সাপ ঘোরাও, আপেল খাও, নিজের গায়ে বা দেয়ালে লেগো না।
 * P = থামাও, Esc = মেনু। প্রতি আপেলে সাপ লম্বা আর একটু দ্রুত হয়।
 *
 * নিজে বদলে চালাতে:  cd /usr/src/games; tcc snake.c -lrozgame -lrozgfx -lm -o /tmp/snake; /tmp/snake */
#include <stdio.h>
#include <string.h>
#include <rozgame.h>

#define CELL   20
#define TOP    40                        /* ওপরের স্কোর-পট্টি */
#define COLS   (640 / CELL)
#define ROWS   ((480 - TOP) / CELL)
#define MAXLEN (COLS * ROWS)

enum { S_MENU, S_PLAY, S_PAUSE, S_OVER };

typedef struct { int x, y; } cell;
typedef struct { float x, y, vx, vy, life; uint32_t color; } spark;

static struct {
    int state;
    cell body[MAXLEN];                   /* body[0] = মাথা */
    int len, grow;
    int dx, dy;                          /* এখনকার দিক */
    int qx[2], qy[2], nq;                /* আগে থেকে চাপা দিক (দ্রুত দুবার ঘোরা) */
    cell apple;
    float step, acc, apple_t;
    long score, best;
    int new_best;
    spark sparks[64];
    game_image *field, *apple_img;
    game_menu menu;
} G;

static const char *menu_items[] = { "Play", "Quit" };
static int autoplay;                    /* -p: মেনু ছাড়া সোজা খেলা (পরীক্ষার জন্য) */

static void place_apple(void)
{
    int tries, i, ok;
    for (tries = 0; tries < 1000; tries++) {
        G.apple.x = game_rand_int(0, COLS - 1);
        G.apple.y = game_rand_int(0, ROWS - 1);
        ok = 1;
        for (i = 0; i < G.len; i++)
            if (G.body[i].x == G.apple.x && G.body[i].y == G.apple.y) { ok = 0; break; }
        if (ok) return;
    }
}

static void new_game(void)
{
    int i;
    G.len = 4;
    for (i = 0; i < G.len; i++) {
        G.body[i].x = COLS / 2 - i;
        G.body[i].y = ROWS / 2;
    }
    G.dx = 1; G.dy = 0;
    G.nq = 0;
    G.grow = 0;
    G.step = 0.12f;
    G.acc = 0;
    G.score = 0;
    G.new_best = 0;
    place_apple();
    G.state = S_PLAY;
}

static void burst(float x, float y, uint32_t color)
{
    int i, n = 0;
    for (i = 0; i < 64 && n < 18; i++) {
        spark *s = &G.sparks[i];
        float a, sp;
        if (s->life > 0) continue;
        a = game_rand_float(0, 6.2831853f);
        sp = game_rand_float(60, 220);
        s->x = x; s->y = y;
        s->vx = cosf(a) * sp; s->vy = sinf(a) * sp;
        s->life = game_rand_float(0.3f, 0.7f);
        s->color = color;
        n++;
    }
}

static void queue_dir(int dx, int dy)
{
    int lx = G.nq ? G.qx[G.nq - 1] : G.dx, ly = G.nq ? G.qy[G.nq - 1] : G.dy;
    if (G.nq >= 2 || (dx == lx && dy == ly) || (dx == -lx && dy == -ly)) return;   /* উল্টো দিকে নয় */
    G.qx[G.nq] = dx;
    G.qy[G.nq] = dy;
    G.nq++;
}

static void move_snake(void)
{
    cell head;
    int i;
    if (G.nq > 0) {
        G.dx = G.qx[0]; G.dy = G.qy[0];
        G.qx[0] = G.qx[1]; G.qy[0] = G.qy[1];
        G.nq--;
    }
    head.x = G.body[0].x + G.dx;
    head.y = G.body[0].y + G.dy;
    /* দেয়াল বা নিজের গা (লেজের শেষ ঘর বাদ — সেটা এই ধাপে সরে যাবে) */
    if (head.x < 0 || head.y < 0 || head.x >= COLS || head.y >= ROWS) goto dead;
    for (i = 0; i < G.len - (G.grow ? 0 : 1); i++)
        if (G.body[i].x == head.x && G.body[i].y == head.y) goto dead;
    if (G.grow > 0) {
        G.grow--;
        if (G.len < MAXLEN) G.len++;
    }
    memmove(&G.body[1], &G.body[0], sizeof(cell) * (size_t)(G.len - 1));
    G.body[0] = head;
    if (head.x == G.apple.x && head.y == G.apple.y) {
        G.score += 10;
        G.grow += 2;
        if (G.step > 0.055f) G.step -= 0.004f;
        burst(head.x * CELL + CELL / 2.0f, TOP + head.y * CELL + CELL / 2.0f, GFX_RGB(255, 80, 60));
        place_apple();
    }
    return;
dead:
    burst(G.body[0].x * CELL + CELL / 2.0f, TOP + G.body[0].y * CELL + CELL / 2.0f, GFX_RGB(150, 255, 120));
    G.new_best = game_score_save("snake", G.score);
    G.best = game_score_load("snake");
    G.state = S_OVER;
}

static int init(void *ud)
{
    int x, y;
    game_image *grass;
    (void)ud;
    G.apple_img = game_image_load(game_data_path("snake-apple.png"));
    grass = game_image_load(game_data_path("snake-grass.png"));
    /* মাঠ একবারই বানিয়ে রাখা — প্রতি ফ্রেমে শুধু কপি (দ্রুত) */
    G.field = game_image_new(640, 480 - TOP);
    if (G.field) {
        gfx_surface *f = G.field->surf;
        for (y = 0; y < f->h; y += 40)
            for (x = 0; x < f->w; x += 40) {
                if (grass) gfx_blit(f, x, y, grass->surf, 0, 0, 40, 40);
                else gfx_fill_rect(f, x, y, 40, 40, ((x + y) / 40) % 2 ? GFX_RGB(40, 100, 48) : GFX_RGB(36, 92, 44));
            }
        for (y = 0; y < ROWS; y++)               /* হালকা ছক */
            for (x = 0; x < COLS; x++)
                if ((x + y) % 2) gfx_fill_rect_alpha(f, x * CELL, y * CELL, CELL, CELL, GFX_RGBA(0, 0, 0, 22));
        game_image_update(G.field);
    }
    game_image_free(grass);
    G.best = game_score_load("snake");
    G.menu.items = menu_items;
    G.menu.count = 2;
    G.menu.size = 30;
    G.menu.y = 250;
    G.state = S_MENU;
    if (autoplay) new_game();
    return 0;
}

static void update(void *ud, float dt)
{
    int i;
    (void)ud;
    G.apple_t += dt;
    for (i = 0; i < 64; i++) {
        spark *s = &G.sparks[i];
        if (s->life <= 0) continue;
        s->life -= dt;
        s->x += s->vx * dt;
        s->y += s->vy * dt;
        s->vx *= 0.96f;
        s->vy *= 0.96f;
    }
    switch (G.state) {
    case S_MENU: {
        int c = game_menu_update(&G.menu);
        if (c == 0) new_game();
        else if (c == 1 || game_key_pressed(GFX_KEY_ESC)) game_quit();
        break;
    }
    case S_PLAY:
        if (game_key_pressed(GFX_KEY_UP) || game_key_pressed('w')) queue_dir(0, -1);
        if (game_key_pressed(GFX_KEY_DOWN) || game_key_pressed('s')) queue_dir(0, 1);
        if (game_key_pressed(GFX_KEY_LEFT) || game_key_pressed('a')) queue_dir(-1, 0);
        if (game_key_pressed(GFX_KEY_RIGHT) || game_key_pressed('d')) queue_dir(1, 0);
        if (game_key_pressed('p') || !game_focused()) { G.state = S_PAUSE; break; }
        if (game_key_pressed(GFX_KEY_ESC)) { G.state = S_MENU; break; }
        G.acc += dt;
        while (G.acc >= G.step && G.state == S_PLAY) {
            G.acc -= G.step;
            move_snake();
        }
        break;
    case S_PAUSE:
        if (game_key_pressed('p') || game_key_pressed(' ') || game_key_pressed(GFX_KEY_ENTER)) G.state = S_PLAY;
        if (game_key_pressed(GFX_KEY_ESC)) G.state = S_MENU;
        break;
    case S_OVER:
        if (game_key_pressed(GFX_KEY_ENTER) || game_key_pressed(' ') || game_mouse_pressed(GFX_BUTTON_LEFT)) new_game();
        if (game_key_pressed(GFX_KEY_ESC)) G.state = S_MENU;
        break;
    default:
        break;
    }
}

static void draw_body(void)
{
    int i;
    for (i = G.len - 1; i >= 0; i--) {
        int x = G.body[i].x * CELL, y = TOP + G.body[i].y * CELL;
        float t = G.len > 1 ? (float)i / (G.len - 1) : 0;
        uint32_t c = game_color_lerp(GFX_RGB(150, 235, 90), GFX_RGB(40, 140, 60), t);
        gfx_fill_round_rect(game_screen(), x + 1, y + 1, CELL - 2, CELL - 2, 6, c);
        gfx_fill_round_rect(game_screen(), x + 4, y + 3, CELL - 8, 4, 2, game_color_lerp(c, GFX_WHITE, 0.35f));
    }
    if (G.len > 0) {                          /* চোখ — যেদিকে যাচ্ছে সেদিকে */
        int hx = G.body[0].x * CELL + CELL / 2, hy = TOP + G.body[0].y * CELL + CELL / 2;
        int ex = G.dy ? 4 : 0, ey = G.dx ? 4 : 0, fx = G.dx * 4, fy = G.dy * 4;
        game_fill_circle(hx + fx - ex, hy + fy - ey, 3, GFX_WHITE);
        game_fill_circle(hx + fx + ex, hy + fy + ey, 3, GFX_WHITE);
        game_fill_circle(hx + fx - ex + G.dx, hy + fy - ey + G.dy, 1, GFX_BLACK);
        game_fill_circle(hx + fx + ex + G.dx, hy + fy + ey + G.dy, 1, GFX_BLACK);
    }
}

static void draw(void *ud, gfx_surface *scr)
{
    int i;
    char b[64];
    (void)ud; (void)scr;
    if (G.field) game_draw_image(G.field, 0, TOP);
    else game_fill_rect(0, TOP, 640, 480 - TOP, GFX_RGB(36, 92, 44));
    game_gradient(0, 0, 640, TOP, GFX_RGB(30, 34, 40), GFX_RGB(14, 16, 20));

    if (G.state != S_MENU) {
        /* আপেল একটু দোলে */
        int bob = (int)(sinf(G.apple_t * 5) * 2);
        if (G.apple_img) game_draw_image(G.apple_img, G.apple.x * CELL - 6, TOP + G.apple.y * CELL - 6 + bob);
        else game_fill_circle(G.apple.x * CELL + CELL / 2, TOP + G.apple.y * CELL + CELL / 2, 8, GFX_RED);
        draw_body();
    }
    for (i = 0; i < 64; i++) {
        spark *s = &G.sparks[i];
        if (s->life > 0) game_fill_rect((int)s->x - 2, (int)s->y - 2, 4, 4, (s->color & 0xFFFFFF) | ((uint32_t)(s->life * 360 > 255 ? 255 : s->life * 360) << 24));
    }
    snprintf(b, sizeof(b), "Score %ld", G.score);
    game_ttf(NULL, 22, 14, 8, b, GFX_WHITE);
    snprintf(b, sizeof(b), "Best %ld", G.best > G.score ? G.best : G.score);
    game_ttf(NULL, 22, 640 - 14 - game_ttf_width(NULL, 22, b), 8, b, GFX_RGB(255, 220, 120));

    if (G.state == S_MENU) {
        game_fill_rect(0, TOP, 640, 480 - TOP, GFX_RGBA(0, 0, 0, 120));
        game_ttf_center(NULL, 64, 110, "SNAKE", GFX_RGB(150, 240, 100));
        game_ttf_center(NULL, 18, 190, "arrows / WASD to steer, P pause, Esc menu", GFX_RGB(220, 220, 220));
        game_menu_draw(&G.menu, 250);
    } else if (G.state == S_PAUSE) {
        game_fill_rect(0, TOP, 640, 480 - TOP, GFX_RGBA(0, 0, 0, 140));
        game_ttf_center(NULL, 48, 200, "Paused", GFX_WHITE);
        game_ttf_center(NULL, 18, 260, "P or Enter to continue", GFX_RGB(220, 220, 220));
    } else if (G.state == S_OVER) {
        game_fill_rect(0, TOP, 640, 480 - TOP, GFX_RGBA(0, 0, 0, 150));
        game_ttf_center(NULL, 52, 150, "Game over", GFX_RGB(255, 110, 90));
        snprintf(b, sizeof(b), "Score %ld%s", G.score, G.new_best ? "  - new best!" : "");
        game_ttf_center(NULL, 26, 230, b, G.new_best ? GFX_RGB(255, 220, 120) : GFX_WHITE);
        game_ttf_center(NULL, 18, 290, "Enter to play again, Esc for menu", GFX_RGB(220, 220, 220));
    }
}

static void quit(void *ud)
{
    (void)ud;
    game_image_free(G.field);
    game_image_free(G.apple_img);
}

int main(int argc, char **argv)
{
    game_config c = game_default_config("Snake");
    game_app a;
    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f")) c.show_fps = 1;
        else if (!strcmp(argv[i], "-p")) autoplay = 1;
        else {
            fprintf(stderr, "usage: snake [-f] [-p]   (-f fps, -p start playing)\n");
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
