/* rozOS — Tetris (ধাপ ১৮খ): librozgame-এর উদাহরণ গেম
 *
 * খেলা: পড়ন্ত টুকরো সাজাও; একটা সারি ভরলে মুছে যায়। ১০ সারিতে লেভেল বাড়ে, পড়া দ্রুত হয়।
 *   ← →  সরাও (চেপে রাখলে চলতে থাকে)     ↑ বা X  ঘোরাও   Z  উল্টো ঘোরাও
 *   ↓    আস্তে নামাও                      Space  এক ধাক্কায় ফেলো
 *   C বা Shift  রেখে দাও (hold)            P  থামাও   Esc  মেনু
 *
 * নিজে বদলে চালাতে:  cd /usr/src/games; tcc tetris.c -lrozgame -lrozgfx -lm -o /tmp/t; /tmp/t */
#include <stdio.h>
#include <string.h>
#include <rozgame.h>

#define BW 10
#define BH 20
#define CELL 24
#define OX 200                     /* বোর্ডের বাঁ কিনারা */
#define OY 0

enum { S_MENU, S_PLAY, S_CLEAR, S_PAUSE, S_OVER };

/* I O T S Z J L — প্রতিটার ঘর (বাক্সের ভেতরে) আর বাক্সের মাপ */
static const int SHAPE[7][4][2] = {
    { {0, 1}, {1, 1}, {2, 1}, {3, 1} },   /* I */
    { {1, 0}, {2, 0}, {1, 1}, {2, 1} },   /* O */
    { {1, 0}, {0, 1}, {1, 1}, {2, 1} },   /* T */
    { {1, 0}, {2, 0}, {0, 1}, {1, 1} },   /* S */
    { {0, 0}, {1, 0}, {1, 1}, {2, 1} },   /* Z */
    { {0, 0}, {0, 1}, {1, 1}, {2, 1} },   /* J */
    { {2, 0}, {0, 1}, {1, 1}, {2, 1} }    /* L */
};
static const int BOX[7] = { 4, 4, 3, 3, 3, 3, 3 };
static const char *menu_items[] = { "Play", "Quit" };
static int autoplay;

typedef struct { int type, rot, x, y; } piece;

static struct {
    int state;
    unsigned char board[BH][BW];          /* 0 খালি, নইলে রং + 1 */
    piece cur;
    int next[3], hold, held_this_turn;
    int bag[7], bag_n;
    long score, best;
    int lines, level, new_best;
    float fall_t, lock_t, das_t, clear_t;
    int lock_moves, das_dir;
    int clear_rows[4], nclear;
    game_sheet *blocks;
    game_image *bg;
    game_particles *ps;
    game_menu menu;
} G;

static void cells(const piece *p, int out[4][2])
{
    int i, r, n = BOX[p->type];
    for (i = 0; i < 4; i++) {
        int x = SHAPE[p->type][i][0], y = SHAPE[p->type][i][1];
        if (p->type != 1)
            for (r = 0; r < p->rot; r++) {             /* ঘড়ির কাঁটার দিকে: (x, y) → (n−1−y, x) */
                int t = x;
                x = n - 1 - y;
                y = t;
            }
        out[i][0] = p->x + x;
        out[i][1] = p->y + y;
    }
}

static int fits(const piece *p)
{
    int c[4][2], i;
    cells(p, c);
    for (i = 0; i < 4; i++) {
        if (c[i][0] < 0 || c[i][0] >= BW || c[i][1] >= BH) return 0;
        if (c[i][1] >= 0 && G.board[c[i][1]][c[i][0]]) return 0;
    }
    return 1;
}

static int bag_next(void)
{
    int i;
    if (G.bag_n == 0) {                                 /* ৭-ব্যাগ: প্রতি ৭টায় সব টুকরো একবার */
        for (i = 0; i < 7; i++) G.bag[i] = i;
        for (i = 6; i > 0; i--) {
            int j = game_rand_int(0, i), t = G.bag[i];
            G.bag[i] = G.bag[j];
            G.bag[j] = t;
        }
        G.bag_n = 7;
    }
    return G.bag[--G.bag_n];
}

static void spawn(int type)
{
    G.cur.type = type;
    G.cur.rot = 0;
    G.cur.x = (BW - BOX[type]) / 2;
    G.cur.y = type == 0 ? -1 : 0;
    G.fall_t = G.lock_t = 0;
    G.lock_moves = 0;
    if (!fits(&G.cur)) {
        G.new_best = game_score_save("tetris", G.score);
        G.best = game_score_load("tetris");
        G.state = S_OVER;
    }
}

static void spawn_next(void)
{
    int t = G.next[0];
    G.next[0] = G.next[1];
    G.next[1] = G.next[2];
    G.next[2] = bag_next();
    G.held_this_turn = 0;
    spawn(t);
}

static void new_game(void)
{
    memset(G.board, 0, sizeof(G.board));
    G.bag_n = 0;
    G.next[0] = bag_next();
    G.next[1] = bag_next();
    G.next[2] = bag_next();
    G.hold = -1;
    G.score = 0;
    G.lines = 0;
    G.level = 1;
    G.new_best = 0;
    G.state = S_PLAY;
    spawn_next();
}

static int try_move(int dx, int dy)
{
    piece p = G.cur;
    p.x += dx;
    p.y += dy;
    if (!fits(&p)) return 0;
    G.cur = p;
    if (G.lock_moves < 15) { G.lock_t = 0; G.lock_moves++; }   /* মাটিতে নড়লে একটু সময় বাড়ে */
    return 1;
}

static void rotate(int dir)
{
    /* সাধারণ wall kick: জায়গা না হলে পাশে/ওপরে সরিয়ে চেষ্টা */
    static const int kick[][2] = { {0, 0}, {-1, 0}, {1, 0}, {0, -1}, {-2, 0}, {2, 0}, {-1, -1}, {1, -1} };
    piece p = G.cur;
    int i;
    if (p.type == 1) return;
    p.rot = (p.rot + dir + 4) % 4;
    for (i = 0; i < (int)(sizeof(kick) / sizeof(kick[0])); i++) {
        piece q = p;
        q.x += kick[i][0];
        q.y += kick[i][1];
        if (fits(&q)) {
            G.cur = q;
            if (G.lock_moves < 15) { G.lock_t = 0; G.lock_moves++; }
            return;
        }
    }
}

static float fall_interval(void)
{
    float t = 0.8f - (G.level - 1) * 0.07f;
    return t < 0.05f ? 0.05f : t;
}

static void lock_piece(void)
{
    int c[4][2], i, r, x;
    cells(&G.cur, c);
    for (i = 0; i < 4; i++)
        if (c[i][1] >= 0) G.board[c[i][1]][c[i][0]] = (unsigned char)(G.cur.type + 1);
    G.nclear = 0;
    for (r = 0; r < BH; r++) {
        int full = 1;
        for (x = 0; x < BW; x++) if (!G.board[r][x]) { full = 0; break; }
        if (full && G.nclear < 4) G.clear_rows[G.nclear++] = r;
    }
    if (G.nclear > 0) {
        static const int pts[5] = { 0, 100, 300, 500, 800 };
        game_emitter e = game_emitter_default(GFX_WHITE);
        e.speed_max = 260;
        G.score += (long)pts[G.nclear] * G.level;
        for (i = 0; i < G.nclear; i++)
            for (x = 0; x < BW; x += 2) {
                e.color_start = game_hsv((float)(x * 36 + i * 50), 0.6f, 1) | 0xFF000000u;
                e.color_end = e.color_start & 0xFFFFFF;
                game_particles_burst(G.ps, (float)(OX + x * CELL + CELL), (float)(OY + G.clear_rows[i] * CELL + CELL / 2), 6, &e);
            }
        G.state = S_CLEAR;
        G.clear_t = 0.25f;
    } else {
        spawn_next();
    }
}

static void finish_clear(void)
{
    int i, r;
    for (i = 0; i < G.nclear; i++) {
        for (r = G.clear_rows[i]; r > 0; r--) memcpy(G.board[r], G.board[r - 1], BW);
        memset(G.board[0], 0, BW);
    }
    G.lines += G.nclear;
    G.level = 1 + G.lines / 10;
    G.nclear = 0;
    G.state = S_PLAY;
    spawn_next();
}

static void hard_drop(void)
{
    int n = 0;
    while (try_move(0, 1)) n++;
    G.score += 2L * n;
    lock_piece();
}

static void update_play(float dt)
{
    int dir = 0;
    if (game_key_pressed('p') || !game_focused()) { G.state = S_PAUSE; return; }
    if (game_key_pressed(GFX_KEY_ESC)) { G.state = S_MENU; return; }
    if (game_key_pressed(GFX_KEY_UP) || game_key_pressed('x')) rotate(1);
    if (game_key_pressed('z')) rotate(-1);
    if ((game_key_pressed('c') || game_key_pressed(GFX_KEY_LSHIFT) || game_key_pressed(GFX_KEY_RSHIFT)) && !G.held_this_turn) {
        int t = G.cur.type;
        if (G.hold < 0) { G.hold = t; spawn_next(); }
        else { int h = G.hold; G.hold = t; spawn(h); }
        G.held_this_turn = 1;
        return;
    }
    if (game_key_pressed(' ')) { hard_drop(); return; }

    /* বাঁ/ডান: চাপলে এক ঘর, চেপে রাখলে ০.১৭ সেকেন্ড পরে প্রতি ০.০৫ সেকেন্ডে */
    if (game_key_down(GFX_KEY_LEFT) || game_key_down('a')) dir = -1;
    if (game_key_down(GFX_KEY_RIGHT) || game_key_down('d')) dir = dir ? 0 : 1;
    if (game_key_pressed(GFX_KEY_LEFT) || game_key_pressed('a')) { try_move(-1, 0); G.das_dir = -1; G.das_t = 0; }
    else if (game_key_pressed(GFX_KEY_RIGHT) || game_key_pressed('d')) { try_move(1, 0); G.das_dir = 1; G.das_t = 0; }
    else if (dir && dir == G.das_dir) {
        G.das_t += dt;
        while (G.das_t >= 0.17f) {
            try_move(dir, 0);
            G.das_t -= 0.05f;
        }
    } else {
        G.das_dir = dir;
        G.das_t = 0;
    }

    /* পড়া */
    G.fall_t += dt;
    {
        float iv = (game_key_down(GFX_KEY_DOWN) || game_key_down('s')) ? 0.04f : fall_interval();
        while (G.fall_t >= iv) {
            G.fall_t -= iv;
            if (try_move(0, 1)) {
                if (iv < 0.05f) G.score += 1;
                G.lock_t = 0;
            }
        }
    }
    /* মাটিতে বসে থাকলে ০.৫ সেকেন্ড পরে আটকে যায় */
    {
        piece p = G.cur;
        p.y++;
        if (!fits(&p)) {
            G.lock_t += dt;
            if (G.lock_t >= 0.5f) lock_piece();
        } else {
            G.lock_t = 0;
        }
    }
}

static void update(void *ud, float dt)
{
    (void)ud;
    game_particles_update(G.ps, dt);
    switch (G.state) {
    case S_MENU: {
        int c = game_menu_update(&G.menu);
        if (c == 0) new_game();
        else if (c == 1 || game_key_pressed(GFX_KEY_ESC)) game_quit();
        break;
    }
    case S_PLAY:
        update_play(dt);
        break;
    case S_CLEAR:
        G.clear_t -= dt;
        if (G.clear_t <= 0) finish_clear();
        break;
    case S_PAUSE:
        if (game_key_pressed('p') || game_key_pressed(GFX_KEY_ENTER) || game_key_pressed(' ')) G.state = S_PLAY;
        if (game_key_pressed(GFX_KEY_ESC)) G.state = S_MENU;
        break;
    case S_OVER:
        if (game_key_pressed(GFX_KEY_ENTER) || game_key_pressed(' ')) new_game();
        if (game_key_pressed(GFX_KEY_ESC)) G.state = S_MENU;
        break;
    }
}

static void block(int color, int x, int y)
{
    if (G.blocks) game_draw_frame(G.blocks, color, x, y, 0);
    else game_fill_rect(x + 1, y + 1, CELL - 2, CELL - 2, game_hsv((float)color * 50, 0.7f, 0.9f));
}

static void draw_piece_at(int type, int x, int y, int size)
{
    piece p;
    int c[4][2], i;
    memset(&p, 0, sizeof(p));
    p.type = type;
    cells(&p, c);
    for (i = 0; i < 4; i++) {
        if (size == CELL) block(type, x + c[i][0] * CELL, y + c[i][1] * CELL);
        else if (G.blocks) game_draw_frame_scaled(G.blocks, type, x + c[i][0] * size, y + c[i][1] * size, size, size, 0);
    }
}

static void panel(int x, int y, int w, int h, const char *title)
{
    game_fill_rect(x, y, w, h, GFX_RGBA(0, 0, 0, 150));
    game_draw_rect(x, y, w, h, GFX_RGBA(120, 140, 200, 255));
    if (title) game_ttf(NULL, 18, x + 10, y + 6, title, GFX_RGB(180, 200, 255));
}

static void draw(void *ud, gfx_surface *scr)
{
    int r, x, i;
    char b[64];
    (void)ud; (void)scr;
    if (G.bg) game_draw_background(G.bg);
    else game_gradient(0, 0, 640, 480, GFX_RGB(14, 16, 40), GFX_RGB(30, 26, 70));

    /* বোর্ড */
    game_fill_rect(OX - 4, OY, BW * CELL + 8, BH * CELL, GFX_RGBA(0, 0, 0, 190));
    for (x = 1; x < BW; x++) game_fill_rect(OX + x * CELL, OY, 1, BH * CELL, GFX_RGBA(255, 255, 255, 14));
    for (r = 1; r < BH; r++) game_fill_rect(OX, OY + r * CELL, BW * CELL, 1, GFX_RGBA(255, 255, 255, 14));
    game_fill_rect(OX - 4, OY, 3, BH * CELL, GFX_RGB(120, 140, 200));
    game_fill_rect(OX + BW * CELL + 1, OY, 3, BH * CELL, GFX_RGB(120, 140, 200));
    for (r = 0; r < BH; r++)
        for (x = 0; x < BW; x++)
            if (G.board[r][x]) block(G.board[r][x] - 1, OX + x * CELL, OY + r * CELL);
    if (G.state == S_CLEAR) {                              /* মুছে যাওয়া সারি ঝলকায় */
        int a = (int)(255 * G.clear_t / 0.25f);
        for (i = 0; i < G.nclear; i++)
            game_fill_rect(OX, OY + G.clear_rows[i] * CELL, BW * CELL, CELL, GFX_RGBA(255, 255, 255, a < 0 ? 0 : a));
    }
    if (G.state == S_PLAY || G.state == S_PAUSE) {
        piece ghost = G.cur;
        int c[4][2];
        while (1) { ghost.y++; if (!fits(&ghost)) { ghost.y--; break; } }
        cells(&ghost, c);                                  /* কোথায় পড়বে তার ছায়া */
        for (i = 0; i < 4; i++)
            if (c[i][1] >= 0) {
                game_fill_rect(OX + c[i][0] * CELL + 2, OY + c[i][1] * CELL + 2, CELL - 4, CELL - 4, GFX_RGBA(255, 255, 255, 40));
                game_draw_rect(OX + c[i][0] * CELL + 2, OY + c[i][1] * CELL + 2, CELL - 4, CELL - 4, GFX_RGBA(255, 255, 255, 255));
            }
        cells(&G.cur, c);
        for (i = 0; i < 4; i++)
            if (c[i][1] >= 0) block(G.cur.type, OX + c[i][0] * CELL, OY + c[i][1] * CELL);
    }
    game_particles_draw(G.ps, 0, 0);

    /* বাঁ পাশ: hold, স্কোর */
    panel(30, 20, 140, 110, "Hold");
    if (G.hold >= 0 && G.state != S_MENU) draw_piece_at(G.hold, 50, 60, 20);
    panel(30, 150, 140, 190, NULL);
    game_ttf(NULL, 16, 42, 160, "Score", GFX_RGB(180, 200, 255));
    snprintf(b, sizeof(b), "%ld", G.score);
    game_ttf(NULL, 26, 42, 180, b, GFX_WHITE);
    game_ttf(NULL, 16, 42, 220, "Level", GFX_RGB(180, 200, 255));
    snprintf(b, sizeof(b), "%d", G.level);
    game_ttf(NULL, 26, 42, 240, b, GFX_WHITE);
    game_ttf(NULL, 16, 42, 280, "Lines", GFX_RGB(180, 200, 255));
    snprintf(b, sizeof(b), "%d", G.lines);
    game_ttf(NULL, 26, 42, 300, b, GFX_WHITE);
    snprintf(b, sizeof(b), "Best %ld", G.best > G.score ? G.best : G.score);
    game_ttf(NULL, 16, 36, 360, b, GFX_RGB(255, 220, 120));

    /* ডান পাশ: পরের ৩টা */
    panel(470, 20, 140, 300, "Next");
    if (G.state != S_MENU)
        for (i = 0; i < 3; i++) draw_piece_at(G.next[i], 490, 60 + i * 85, i == 0 ? 24 : 18);
    game_ttf(NULL, 13, 470, 340, "arrows move/rotate", GFX_RGB(200, 200, 210));
    game_ttf(NULL, 13, 470, 358, "Space drop, C hold", GFX_RGB(200, 200, 210));
    game_ttf(NULL, 13, 470, 376, "Z rotate back, P pause", GFX_RGB(200, 200, 210));

    switch (G.state) {
    case S_MENU:
        game_fill_rect(0, 0, 640, 480, GFX_RGBA(0, 0, 0, 130));
        game_ttf_center(NULL, 64, 120, "TETRIS", GFX_RGB(120, 210, 255));
        game_menu_draw(&G.menu, 230);
        break;
    case S_PAUSE:
        game_fill_rect(OX, 0, BW * CELL, 480, GFX_RGBA(0, 0, 0, 170));
        game_ttf_center(NULL, 40, 210, "Paused", GFX_WHITE);
        break;
    case S_OVER:
        game_fill_rect(0, 0, 640, 480, GFX_RGBA(0, 0, 0, 150));
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
    (void)ud;
    G.blocks = game_sheet_load(game_data_path("tetris-blocks.png"), CELL, CELL);
    G.bg = game_image_load(game_data_path("tetris-bg.png"));
    G.ps = game_particles_new(400);
    G.best = game_score_load("tetris");
    G.hold = -1;
    G.level = 1;
    G.menu.items = menu_items;
    G.menu.count = 2;
    G.menu.size = 30;
    G.menu.y = 230;
    G.state = S_MENU;
    if (autoplay) new_game();
    return 0;
}

static void quit(void *ud)
{
    (void)ud;
    game_sheet_free(G.blocks);
    game_image_free(G.bg);
    game_particles_free(G.ps);
}

int main(int argc, char **argv)
{
    game_config c = game_default_config("Tetris");
    game_app a;
    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f")) c.show_fps = 1;
        else if (!strcmp(argv[i], "-p")) autoplay = 1;
        else {
            fprintf(stderr, "usage: tetris [-f] [-p]   (-f fps, -p start playing)\n");
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
