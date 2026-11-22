/* rozOS — Platformer (ধাপ ১৮খ): টাইল ম্যাপ, ক্যামেরা, অ্যানিমেশন আর কণার উদাহরণ
 *
 * খেলা: ← → বা A D দৌড়াও, ↑ / W / Space লাফাও (বেশিক্ষণ চাপলে বেশি উঁচু)।
 * মুদ্রা তোলো, slime-এর মাথায় লাফিয়ে পড়ো, কাঁটা আর খাদ এড়াও, পতাকায় পৌঁছাও।
 * P থামাও, Esc মেনু।
 *
 * লেভেল: /usr/share/games/platformer-1.map, -2.map — টেক্সট ফাইল, নিজে বানানো যায়:
 *   platformer mylevel.map     (help platformer)
 *
 * নিজে বদলে চালাতে:  cd /usr/src/games; tcc platformer.c -lrozgame -lrozgfx -lm -o /tmp/p; /tmp/p */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <rozgame.h>

#define MAXCOIN 128
#define MAXENEMY 32

#define GRAVITY   1500.0f
#define MAX_FALL  720.0f
#define RUN_SPEED 220.0f
#define JUMP_V    560.0f

enum { S_MENU, S_PLAY, S_DEAD, S_DONE, S_WIN, S_OVER, S_PAUSE };

typedef struct { float x, y; int taken; } coin;
typedef struct { game_rect r; float vx, vy; int alive; float squash; } enemy;

static const char *menu_items[] = { "Play", "Quit" };
static int autoplay;
static const char *custom_map;                 /* কমান্ড লাইনে দেওয়া ম্যাপ */

static struct {
    int state, level, lives, coins;
    long score, best;
    game_tilemap *map;
    game_camera cam;
    game_rect pr;                              /* খেলোয়াড়ের ঘর (sprite-এর চেয়ে ছোট) */
    float vx, vy, coyote, jump_buf, timer, start_x, start_y;
    int on_ground, facing_left;
    game_anim anim;
    coin coin[MAXCOIN];
    int ncoin;
    enemy en[MAXENEMY];
    int nen;
    float t;
    game_sheet *hero, *coin_img, *slime;
    game_image *hills;
    game_particles *ps;
    game_menu menu;
} G;

static int load_level(int level)
{
    char name[64];
    const char *path;
    int idx, c, r;
    if (custom_map) path = custom_map;
    else {
        snprintf(name, sizeof(name), "platformer-%d.map", level);
        path = game_data_path(name);
        if (access(path, R_OK) != 0) return -1;     /* আর লেভেল নেই — আগের ম্যাপ থাকে */
    }
    game_tilemap_free(G.map);
    G.map = game_tilemap_load(path);
    if (!G.map) return -1;
    G.map->edges = GAME_EDGE_LEFT | GAME_EDGE_RIGHT;  /* নিচে খাদ — পড়ে যাওয়া যায় */
    /* বস্তু বের করে ম্যাপ থেকে মুছে ফেলা */
    G.ncoin = G.nen = 0;
    G.start_x = 64; G.start_y = 64;
    for (idx = game_tilemap_find(G.map, 'P', 0, &c, &r); idx >= 0; idx = game_tilemap_find(G.map, 'P', idx + 1, &c, &r)) {
        G.start_x = (float)(c * G.map->tw + (G.map->tw - 20) / 2);
        G.start_y = (float)(r * G.map->th + G.map->th - 28);
        game_tilemap_set(G.map, c, r, '.');
    }
    for (idx = game_tilemap_find(G.map, 'C', 0, &c, &r); idx >= 0 && G.ncoin < MAXCOIN; idx = game_tilemap_find(G.map, 'C', idx + 1, &c, &r)) {
        G.coin[G.ncoin].x = (float)(c * G.map->tw + G.map->tw / 2);
        G.coin[G.ncoin].y = (float)(r * G.map->th + G.map->th / 2);
        G.coin[G.ncoin].taken = 0;
        G.ncoin++;
        game_tilemap_set(G.map, c, r, '.');
    }
    for (idx = game_tilemap_find(G.map, 'E', 0, &c, &r); idx >= 0 && G.nen < MAXENEMY; idx = game_tilemap_find(G.map, 'E', idx + 1, &c, &r)) {
        enemy *e = &G.en[G.nen++];
        e->r = game_make_rect((float)(c * G.map->tw + 2), (float)(r * G.map->th + G.map->th - 20), 28, 20);
        e->vx = -50;
        e->vy = 0;
        e->alive = 1;
        e->squash = 0;
        game_tilemap_set(G.map, c, r, '.');
    }
    game_camera_init(&G.cam);
    game_camera_bounds(&G.cam, 0, 0, (float)(G.map->cols * G.map->tw), (float)(G.map->rows * G.map->th));
    return 0;
}

static void respawn(void)
{
    G.pr = game_make_rect(G.start_x, G.start_y, 20, 28);
    G.vx = G.vy = 0;
    G.facing_left = 0;
    G.coyote = G.jump_buf = 0;
    game_camera_follow(&G.cam, G.pr.x, G.pr.y, 0, 0);
    G.state = S_PLAY;
}

static void start_level(int level)
{
    G.level = level;
    if (load_level(level) < 0) {
        G.state = S_WIN;                       /* আর লেভেল নেই */
        return;
    }
    respawn();
}

static void new_game(void)
{
    G.lives = 3;
    G.score = 0;
    G.coins = 0;
    start_level(1);
}

static void die(void)
{
    game_emitter e = game_emitter_default(GFX_RGB(250, 140, 60));
    e.speed_max = 300;
    game_particles_burst(G.ps, G.pr.x + 10, G.pr.y + 14, 40, &e);
    game_camera_shake(&G.cam, 8, 0.4f);
    G.lives--;
    G.state = S_DEAD;
    G.timer = 1.2f;
}

/* খেলোয়াড়ের ঘরের নিচে কোন অক্ষর আছে (কাঁটা, পতাকা) */
static int touches(int ch)
{
    int c0 = (int)floorf(G.pr.x / G.map->tw), c1 = (int)floorf((G.pr.x + G.pr.w - 1) / G.map->tw);
    int r0 = (int)floorf(G.pr.y / G.map->th), r1 = (int)floorf((G.pr.y + G.pr.h - 1) / G.map->th), c, r;
    for (r = r0; r <= r1; r++)
        for (c = c0; c <= c1; c++)
            if (game_tilemap_get(G.map, c, r) == ch) {
                /* কাঁটা টাইলের শুধু নিচের অর্ধেক বিপজ্জনক */
                if (ch == '^' && G.pr.y + G.pr.h < r * G.map->th + G.map->th / 2) continue;
                return 1;
            }
    return 0;
}

static void update_enemies(float dt)
{
    int i;
    for (i = 0; i < G.nen; i++) {
        enemy *e = &G.en[i];
        int hit;
        if (!e->alive) {
            if (e->squash > 0) e->squash -= dt;
            continue;
        }
        e->vy += GRAVITY * dt;
        if (e->vy > MAX_FALL) e->vy = MAX_FALL;
        hit = game_tilemap_move(G.map, &e->r, e->vx * dt, e->vy * dt);
        if (hit & (GAME_HIT_BOTTOM | GAME_HIT_TOP)) e->vy = 0;
        if (hit & (GAME_HIT_LEFT | GAME_HIT_RIGHT)) e->vx = -e->vx;
        /* কিনারায় এলে ঘুরে দাঁড়ায় (খাদে পড়ে না) */
        if (hit & GAME_HIT_BOTTOM) {
            float fx = e->vx < 0 ? e->r.x - 2 : e->r.x + e->r.w + 2;
            if (!game_tilemap_solid_at(G.map, fx, e->r.y + e->r.h + 4)) e->vx = -e->vx;
        }
        if (e->r.y > G.map->rows * G.map->th + 64) e->alive = 0;
        if (G.state == S_PLAY && game_rect_overlap(G.pr, e->r)) {
            /* ওপর থেকে পড়লে slime চ্যাপ্টা, নইলে খেলোয়াড় হারায় */
            if (G.vy > 0 && G.pr.y + G.pr.h - e->r.y < 14) {
                game_emitter em = game_emitter_default(GFX_RGB(180, 90, 230));
                e->alive = 0;
                e->squash = 0.4f;
                G.vy = -JUMP_V * 0.7f;
                G.score += 50;
                game_particles_burst(G.ps, e->r.x + 14, e->r.y + 10, 20, &em);
            } else {
                die();
            }
        }
    }
}

static void update_player(float dt)
{
    int left = game_key_down(GFX_KEY_LEFT) || game_key_down('a');
    int right = game_key_down(GFX_KEY_RIGHT) || game_key_down('d');
    int jump_held = game_key_down(GFX_KEY_UP) || game_key_down('w') || game_key_down(' ');
    float target = (float)(right - left) * RUN_SPEED, accel = G.on_ground ? 2200.0f : 1300.0f;
    int hit, i;

    if (game_key_pressed(GFX_KEY_UP) || game_key_pressed('w') || game_key_pressed(' ')) G.jump_buf = 0.12f;
    G.vx = game_approach(G.vx, target, accel * dt);
    if (left && !right) G.facing_left = 1;
    if (right && !left) G.facing_left = 0;

    /* লাফ: মাটি ছাড়ার পরেও একটু সময় (coyote), আর একটু আগে চাপলেও মনে রাখা (buffer) */
    G.coyote = G.on_ground ? 0.09f : G.coyote - dt;
    G.jump_buf -= dt;
    if (G.jump_buf > 0 && G.coyote > 0) {
        game_emitter e = game_emitter_default(GFX_RGB(230, 230, 220));
        G.vy = -JUMP_V;
        G.jump_buf = G.coyote = 0;
        e.angle = 1.5708f; e.spread = 2.5f; e.speed_max = 80; e.gravity = 0; e.size_start = 3;
        game_particles_burst(G.ps, G.pr.x + 10, G.pr.y + G.pr.h, 6, &e);
    }
    if (!jump_held && G.vy < -220) G.vy = -220;        /* ছেড়ে দিলে নিচু লাফ */
    G.vy += GRAVITY * dt;
    if (G.vy > MAX_FALL) G.vy = MAX_FALL;

    hit = game_tilemap_move(G.map, &G.pr, G.vx * dt, G.vy * dt);
    G.on_ground = (hit & GAME_HIT_BOTTOM) != 0;
    if (hit & (GAME_HIT_BOTTOM | GAME_HIT_TOP)) G.vy = 0;
    if (hit & (GAME_HIT_LEFT | GAME_HIT_RIGHT)) G.vx = 0;

    /* অ্যানিমেশন */
    if (!G.on_ground) game_anim_set(&G.anim, G.vy < 0 ? 6 : 7, 1, 1, 1);
    else if (fabsf(G.vx) > 20) game_anim_set(&G.anim, 2, 4, 12, 1);
    else game_anim_set(&G.anim, 0, 2, 2, 1);
    game_anim_update(&G.anim, dt);

    /* মুদ্রা */
    for (i = 0; i < G.ncoin; i++) {
        coin *c = &G.coin[i];
        if (c->taken) continue;
        if (game_rect_overlap(G.pr, game_make_rect(c->x - 9, c->y - 9, 18, 18))) {
            game_emitter e = game_emitter_default(GFX_RGB(255, 220, 80));
            e.gravity = -40; e.speed_max = 120; e.size_start = 3;
            c->taken = 1;
            G.coins++;
            G.score += 10;
            game_particles_burst(G.ps, c->x, c->y, 12, &e);
        }
    }
    if (touches('^') || G.pr.y > G.map->rows * G.map->th + 40) { die(); return; }
    if (touches('F') || touches('|')) {
        game_emitter e = game_emitter_default(GFX_WHITE);
        int k;
        for (k = 0; k < 5; k++) {
            e.color_start = game_hsv((float)(k * 70), 0.8f, 1) | 0xFF000000u;
            e.color_end = e.color_start & 0xFFFFFF;
            e.speed_max = 320;
            game_particles_burst(G.ps, G.pr.x + 10, G.pr.y, 20, &e);
        }
        G.score += 500;
        G.state = S_DONE;
        G.timer = 2.0f;
    }
}

static void update(void *ud, float dt)
{
    (void)ud;
    G.t += dt;
    game_particles_update(G.ps, dt);
    game_camera_update(&G.cam, dt);
    switch (G.state) {
    case S_MENU: {
        int c = game_menu_update(&G.menu);
        if (c == 0) new_game();
        else if (c == 1 || game_key_pressed(GFX_KEY_ESC)) game_quit();
        break;
    }
    case S_PLAY:
        if (game_key_pressed('p') || !game_focused()) { G.state = S_PAUSE; break; }
        if (game_key_pressed(GFX_KEY_ESC)) { G.state = S_MENU; break; }
        update_player(dt);
        update_enemies(dt);
        game_camera_follow(&G.cam, G.pr.x + G.pr.w / 2, G.pr.y, 7, dt);
        break;
    case S_DEAD:
        update_enemies(dt);
        G.timer -= dt;
        if (G.timer <= 0) {
            if (G.lives <= 0) {
                game_score_save("platformer", G.score);
                G.best = game_score_load("platformer");
                G.state = S_OVER;
            } else {
                respawn();
            }
        }
        break;
    case S_DONE:
        G.timer -= dt;
        if (G.timer <= 0) {
            if (custom_map) G.state = S_WIN;
            else start_level(G.level + 1);
            if (G.state == S_WIN) {
                game_score_save("platformer", G.score);
                G.best = game_score_load("platformer");
            }
        }
        break;
    case S_PAUSE:
        if (game_key_pressed('p') || game_key_pressed(GFX_KEY_ENTER)) G.state = S_PLAY;
        if (game_key_pressed(GFX_KEY_ESC)) G.state = S_MENU;
        break;
    case S_WIN:
    case S_OVER:
        if (game_key_pressed(GFX_KEY_ENTER) || game_key_pressed(' ')) new_game();
        if (game_key_pressed(GFX_KEY_ESC)) G.state = S_MENU;
        break;
    }
}

static void draw(void *ud, gfx_surface *scr)
{
    int vx = game_camera_view_x(&G.cam), vy = game_camera_view_y(&G.cam), i, w = game_width(), h = game_height();
    char b[64];
    (void)ud; (void)scr;
    game_gradient(0, 0, w, h, GFX_RGB(110, 180, 240), GFX_RGB(200, 230, 250));
    if (G.hills) {                                         /* parallax: দূরের পাহাড় ধীরে সরে */
        int px = -((vx * 3 / 10) % G.hills->w);
        if (px > 0) px -= G.hills->w;
        game_draw_image(G.hills, px, h - G.hills->h - vy / 10);
        game_draw_image(G.hills, px + G.hills->w, h - G.hills->h - vy / 10);
    }
    if (G.map) game_tilemap_draw(G.map, vx, vy);

    if (G.map && G.state != S_MENU) {
        for (i = 0; i < G.ncoin; i++) {
            coin *c = &G.coin[i];
            if (c->taken) continue;
            if (G.coin_img) game_draw_frame(G.coin_img, (int)(G.t * 10 + i) % 6, (int)c->x - 10 - vx, (int)c->y - 10 - vy + (int)(sinf(G.t * 4 + i) * 2), 0);
            else game_fill_circle((int)c->x - vx, (int)c->y - vy, 8, GFX_YELLOW);
        }
        for (i = 0; i < G.nen; i++) {
            enemy *e = &G.en[i];
            int ex = (int)e->r.x - 2 - vx, ey = (int)e->r.y - 4 - vy;
            if (e->alive) {
                if (G.slime) game_draw_frame(G.slime, (int)(G.t * 4 + i) % 2, ex, ey, e->vx > 0 ? GAME_FLIP_X : 0);
                else game_fill_rect(ex, ey + 4, 28, 20, GFX_RGB(150, 60, 200));
            } else if (e->squash > 0 && G.slime) {
                game_draw_frame_scaled(G.slime, 1, ex - 4, ey + 16, 40, 8, 0);
            }
        }
        if (G.state != S_DEAD && G.state != S_OVER) {
            int fx = (int)G.pr.x - 6 - vx, fy = (int)G.pr.y - 4 - vy;
            if (G.hero) game_draw_frame(G.hero, game_anim_frame(&G.anim), fx, fy, G.facing_left ? GAME_FLIP_X : 0);
            else game_fill_rect((int)G.pr.x - vx, (int)G.pr.y - vy, 20, 28, GFX_RGB(240, 120, 50));
        }
    }
    game_particles_draw(G.ps, vx, vy);

    /* ওপরের পট্টি */
    game_fill_rect(0, 0, w, 30, GFX_RGBA(0, 0, 0, 120));
    if (G.coin_img) game_draw_frame(G.coin_img, 0, 10, 5, 0);
    snprintf(b, sizeof(b), "x %d", G.coins);
    game_ttf(NULL, 18, 34, 5, b, GFX_WHITE);
    snprintf(b, sizeof(b), "Score %ld", G.score);
    game_ttf(NULL, 18, 110, 5, b, GFX_WHITE);
    snprintf(b, sizeof(b), "Level %d", G.level);
    game_ttf_center(NULL, 18, 5, b, GFX_RGB(255, 240, 180));
    for (i = 0; i < G.lives && G.state != S_MENU; i++)
        if (G.hero) game_draw_frame_scaled(G.hero, 0, w - 30 - i * 26, 3, 24, 24, 0);

    switch (G.state) {
    case S_MENU:
        game_fill_rect(0, 0, w, h, GFX_RGBA(0, 0, 0, 100));
        game_ttf_center(NULL, 60, 100, "PLATFORMER", GFX_RGB(255, 200, 90));
        snprintf(b, sizeof(b), "Best %ld", G.best);
        game_ttf_center(NULL, 20, 180, b, GFX_RGB(255, 230, 150));
        game_menu_draw(&G.menu, 230);
        game_ttf_center(NULL, 16, 420, "arrows / A D run, Up / W / Space jump, P pause, Esc menu", GFX_WHITE);
        break;
    case S_DONE:
        game_ttf_center(NULL, 44, 200, "Level clear!", GFX_RGB(255, 255, 160));
        break;
    case S_PAUSE:
        game_fill_rect(0, 0, w, h, GFX_RGBA(0, 0, 0, 130));
        game_ttf_center(NULL, 48, 200, "Paused", GFX_WHITE);
        break;
    case S_WIN:
    case S_OVER:
        game_fill_rect(0, 0, w, h, GFX_RGBA(0, 0, 0, 150));
        game_ttf_center(NULL, 52, 150, G.state == S_WIN ? "You win!" : "Game over", G.state == S_WIN ? GFX_RGB(140, 255, 140) : GFX_RGB(255, 110, 90));
        snprintf(b, sizeof(b), "Score %ld   Coins %d", G.score, G.coins);
        game_ttf_center(NULL, 26, 230, b, GFX_WHITE);
        game_ttf_center(NULL, 18, 290, "Enter to play again, Esc for menu", GFX_RGB(220, 220, 220));
        break;
    default:
        break;
    }
}

static int init(void *ud)
{
    (void)ud;
    G.hero = game_sheet_load(game_data_path("platformer-hero.png"), 32, 32);
    G.coin_img = game_sheet_load(game_data_path("platformer-coin.png"), 20, 20);
    G.slime = game_sheet_load(game_data_path("platformer-slime.png"), 32, 24);
    G.hills = game_image_load(game_data_path("platformer-hills.png"));
    G.ps = game_particles_new(600);
    G.best = game_score_load("platformer");
    G.menu.items = menu_items;
    G.menu.count = 2;
    G.menu.size = 30;
    G.menu.y = 230;
    G.level = 1;
    G.lives = 3;
    if (load_level(1) < 0) {
        fprintf(stderr, "platformer: cannot load the level map\n");
        return 1;
    }
    G.state = S_MENU;
    if (autoplay) new_game();
    return 0;
}

static void quit(void *ud)
{
    (void)ud;
    game_tilemap_free(G.map);
    game_sheet_free(G.hero);
    game_sheet_free(G.coin_img);
    game_sheet_free(G.slime);
    game_image_free(G.hills);
    game_particles_free(G.ps);
}

int main(int argc, char **argv)
{
    game_config c = game_default_config("Platformer");
    game_app a;
    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f")) c.show_fps = 1;
        else if (!strcmp(argv[i], "-p")) autoplay = 1;
        else if (argv[i][0] != '-') custom_map = argv[i];
        else {
            fprintf(stderr, "usage: platformer [-f] [-p] [level.map]   (-f fps, -p start playing)\n");
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
