/* New game for rozOS (librozgame) - help rozgame
 * Ctrl+S: save (asks for a name)   Ctrl+R / F5: compile and run   Esc in the game: quit
 * Move the blue square with the arrows, catch the yellow stars. */
#include <stdio.h>
#include <rozgame.h>

static float px = 300, py = 400;        /* the player */
static float sx, sy = -20;              /* the star */
static int score;

static void update(void *ud, float dt)
{
    (void)ud;
    if (game_key_down(GFX_KEY_LEFT))  px -= 300 * dt;
    if (game_key_down(GFX_KEY_RIGHT)) px += 300 * dt;
    if (game_key_down(GFX_KEY_UP))    py -= 300 * dt;
    if (game_key_down(GFX_KEY_DOWN))  py += 300 * dt;
    px = game_clamp(px, 0, game_width() - 40);
    py = game_clamp(py, 0, game_height() - 40);

    sy += 150 * dt;                     /* the star falls */
    if (sy > game_height()) {
        sx = (float)game_rand_int(20, game_width() - 20);
        sy = -20;
    }
    if (game_rect_overlap(game_make_rect(px, py, 40, 40), game_make_rect(sx - 10, sy - 10, 20, 20))) {
        score++;
        sy = game_height() + 1;         /* a new star next frame */
    }
    if (game_key_pressed(GFX_KEY_ESC)) game_quit();
}

static void draw(void *ud, gfx_surface *screen)
{
    (void)ud; (void)screen;
    game_gradient(0, 0, game_width(), game_height(), GFX_RGB(20, 30, 70), GFX_RGB(5, 5, 20));
    game_fill_rect((int)px, (int)py, 40, 40, GFX_RGB(60, 140, 240));
    game_fill_circle((int)sx, (int)sy, 10, GFX_YELLOW);
    game_printf(10, 10, GFX_WHITE, "Score %d", score);
}

int main(void)
{
    game_config c = game_default_config("My game");
    game_app a = { NULL, update, draw, NULL, NULL };   /* init update draw quit userdata */
    sx = (float)game_rand_int(20, 620);
    return game_run(&c, &a);
}
