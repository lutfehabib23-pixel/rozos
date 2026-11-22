/* rozOS — luagame (ধাপ ১৮খ): Lua 5.4 দিয়ে গেম লেখা — librozgame-এর Lua মডিউল "game"
 *
 *   luagame mygame.lua [args...]      (help luagame)
 *
 * স্ক্রিপ্টে `game` টেবিল আগে থেকেই আছে (require "game"-ও চলে):
 *   game.run{ title = "Hi", update = function(dt) ... end, draw = function() ... end }
 *
 * নিজে বানাতে:  cd /usr/src/games; tcc luagame.c -llua -lrozgame -lrozgfx -lm -o /tmp/luagame */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <rozgame.h>

static lua_State *GL;
static int ref_init = LUA_NOREF, ref_update = LUA_NOREF, ref_draw = LUA_NOREF, ref_quit = LUA_NOREF;
static int script_failed;

/* ================= সাহায্যকারী ================= */

static int traceback(lua_State *L)
{
    const char *msg = lua_tostring(L, 1);
    luaL_traceback(L, L, msg ? msg : "(error object is not a string)", 1);
    return 1;
}

/* registry-তে রাখা Lua ফাংশন ডাকা; ভুল হলে বার্তা দেখিয়ে গেম বন্ধ */
static int call_ref(int ref, int nargs)
{
    int base, r;
    if (ref == LUA_NOREF || script_failed) {
        lua_pop(GL, nargs);
        return 0;
    }
    base = lua_gettop(GL) - nargs;
    lua_pushcfunction(GL, traceback);
    lua_rawgeti(GL, LUA_REGISTRYINDEX, ref);
    lua_rotate(GL, base + 1, 2);                       /* traceback, fn, args… */
    r = lua_pcall(GL, nargs, 0, base + 1);
    if (r != LUA_OK) {
        fprintf(stderr, "luagame: %s\n", lua_tostring(GL, -1));
        lua_pop(GL, 1);
        script_failed = 1;
        game_quit();
    }
    lua_pop(GL, 1);                                    /* traceback */
    return r == LUA_OK;
}

/* কী: "left" "right" "up" "down" "space" "enter" "escape" "a" "1" "f1" "lshift" … বা সংখ্যা */
static int key_code(lua_State *L, int idx)
{
    static const struct { const char *n; int k; } names[] = {
        { "left", GFX_KEY_LEFT }, { "right", GFX_KEY_RIGHT }, { "up", GFX_KEY_UP }, { "down", GFX_KEY_DOWN },
        { "space", ' ' }, { "enter", GFX_KEY_ENTER }, { "return", GFX_KEY_ENTER }, { "escape", GFX_KEY_ESC },
        { "esc", GFX_KEY_ESC }, { "tab", GFX_KEY_TAB }, { "backspace", GFX_KEY_BACKSPACE },
        { "home", GFX_KEY_HOME }, { "end", GFX_KEY_END }, { "pageup", GFX_KEY_PAGE_UP },
        { "pagedown", GFX_KEY_PAGE_DOWN }, { "insert", GFX_KEY_INSERT }, { "delete", GFX_KEY_DELETE },
        { "lshift", GFX_KEY_LSHIFT }, { "rshift", GFX_KEY_RSHIFT }, { "lctrl", GFX_KEY_LCTRL },
        { "rctrl", GFX_KEY_RCTRL }, { "lalt", GFX_KEY_LALT }, { "ralt", GFX_KEY_RALT }
    };
    const char *s;
    size_t i, n;
    if (lua_type(L, idx) == LUA_TNUMBER) return (int)lua_tointeger(L, idx);
    s = luaL_checklstring(L, idx, &n);
    if (n == 1) return tolower((unsigned char)s[0]);
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(s, names[i].n)) return names[i].k;
    if ((s[0] == 'f' || s[0] == 'F') && atoi(s + 1) >= 1 && atoi(s + 1) <= 12) return GFX_KEY_F1 + atoi(s + 1) - 1;
    return luaL_error(L, "unknown key name '%s'", s);
}

static int button(lua_State *L, int idx)
{
    if (lua_isnoneornil(L, idx)) return GFX_BUTTON_LEFT;
    if (lua_type(L, idx) == LUA_TSTRING) {
        const char *s = lua_tostring(L, idx);
        if (!strcmp(s, "right")) return GFX_BUTTON_RIGHT;
        if (!strcmp(s, "middle")) return GFX_BUTTON_MIDDLE;
        return GFX_BUTTON_LEFT;
    }
    switch (lua_tointeger(L, idx)) {
    case 2: return GFX_BUTTON_RIGHT;
    case 3: return GFX_BUTTON_MIDDLE;
    default: return GFX_BUTTON_LEFT;
    }
}

static uint32_t color(lua_State *L, int idx, uint32_t def)
{
    if (lua_isnoneornil(L, idx)) return def;
    return (uint32_t)luaL_checkinteger(L, idx);
}

#define NUM(i)  ((float)luaL_checknumber(L, (i)))
#define INT(i)  ((int)luaL_checknumber(L, (i)))
#define OPTN(i, d) ((float)luaL_optnumber(L, (i), (d)))

/* ================= লুপ ================= */

static int cb_init(void *ud) { (void)ud; call_ref(ref_init, 0); return script_failed; }
static void cb_update(void *ud, float dt) { (void)ud; lua_pushnumber(GL, dt); call_ref(ref_update, 1); }
static void cb_draw(void *ud, gfx_surface *s) { (void)ud; (void)s; call_ref(ref_draw, 0); }
static void cb_quit(void *ud) { (void)ud; call_ref(ref_quit, 0); }

static int take_fn(lua_State *L, const char *field)
{
    int r = LUA_NOREF;
    lua_getfield(L, 1, field);
    if (lua_isfunction(L, -1)) r = luaL_ref(L, LUA_REGISTRYINDEX);
    else lua_pop(L, 1);
    return r;
}

static int l_run(lua_State *L)
{
    game_config c;
    game_app a;
    const char *scale;
    int r;
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "title");
    c = game_default_config(lua_isstring(L, -1) ? lua_tostring(L, -1) : "Lua game");
    /* title-এর স্ট্রিং টেবিলে থাকে (Lua-র GC সরাবে না যতক্ষণ টেবিল স্ট্যাকে) */
    lua_pop(L, 1);
    lua_getfield(L, 1, "width");  if (lua_isnumber(L, -1)) c.width = (int)lua_tointeger(L, -1);  lua_pop(L, 1);
    lua_getfield(L, 1, "height"); if (lua_isnumber(L, -1)) c.height = (int)lua_tointeger(L, -1); lua_pop(L, 1);
    lua_getfield(L, 1, "smooth"); c.smooth = lua_toboolean(L, -1); lua_pop(L, 1);
    lua_getfield(L, 1, "show_fps"); c.show_fps = lua_toboolean(L, -1); lua_pop(L, 1);
    lua_getfield(L, 1, "max_fps"); if (lua_isnumber(L, -1)) c.max_fps = (int)lua_tointeger(L, -1); lua_pop(L, 1);
    lua_getfield(L, 1, "scale");
    scale = lua_tostring(L, -1);
    if (scale) {
        if (!strcmp(scale, "integer")) c.scale = GAME_SCALE_INTEGER;
        else if (!strcmp(scale, "stretch")) c.scale = GAME_SCALE_STRETCH;
        else if (!strcmp(scale, "native")) c.scale = GAME_SCALE_NATIVE;
    }
    lua_pop(L, 1);
    ref_init = take_fn(L, "init");
    ref_update = take_fn(L, "update");
    ref_draw = take_fn(L, "draw");
    ref_quit = take_fn(L, "quit");
    lua_getfield(L, 1, "title");                        /* শিরোনাম স্ট্যাকে রেখে দেওয়া */
    if (lua_isstring(L, -1)) c.title = lua_tostring(L, -1);
    memset(&a, 0, sizeof(a));
    a.init = cb_init;
    a.update = cb_update;
    a.draw = cb_draw;
    a.quit = cb_quit;
    r = game_run(&c, &a);
    lua_pop(L, 1);
    lua_pushboolean(L, r == 0 && !script_failed);
    return 1;
}

static int l_quit(lua_State *L) { (void)L; game_quit(); return 0; }
static int l_width(lua_State *L) { lua_pushinteger(L, game_width()); return 1; }
static int l_height(lua_State *L) { lua_pushinteger(L, game_height()); return 1; }
static int l_time(lua_State *L) { lua_pushnumber(L, game_time()); return 1; }
static int l_fps(lua_State *L) { lua_pushnumber(L, game_fps()); return 1; }
static int l_focused(lua_State *L) { lua_pushboolean(L, game_focused()); return 1; }
static int l_screenshot(lua_State *L) { lua_pushboolean(L, game_screenshot(luaL_checkstring(L, 1)) == 0); return 1; }
static int l_set_title(lua_State *L) { game_set_title(luaL_checkstring(L, 1)); return 0; }

/* ================= ইনপুট ================= */

static int l_key_down(lua_State *L)     { lua_pushboolean(L, game_key_down(key_code(L, 1))); return 1; }
static int l_key_pressed(lua_State *L)  { lua_pushboolean(L, game_key_pressed(key_code(L, 1))); return 1; }
static int l_key_released(lua_State *L) { lua_pushboolean(L, game_key_released(key_code(L, 1))); return 1; }
static int l_key_typed(lua_State *L)    { lua_pushboolean(L, game_key_typed(key_code(L, 1))); return 1; }

static int l_text_input(lua_State *L)
{
    char b[64];
    game_text_input(b, sizeof(b));
    lua_pushstring(L, b);
    return 1;
}

static int l_mouse(lua_State *L)
{
    lua_pushinteger(L, game_mouse_x());
    lua_pushinteger(L, game_mouse_y());
    return 2;
}
static int l_mouse_down(lua_State *L)     { lua_pushboolean(L, game_mouse_down(button(L, 1))); return 1; }
static int l_mouse_pressed(lua_State *L)  { lua_pushboolean(L, game_mouse_pressed(button(L, 1))); return 1; }
static int l_mouse_released(lua_State *L) { lua_pushboolean(L, game_mouse_released(button(L, 1))); return 1; }
static int l_mouse_wheel(lua_State *L)    { lua_pushinteger(L, game_mouse_wheel()); return 1; }

/* ================= রং আর আঁকা ================= */

static int l_rgb(lua_State *L)
{
    int a = (int)luaL_optinteger(L, 4, 255);
    lua_pushinteger(L, (lua_Integer)GFX_RGBA(INT(1) & 255, INT(2) & 255, INT(3) & 255, a & 255));
    return 1;
}
static int l_hsv(lua_State *L) { lua_pushinteger(L, (lua_Integer)game_hsv(NUM(1), OPTN(2, 1), OPTN(3, 1))); return 1; }
static int l_clear(lua_State *L) { game_clear(color(L, 1, GFX_BLACK)); return 0; }
static int l_fill_rect(lua_State *L) { game_fill_rect(INT(1), INT(2), INT(3), INT(4), color(L, 5, GFX_WHITE)); return 0; }
static int l_rect(lua_State *L) { game_draw_rect(INT(1), INT(2), INT(3), INT(4), color(L, 5, GFX_WHITE)); return 0; }
static int l_circle(lua_State *L) { game_fill_circle(INT(1), INT(2), INT(3), color(L, 4, GFX_WHITE)); return 0; }
static int l_line(lua_State *L) { game_line(INT(1), INT(2), INT(3), INT(4), color(L, 5, GFX_WHITE)); return 0; }
static int l_gradient(lua_State *L) { game_gradient(INT(1), INT(2), INT(3), INT(4), color(L, 5, GFX_BLACK), color(L, 6, GFX_BLACK)); return 0; }
static int l_text(lua_State *L) { lua_pushinteger(L, game_text(INT(1), INT(2), luaL_checkstring(L, 3), color(L, 4, GFX_WHITE))); return 1; }
static int l_text_center(lua_State *L) { game_text_center(INT(1), luaL_checkstring(L, 2), color(L, 3, GFX_WHITE)); return 0; }
static int l_ttf(lua_State *L) { lua_pushinteger(L, game_ttf(NULL, NUM(1), INT(2), INT(3), luaL_checkstring(L, 4), color(L, 5, GFX_WHITE))); return 1; }
static int l_ttf_center(lua_State *L) { game_ttf_center(NULL, NUM(1), INT(2), luaL_checkstring(L, 3), color(L, 4, GFX_WHITE)); return 0; }
static int l_ttf_width(lua_State *L) { lua_pushinteger(L, game_ttf_width(NULL, NUM(1), luaL_checkstring(L, 2))); return 1; }

/* ================= ছবি ================= */

#define IMG_MT "rozgame.image"
#define SHEET_MT "rozgame.sheet"
#define MAP_MT "rozgame.tilemap"
#define CAM_MT "rozgame.camera"
#define PART_MT "rozgame.particles"

typedef struct { game_image *img; } limg;
typedef struct { game_sheet *sh; } lsheet;
typedef struct { game_tilemap *m; } lmap;
typedef struct { game_particles *ps; } lpart;

static int push_image(lua_State *L, game_image *img)
{
    limg *u;
    if (!img) { lua_pushnil(L); return 1; }
    u = (limg *)lua_newuserdatauv(L, sizeof(limg), 0);
    u->img = img;
    luaL_setmetatable(L, IMG_MT);
    return 1;
}

static game_image *check_image(lua_State *L, int i)
{
    limg *u = (limg *)luaL_checkudata(L, i, IMG_MT);
    if (!u->img) luaL_error(L, "image was freed");
    return u->img;
}

static int l_image(lua_State *L) { return push_image(L, game_image_load(game_data_path(luaL_checkstring(L, 1)))); }
static int l_image_new(lua_State *L) { return push_image(L, game_image_new(INT(1), INT(2))); }
static int img_gc(lua_State *L) { limg *u = (limg *)luaL_checkudata(L, 1, IMG_MT); game_image_free(u->img); u->img = NULL; return 0; }
static int img_size(lua_State *L) { game_image *i = check_image(L, 1); lua_pushinteger(L, i->w); lua_pushinteger(L, i->h); return 2; }

static int flags_at(lua_State *L, int i)
{
    return (lua_toboolean(L, i) ? GAME_FLIP_X : 0) | (lua_toboolean(L, i + 1) ? GAME_FLIP_Y : 0);
}

static int img_draw(lua_State *L)
{
    game_image *i = check_image(L, 1);
    if (lua_isnoneornil(L, 4)) game_draw_image(i, INT(2), INT(3));
    else game_draw_image_alpha(i, INT(2), INT(3), INT(4));
    return 0;
}
static int img_draw_part(lua_State *L) { game_draw_image_ex(check_image(L, 1), INT(2), INT(3), INT(4), INT(5), INT(6), INT(7), flags_at(L, 8)); return 0; }
static int img_draw_scaled(lua_State *L)
{
    game_image *i = check_image(L, 1);
    game_draw_image_scaled(i, 0, 0, i->w, i->h, INT(2), INT(3), INT(4), INT(5), flags_at(L, 6));
    return 0;
}
static int img_draw_rotated(lua_State *L) { game_draw_image_rotated(check_image(L, 1), INT(2), INT(3), NUM(4), OPTN(5, 1)); return 0; }
static int img_resize(lua_State *L) { return push_image(L, game_image_resize(check_image(L, 1), INT(2), INT(3), lua_toboolean(L, 4))); }
static int img_background(lua_State *L) { game_draw_background(check_image(L, 1)); return 0; }

/* sprite sheet */
static int l_sheet(lua_State *L)
{
    game_sheet *sh = game_sheet_load(game_data_path(luaL_checkstring(L, 1)), INT(2), INT(3));
    lsheet *u;
    if (!sh) { lua_pushnil(L); return 1; }
    u = (lsheet *)lua_newuserdatauv(L, sizeof(lsheet), 0);
    u->sh = sh;
    luaL_setmetatable(L, SHEET_MT);
    return 1;
}
static game_sheet *check_sheet(lua_State *L, int i) { return ((lsheet *)luaL_checkudata(L, i, SHEET_MT))->sh; }
static int sheet_gc(lua_State *L) { lsheet *u = (lsheet *)luaL_checkudata(L, 1, SHEET_MT); game_sheet_free(u->sh); u->sh = NULL; return 0; }
static int sheet_draw(lua_State *L)
{
    game_sheet *sh = check_sheet(L, 1);
    if (sh) game_draw_frame(sh, INT(2), INT(3), INT(4), flags_at(L, 5));
    return 0;
}
static int sheet_draw_scaled(lua_State *L)
{
    game_sheet *sh = check_sheet(L, 1);
    if (sh) game_draw_frame_scaled(sh, INT(2), INT(3), INT(4), INT(5), INT(6), flags_at(L, 7));
    return 0;
}
static int sheet_count(lua_State *L) { game_sheet *sh = check_sheet(L, 1); lua_pushinteger(L, sh ? sh->count : 0); return 1; }

/* ================= টাইল ম্যাপ ================= */

static game_tilemap *check_map(lua_State *L, int i)
{
    lmap *u = (lmap *)luaL_checkudata(L, i, MAP_MT);
    if (!u->m) luaL_error(L, "tilemap was freed");
    return u->m;
}
static int l_tilemap(lua_State *L)
{
    game_tilemap *m = game_tilemap_load(game_data_path(luaL_checkstring(L, 1)));
    lmap *u;
    if (!m) { lua_pushnil(L); return 1; }
    u = (lmap *)lua_newuserdatauv(L, sizeof(lmap), 0);
    u->m = m;
    luaL_setmetatable(L, MAP_MT);
    return 1;
}
static int map_gc(lua_State *L) { lmap *u = (lmap *)luaL_checkudata(L, 1, MAP_MT); game_tilemap_free(u->m); u->m = NULL; return 0; }
static int map_draw(lua_State *L) { game_tilemap_draw(check_map(L, 1), (int)luaL_optnumber(L, 2, 0), (int)luaL_optnumber(L, 3, 0)); return 0; }
static int map_size(lua_State *L)
{
    game_tilemap *m = check_map(L, 1);
    lua_pushinteger(L, m->cols); lua_pushinteger(L, m->rows);
    lua_pushinteger(L, m->tw); lua_pushinteger(L, m->th);
    return 4;
}
static int map_get(lua_State *L)
{
    char s[2];
    s[0] = (char)game_tilemap_get(check_map(L, 1), INT(2), INT(3));
    s[1] = 0;
    lua_pushstring(L, s);
    return 1;
}
static int map_set(lua_State *L) { game_tilemap_set(check_map(L, 1), INT(2), INT(3), luaL_checkstring(L, 4)[0]); return 0; }
static int map_solid_at(lua_State *L) { lua_pushboolean(L, game_tilemap_solid_at(check_map(L, 1), NUM(2), NUM(3))); return 1; }
static int map_edges(lua_State *L) { check_map(L, 1)->edges = INT(2); return 0; }
static int map_find(lua_State *L)
{
    int c, r, i = game_tilemap_find(check_map(L, 1), luaL_checkstring(L, 2)[0], (int)luaL_optinteger(L, 3, 0), &c, &r);
    if (i < 0) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, i); lua_pushinteger(L, c); lua_pushinteger(L, r);
    return 3;
}
static int map_move(lua_State *L)
{
    game_rect r = game_make_rect(NUM(2), NUM(3), NUM(4), NUM(5));
    int hit = game_tilemap_move(check_map(L, 1), &r, NUM(6), NUM(7));
    lua_pushnumber(L, r.x);
    lua_pushnumber(L, r.y);
    lua_pushinteger(L, hit);
    return 3;
}

/* ================= ক্যামেরা ================= */

static game_camera *check_cam(lua_State *L, int i) { return (game_camera *)luaL_checkudata(L, i, CAM_MT); }
static int l_camera(lua_State *L)
{
    game_camera *c = (game_camera *)lua_newuserdatauv(L, sizeof(game_camera), 0);
    game_camera_init(c);
    luaL_setmetatable(L, CAM_MT);
    return 1;
}
static int cam_bounds(lua_State *L) { game_camera_bounds(check_cam(L, 1), NUM(2), NUM(3), NUM(4), NUM(5)); return 0; }
static int cam_follow(lua_State *L) { game_camera_follow(check_cam(L, 1), NUM(2), NUM(3), OPTN(4, 0), OPTN(5, game_dt())); return 0; }
static int cam_shake(lua_State *L) { game_camera_shake(check_cam(L, 1), NUM(2), NUM(3)); return 0; }
static int cam_update(lua_State *L) { game_camera_update(check_cam(L, 1), NUM(2)); return 0; }
static int cam_view(lua_State *L)
{
    game_camera *c = check_cam(L, 1);
    lua_pushinteger(L, game_camera_view_x(c));
    lua_pushinteger(L, game_camera_view_y(c));
    return 2;
}

/* ================= কণা ================= */

static game_particles *check_part(lua_State *L, int i)
{
    lpart *u = (lpart *)luaL_checkudata(L, i, PART_MT);
    if (!u->ps) luaL_error(L, "particles were freed");
    return u->ps;
}
static int l_particles(lua_State *L)
{
    game_particles *ps = game_particles_new((int)luaL_optinteger(L, 1, 500));
    lpart *u;
    if (!ps) { lua_pushnil(L); return 1; }
    u = (lpart *)lua_newuserdatauv(L, sizeof(lpart), 0);
    u->ps = ps;
    luaL_setmetatable(L, PART_MT);
    return 1;
}
static int part_gc(lua_State *L) { lpart *u = (lpart *)luaL_checkudata(L, 1, PART_MT); game_particles_free(u->ps); u->ps = NULL; return 0; }

static float opt_field(lua_State *L, int t, const char *k, float d)
{
    float v = d;
    lua_getfield(L, t, k);
    if (lua_isnumber(L, -1)) v = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);
    return v;
}

/* ps:burst(x, y, n, {color=, color_end=, speed=, speed_min=, life=, size=, size_end=,
 *                    gravity=, drag=, angle=, spread=}) */
static int part_burst(lua_State *L)
{
    game_particles *ps = check_part(L, 1);
    game_emitter e = game_emitter_default(GFX_WHITE);
    if (lua_istable(L, 5)) {
        float life;
        lua_getfield(L, 5, "color");
        if (lua_isinteger(L, -1)) {
            uint32_t c = (uint32_t)lua_tointeger(L, -1);
            e.color_start = c | 0xFF000000u;
            e.color_end = c & 0xFFFFFF;
        }
        lua_pop(L, 1);
        lua_getfield(L, 5, "color_end");
        if (lua_isinteger(L, -1)) e.color_end = (uint32_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
        e.speed_max = opt_field(L, 5, "speed", e.speed_max);
        e.speed_min = opt_field(L, 5, "speed_min", e.speed_max * 0.25f);
        life = opt_field(L, 5, "life", 0);
        if (life > 0) { e.life_min = life * 0.6f; e.life_max = life; }
        e.size_start = opt_field(L, 5, "size", e.size_start);
        e.size_end = opt_field(L, 5, "size_end", e.size_end);
        e.gravity = opt_field(L, 5, "gravity", e.gravity);
        e.drag = opt_field(L, 5, "drag", e.drag);
        e.angle = opt_field(L, 5, "angle", e.angle);
        e.spread = opt_field(L, 5, "spread", e.spread);
    }
    game_particles_burst(ps, NUM(2), NUM(3), INT(4), &e);
    return 0;
}
static int part_update(lua_State *L) { game_particles_update(check_part(L, 1), NUM(2)); return 0; }
static int part_draw(lua_State *L) { game_particles_draw(check_part(L, 1), (int)luaL_optnumber(L, 2, 0), (int)luaL_optnumber(L, 3, 0)); return 0; }
static int part_alive(lua_State *L) { lua_pushinteger(L, game_particles_alive(check_part(L, 1))); return 1; }

/* ================= সংঘর্ষ আর সহায়ক ================= */

static int l_overlap(lua_State *L)
{
    lua_pushboolean(L, game_rect_overlap(game_make_rect(NUM(1), NUM(2), NUM(3), NUM(4)), game_make_rect(NUM(5), NUM(6), NUM(7), NUM(8))));
    return 1;
}
static int l_circle_overlap(lua_State *L) { lua_pushboolean(L, game_circle_overlap(NUM(1), NUM(2), NUM(3), NUM(4), NUM(5), NUM(6))); return 1; }
static int l_rand_int(lua_State *L) { lua_pushinteger(L, game_rand_int(INT(1), INT(2))); return 1; }
static int l_rand_float(lua_State *L) { lua_pushnumber(L, game_rand_float(OPTN(1, 0), OPTN(2, 1))); return 1; }
static int l_clamp(lua_State *L) { lua_pushnumber(L, game_clamp(NUM(1), NUM(2), NUM(3))); return 1; }
static int l_lerp(lua_State *L) { lua_pushnumber(L, game_lerp(NUM(1), NUM(2), NUM(3))); return 1; }
static int l_approach(lua_State *L) { lua_pushnumber(L, game_approach(NUM(1), NUM(2), NUM(3))); return 1; }
static int l_color_lerp(lua_State *L) { lua_pushinteger(L, (lua_Integer)game_color_lerp((uint32_t)luaL_checkinteger(L, 1), (uint32_t)luaL_checkinteger(L, 2), NUM(3))); return 1; }
static int l_score_load(lua_State *L) { lua_pushinteger(L, game_score_load(luaL_checkstring(L, 1))); return 1; }
static int l_score_save(lua_State *L) { lua_pushboolean(L, game_score_save(luaL_checkstring(L, 1), (long)luaL_checkinteger(L, 2))); return 1; }
static int l_data_path(lua_State *L) { lua_pushstring(L, game_data_path(luaL_checkstring(L, 1))); return 1; }

/* ================= মডিউল ================= */

static void new_mt(lua_State *L, const char *name, const luaL_Reg *methods, lua_CFunction gc)
{
    luaL_newmetatable(L, name);
    lua_newtable(L);
    luaL_setfuncs(L, methods, 0);
    lua_setfield(L, -2, "__index");
    if (gc) {
        lua_pushcfunction(L, gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1);
}

static int luaopen_game(lua_State *L)
{
    static const luaL_Reg fns[] = {
        { "run", l_run }, { "quit", l_quit }, { "width", l_width }, { "height", l_height },
        { "time", l_time }, { "fps", l_fps }, { "focused", l_focused }, { "screenshot", l_screenshot },
        { "set_title", l_set_title },
        { "key_down", l_key_down }, { "key_pressed", l_key_pressed }, { "key_released", l_key_released },
        { "key_typed", l_key_typed }, { "text_input", l_text_input },
        { "mouse", l_mouse }, { "mouse_down", l_mouse_down }, { "mouse_pressed", l_mouse_pressed },
        { "mouse_released", l_mouse_released }, { "mouse_wheel", l_mouse_wheel },
        { "rgb", l_rgb }, { "hsv", l_hsv }, { "clear", l_clear }, { "fill_rect", l_fill_rect }, { "rect", l_rect },
        { "circle", l_circle }, { "line", l_line }, { "gradient", l_gradient },
        { "text", l_text }, { "text_center", l_text_center }, { "ttf", l_ttf }, { "ttf_center", l_ttf_center },
        { "ttf_width", l_ttf_width },
        { "image", l_image }, { "image_new", l_image_new }, { "sheet", l_sheet }, { "tilemap", l_tilemap },
        { "camera", l_camera }, { "particles", l_particles },
        { "overlap", l_overlap }, { "circle_overlap", l_circle_overlap },
        { "rand_int", l_rand_int }, { "rand_float", l_rand_float }, { "clamp", l_clamp }, { "lerp", l_lerp },
        { "approach", l_approach }, { "color_lerp", l_color_lerp },
        { "score_load", l_score_load }, { "score_save", l_score_save }, { "data_path", l_data_path },
        { NULL, NULL }
    };
    static const luaL_Reg img_m[] = {
        { "draw", img_draw }, { "draw_part", img_draw_part }, { "draw_scaled", img_draw_scaled },
        { "draw_rotated", img_draw_rotated }, { "resize", img_resize }, { "size", img_size },
        { "background", img_background }, { "free", img_gc }, { NULL, NULL }
    };
    static const luaL_Reg sheet_m[] = {
        { "draw", sheet_draw }, { "draw_scaled", sheet_draw_scaled }, { "count", sheet_count }, { NULL, NULL }
    };
    static const luaL_Reg map_m[] = {
        { "draw", map_draw }, { "size", map_size }, { "get", map_get }, { "set", map_set },
        { "solid_at", map_solid_at }, { "find", map_find }, { "move", map_move }, { "edges", map_edges },
        { NULL, NULL }
    };
    static const luaL_Reg cam_m[] = {
        { "bounds", cam_bounds }, { "follow", cam_follow }, { "shake", cam_shake }, { "update", cam_update },
        { "view", cam_view }, { NULL, NULL }
    };
    static const luaL_Reg part_m[] = {
        { "burst", part_burst }, { "update", part_update }, { "draw", part_draw }, { "alive", part_alive },
        { NULL, NULL }
    };
    new_mt(L, IMG_MT, img_m, img_gc);
    new_mt(L, SHEET_MT, sheet_m, sheet_gc);
    new_mt(L, MAP_MT, map_m, map_gc);
    new_mt(L, CAM_MT, cam_m, NULL);
    new_mt(L, PART_MT, part_m, part_gc);
    luaL_newlib(L, fns);
    lua_pushinteger(L, GAME_HIT_LEFT);   lua_setfield(L, -2, "HIT_LEFT");
    lua_pushinteger(L, GAME_HIT_RIGHT);  lua_setfield(L, -2, "HIT_RIGHT");
    lua_pushinteger(L, GAME_HIT_TOP);    lua_setfield(L, -2, "HIT_TOP");
    lua_pushinteger(L, GAME_HIT_BOTTOM); lua_setfield(L, -2, "HIT_BOTTOM");
    lua_pushinteger(L, GAME_EDGE_LEFT);  lua_setfield(L, -2, "EDGE_LEFT");
    lua_pushinteger(L, GAME_EDGE_RIGHT); lua_setfield(L, -2, "EDGE_RIGHT");
    lua_pushinteger(L, GAME_EDGE_TOP);   lua_setfield(L, -2, "EDGE_TOP");
    lua_pushinteger(L, GAME_EDGE_BOTTOM); lua_setfield(L, -2, "EDGE_BOTTOM");
    return 1;
}

int main(int argc, char **argv)
{
    lua_State *L;
    int i, r;
    if (argc < 2) {
        fprintf(stderr, "usage: luagame SCRIPT.lua [args...]   (help luagame)\n");
        return 2;
    }
    L = luaL_newstate();
    if (!L) return 1;
    GL = L;
    luaL_openlibs(L);
    luaL_requiref(L, "game", luaopen_game, 1);       /* global `game` আর require "game" */
    lua_pop(L, 1);
    lua_createtable(L, argc, 0);                     /* arg টেবিল, lua-র মতো */
    for (i = 0; i < argc; i++) {
        lua_pushstring(L, argv[i]);
        lua_rawseti(L, -2, i - 1);
    }
    lua_setglobal(L, "arg");
    lua_pushcfunction(L, traceback);
    r = luaL_loadfile(L, game_data_path(argv[1]));
    if (r == LUA_OK) r = lua_pcall(L, 0, 0, -2);
    if (r != LUA_OK) {
        fprintf(stderr, "luagame: %s\n", lua_tostring(L, -1));
        lua_close(L);
        return 1;
    }
    lua_close(L);
    return script_failed ? 1 : 0;
}
