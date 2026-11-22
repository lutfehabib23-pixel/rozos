/* rozOS — PS/2 কিবোর্ড (US QWERTY, scancode set 1)
 *
 * keyboard_getkey() যা ফেরত দেয়:
 *   0-127        : ASCII (Enter = '\n', Backspace = '\b', Tab = '\t', Esc = 27)
 *   Ctrl + অক্ষর : কন্ট্রোল কোড (Ctrl+C = 3, Ctrl+L = 12 ...)
 *   >= 0x100     : বিশেষ কী (নিচের KEY_*) — পরে টেক্সট এডিটর এগুলো ব্যবহার করবে */
#ifndef ROZOS_KEYBOARD_H
#define ROZOS_KEYBOARD_H

#include <stdint.h>

enum key_special {
    KEY_UP = 0x100,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_HOME,
    KEY_END,
    KEY_PAGE_UP,
    KEY_PAGE_DOWN,
    KEY_INSERT,
    KEY_DELETE,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12
};

/* ধাপ ১৮ক: কাঁচা কী-ঘটনা (/dev/keyev, গেমের জন্য) — চাপা আর ছাড়া দুটোই।
 * কোড = Shift ছাড়া "মূল" কী (ছোট হাতের অক্ষর, '1', ' ', '\n', 27, KEY_* ...)
 * আর নিচের modifier/lock কী — যেগুলো সাধারণ কী-বাফারে কখনো আসে না। */
enum key_raw_special {
    KEY_LSHIFT = 0x120, KEY_RSHIFT, KEY_LCTRL, KEY_RCTRL, KEY_LALT, KEY_RALT,
    KEY_CAPSLOCK, KEY_NUMLOCK, KEY_SCROLLLOCK
};

/* একটা কাঁচা ঘটনা (int32): বিট 0-15 কোড, 16-18 modifier (KEYMOD_SHIFT/CTRL/ALT),
 * বিট 29 = auto-repeat (চেপে রাখায় আবার আসা), বিট 30 = ছাড়া হয়েছে */
#define KEYEV_CODE(e)   ((e) & 0xFFFF)
#define KEYEV_REPEAT    0x20000000
#define KEYEV_RELEASE   0x40000000

/* modifier বিট */
#define KEYMOD_SHIFT  0x01
#define KEYMOD_CTRL   0x02
#define KEYMOD_ALT    0x04
#define KEYMOD_CAPS   0x08
#define KEYMOD_NUM    0x10
#define KEYMOD_SCROLL 0x20

int      keyboard_init(void);          /* PS/2 কন্ট্রোলার না থাকলে 0 */
int      keyboard_try_getkey(int *key); /* বাফারে কী থাকলে 1 */
int      keyboard_has_key(void);        /* ধাপ ১৩: না সরিয়ে দেখা (poll) */
void     keyboard_set_intr_hook(int (*fn)(void));   /* ধাপ ১৪: Ctrl+C চাপলেই (IRQ থেকে) */
int      keyboard_getkey(void);         /* কী আসা পর্যন্ত অপেক্ষা (ইন্টারাপ্ট চালু থাকতে হবে) */
uint8_t  keyboard_modifiers(void);
uint8_t  keyboard_last_scancode(void);  /* ডিবাগিং */
const char *keyboard_key_name(int key);
void     keyboard_inject_scancode(uint8_t sc);   /* ধাপ ১৫খ: USB কিবোর্ড থেকে */
int      keyboard_try_getraw(int *ev);  /* ধাপ ১৮ক: কাঁচা ঘটনা থাকলে 1 (কেউ পড়লে তবেই জমে) */
int      keyboard_has_raw(void);

#endif
