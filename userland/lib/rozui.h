/* rozOS — rozui: 3D ধাঁচের UI আর উইজেট (ধাপ ১৪, ১৪খ)
 *
 * থিম (ধাপ ১৪খ, ব্যবহারকারীর বাছাই): Ebony #555D50, Obsidian #0B1215, Black #000000,
 * Dark Charcoal #333333, Onyx #353839, Jet Black #343434, White, Grey #808080 — বোতাম আর
 * ফ্রেম উঁচু/বসানো 3D ধারে। সব রং এক জায়গায় (struct ui_theme, ui_theme_get())।
 *
 * দুই স্তর:
 *  ১) আঁকা (ধাপ ১৪ক): ui_bevel, ui_panel, ui_button ... — শুধু ছবি আঁকে।
 *  ২) উইজেট (ধাপ ১৪খ, "immediate mode"): প্রতিবার পর্দা আঁকার সময় উইজেট ডাকো; ক্লিক হলে
 *     ফাংশন নিজেই 1 ফেরত দেয় — আলাদা callback লাগে না:
 *
 *   #include <rozui.h>
 *   int main(void) {
 *       ui_ctx ui;
 *       char name[64] = "";
 *       int big = 0;
 *       if (gfx_init_window("Hello", 360, 200) < 0) return 1;
 *       ui_init(&ui);
 *       while (ui_run(&ui)) {                       // ঘটনার জন্য অপেক্ষা, তারপর নতুন ছবি
 *           gfx_clear(ui.s, ui_theme_get()->window_bg);
 *           ui_label(ui.s, 16, 20, "Your name:", ui_theme_get()->text);
 *           ui_do_textbox(&ui, 110, 16, 220, name, sizeof(name));
 *           ui_do_check(&ui, 16, 56, "Big letters", &big);
 *           if (ui_do_button(&ui, 16, 100, 100, 28, "Hello"))
 *               ui_msgbox("Hi", name[0] ? name : "nobody", UI_MB_OK | UI_MB_INFO);
 *           if (ui_do_button(&ui, 130, 100, 100, 28, "Quit")) ui_quit(&ui);
 *       }
 *       gfx_quit();
 *       return 0;
 *   }
 *   কম্পাইল: tcc hello.c -lrozui -lrozgfx -o hello
 *
 * আইকনে ছবি (ধাপ ১৪খ): ui_draw_icon(s, x, y, 32, "files", 'f') আগে খোঁজে
 * /usr/share/icons/files.png (.bmp .jpg .gif .tga) — থাকলে সেই ছবি (স্বচ্ছতাসহ, মাপমতো),
 * না থাকলে আঁকা চিহ্ন। ছবির নামের বদলে পুরো পথও দেওয়া যায় ("/home/me/x.png")।
 * লিংক: -lrozui -lrozgfx */
#ifndef ROZUI_H
#define ROZUI_H

#include <rozgfx.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ui_theme {
    uint32_t desktop;        /* ডেস্কটপের পটভূমি */
    uint32_t panel;          /* ওপরের প্যানেল */
    uint32_t taskbar;        /* নিচের taskbar */
    uint32_t line;           /* প্যানেলের সরু রেখা */
    uint32_t face;           /* বোতাম/ফ্রেমের মুখ */
    uint32_t face_hover;
    uint32_t face_down;
    uint32_t light;          /* 3D-র আলো-পড়া ধার */
    uint32_t light2;         /* ভেতরের আলো ধার */
    uint32_t dark;           /* ছায়া ধার */
    uint32_t dark2;          /* ভেতরের ছায়া */
    uint32_t text;
    uint32_t text_dim;
    uint32_t accent;         /* উজ্জ্বল চিহ্ন (লোগো, টিক) */
    uint32_t title_a1, title_a2;   /* সক্রিয় উইন্ডোর শিরোনাম (ঢাল) */
    uint32_t title_i1, title_i2;   /* নিষ্ক্রিয় */
    uint32_t close_hover;    /* [X]-এর ওপর মাউস */
    uint32_t window_bg;      /* উইন্ডোর ভেতরের ডিফল্ট */
    /* ধাপ ১৪খ */
    uint32_t select;         /* নির্বাচিত সারি/লেখার পটভূমি */
    uint32_t field;          /* লেখার ঘর, তালিকার ভেতর */
    uint32_t field_text;
    uint32_t menubar;        /* উইন্ডোর মেনু-ফিতা */
};

const struct ui_theme *ui_theme_get(void);

enum { UI_NORMAL = 0, UI_HOVER, UI_DOWN, UI_DISABLED };

/* ---------- আঁকা (শুধু ছবি) ---------- */
void ui_bevel(gfx_surface *s, int x, int y, int w, int h, int raised);      /* দুই স্তরের 3D ধার */
void ui_panel(gfx_surface *s, int x, int y, int w, int h, uint32_t face, int raised);
void ui_sunken(gfx_surface *s, int x, int y, int w, int h, uint32_t inside);
void ui_button(gfx_surface *s, int x, int y, int w, int h, const char *label, int state);
void ui_label(gfx_surface *s, int x, int y, const char *text, uint32_t color);
void ui_label_center(gfx_surface *s, int x, int y, int w, int h, const char *text, uint32_t color);
void ui_menu_item(gfx_surface *s, int x, int y, int w, int h, const char *label, int selected);
void ui_separator(gfx_surface *s, int x, int y, int w);
/* ছোট ছবির-চিহ্ন (16x16): 't' টার্মিনাল, 'g' গ্রাফিক্স, 'f' ফোল্ডার, 'p' প্রোগ্রাম,
 * 'x' বন্ধ, '_' ছোট করা, 'o' বড় করা, 'r' আগের মাপ, 's' সিস্টেম, 'n' নেটওয়ার্ক, 'e' এডিটর;
 * ধাপ ১৪খ: 'd' কাগজ (ফাইল), 'c' C সোর্স, 'i' ছবি, 'u' ওপরে (↑), 'b' পেছনে (←),
 * 'h' বাড়ি, 'k' নতুন ফোল্ডার, 'm' মুছুন, 'v' সেভ, 'y' চালাও (▶), 'w' কম্পাইল;
 * ধাপ ১৯ছ: 'l' লক করা ফোল্ডার (তালা) */
void ui_icon(gfx_surface *s, int x, int y, char kind, uint32_t color);
/* ধাপ ১৪খ: আইকন — ছবি (/usr/share/icons/<image>.png ইত্যাদি) থাকলে ছবি, না থাকলে
 * size ≥ 24 হলে 3D বাক্সে আঁকা চিহ্ন, ছোট হলে শুধু চিহ্ন। image NULL/"" = সরাসরি চিহ্ন */
void ui_draw_icon(gfx_surface *s, int x, int y, int size, const char *image, char kind);
gfx_surface *ui_icon_image(const char *image, int size);   /* ছবি (cache-এ); না পেলে NULL */
void ui_progress(gfx_surface *s, int x, int y, int w, int h, int percent);
void ui_groupbox(gfx_surface *s, int x, int y, int w, int h, const char *title);
void ui_statusbar(gfx_surface *s, int x, int y, int w, int h, const char *text);

/* ---------- উইজেট (ধাপ ১৪খ) ---------- */
#define UI_MENUBAR_H 22
#define UI_TEXTBOX_H 24

struct ui_menuitem {
    const char *label;       /* "-" = বিভাজক রেখা */
    const char *shortcut;    /* "Ctrl+S", "F5", NULL — দেখানো হয় আর চাপলেও কাজ করে */
    int id;                  /* > 0; ui_menubar এটাই ফেরত দেয় */
};
struct ui_menu {
    const char *title;
    const struct ui_menuitem *items;
    int n;
};

typedef struct ui_ctx {
    /* এই ফ্রেমের অবস্থা — পড়ো, বদলিও না */
    gfx_surface *s;          /* আঁকার জায়গা (= gfx_screen()) */
    int w, h;                /* উইন্ডোর মাপ */
    int mx, my, buttons;     /* মাউস (উইন্ডোর ভেতরের স্থানাঙ্ক) */
    int pressed, released;   /* বাঁ বোতাম এই ফ্রেমে চাপা/ছাড়া হল */
    int rpressed;            /* ডান বোতাম চাপা হল */
    int dclick;              /* ডাবল-ক্লিক */
    int wheel;               /* −1 ওপরে, +1 নিচে */
    int key, mods;           /* চাপা কী (0 = নেই); কোনো উইজেট নিলে 0 হয়ে যায় */
    int closing;             /* [X] চাপা হয়েছে (ui_ask_before_close চালু থাকলে) */
    int resized;             /* উইন্ডোর মাপ বদলেছে */
    int blink;               /* লেখার কার্সর এখন দেখা যাবে কিনা (৫০০ ms পরপর বদলায়) */
    long now;                /* ms */
    int tick_ms;             /* > 0 হলে ঘটনা না এলেও এতক্ষণ পরপর নতুন ফ্রেম */
    /* ভেতরের হিসাব */
    int id, hot, active, focus, focus_claimed, want_focus, block, auto_tick;
    int started, quit, ask_close, need_frame, had_input;
    gfx_event pending;
    int has_pending;
    long last_click, blink_at;
    int last_cx, last_cy;
    int tb_cur, tb_off, drag_off;
    const struct ui_menu *mb;
    int mb_n, mb_x, mb_y, mb_open, mb_item;
} ui_ctx;

void ui_init(ui_ctx *u);                 /* gfx_init / gfx_init_window-এর পরে */
int  ui_run(ui_ctx *u);                  /* আগের ফ্রেম দেখাও, ঘটনার অপেক্ষা; 0 = শেষ করো */
void ui_quit(ui_ctx *u);                 /* পরের ui_run 0 ফেরত দেবে */
void ui_redraw(ui_ctx *u);               /* ঘটনা ছাড়াই আরেকটা ফ্রেম */
/* [X] চাপলে সরাসরি শেষ না করে u->closing = 1 (যেমন "Save changes?" জিজ্ঞেস করতে);
 * তারপর নিজে ui_quit() ডাকো */
void ui_ask_before_close(ui_ctx *u, int on);

int ui_hover(ui_ctx *u, int x, int y, int w, int h);      /* মাউস এর ওপরে (মেনু খোলা থাকলে 0) */
int ui_clicked(ui_ctx *u, int x, int y, int w, int h);    /* এই ফ্রেমে এখানে বাঁ-ক্লিক */
int ui_do_button(ui_ctx *u, int x, int y, int w, int h, const char *label);
/* টুলবারের বোতাম: আইকন (ছবি বা চিহ্ন) + লেখা (label NULL হলে শুধু আইকন) */
int ui_do_tool(ui_ctx *u, int x, int y, int w, int h, const char *image, char kind, const char *label);
int ui_do_check(ui_ctx *u, int x, int y, const char *label, int *value);          /* বদলালে 1 */
int ui_do_radio(ui_ctx *u, int x, int y, const char *label, int *value, int mine);
/* এক লাইনের লেখার ঘর (উচ্চতা UI_TEXTBOX_H): 1 = বদলেছে, 2 = Enter চাপা হয়েছে */
int ui_do_textbox(ui_ctx *u, int x, int y, int w, char *buf, int size);
/* ধাপ ১৯ছ: secret-এর ঘর — লেখা দেখায় * , Ctrl+C কাজ করে না; ফেরত ui_do_textbox-এর মতো */
int ui_do_password(ui_ctx *u, int x, int y, int w, char *buf, int size);
void ui_focus_next(ui_ctx *u);           /* পরের লেখার ঘর/তালিকায় ফোকাস */
/* তালিকা: 1 = নির্বাচন বদলেছে, 2 = ডাবল-ক্লিক বা Enter */
int ui_do_list(ui_ctx *u, int x, int y, int w, int h, const char *const *items, int n, int *sel, int *top);
/* খাড়া স্ক্রলবার: pos 0..total-page; বদলালে 1 */
int ui_do_vscroll(ui_ctx *u, int x, int y, int w, int h, int *pos, int total, int page);
int ui_do_slider(ui_ctx *u, int x, int y, int w, int *value, int min, int max);
/* মেনু-ফিতা (উচ্চতা UI_MENUBAR_H) — ফ্রেমের শুরুতে ডাকো (অন্য উইজেটের আগে)।
 * বেছে নেওয়া (বা শর্টকাট চাপা) আইটেমের id ফেরত দেয়, নইলে 0। F10 = প্রথম মেনু খোলে */
int ui_menubar(ui_ctx *u, int x, int y, int w, const struct ui_menu *menus, int n);
int ui_shortcut_key(const char *shortcut);   /* "Ctrl+S" → 19, "F5" → GFX_KEY_F5 */

/* ---------- ডায়ালগ (ধাপ ১৪খ) — উত্তর না দেওয়া পর্যন্ত অপেক্ষা করে ---------- */
#define UI_MB_OK          0
#define UI_MB_OKCANCEL    1
#define UI_MB_YESNO       2
#define UI_MB_YESNOCANCEL 3
#define UI_MB_INFO     0x10
#define UI_MB_WARN     0x20
#define UI_MB_ERROR    0x40
#define UI_MB_QUESTION 0x80
enum { UI_CANCEL = 0, UI_OK = 1, UI_YES = 2, UI_NO = 3 };
int ui_msgbox(const char *title, const char *text, int type);
int ui_inputbox(const char *title, const char *prompt, char *buf, int size);   /* 1 = OK */
/* ধাপ ১৯ছ: secret চাওয়া (* দেখায়); confirm = 1 হলে দুবার লিখতে হয়। 1 = OK (buf-এ secret) */
int ui_passwordbox(const char *title, const char *prompt, char *buf, int size, int confirm);
/* ধাপ ১৯ছ: ডান-ক্লিকের মেনু (x, y-তে) — বাছাই করা id, বাইরে ক্লিক/Esc = 0।
 * id < 0 = ধূসর আইটেম (দেখায়, চাপা যায় না); label "-" = রেখা */
int ui_popup_menu(const struct ui_menuitem *items, int n, int x, int y);
/* ফাইল বাছাই: path-এ শুরুর ফোল্ডার বা ফাইল; ফেরত 1 হলে path = বাছাই করা ফাইল।
 * save = 1: নতুন নাম লেখা যায়, আগে থাকলে "Replace?" জিজ্ঞেস করে */
int ui_filedlg(const char *title, char *path, int size, int save);

/* ---------- সাহায্য (ধাপ ১৪খ) ---------- */
/* প্রোগ্রাম আলাদা চালানো (অপেক্ষা না করে); dir = কাজের ফোল্ডার (NULL = এখানেই) */
int  ui_launch(const char *const argv[], const char *dir);
/* ক্লিপবোর্ড — /tmp/.clipboard, rozterm-এর সাথে একই */
int  ui_clip_set(const char *text, int len);
char *ui_clip_get(int *len);             /* malloc করা (free করো); খালি হলে NULL */

#ifdef __cplusplus
}
#endif

#endif
