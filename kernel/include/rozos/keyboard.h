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

/* modifier বিট */
#define KEYMOD_SHIFT  0x01
#define KEYMOD_CTRL   0x02
#define KEYMOD_ALT    0x04
#define KEYMOD_CAPS   0x08
#define KEYMOD_NUM    0x10
#define KEYMOD_SCROLL 0x20

int      keyboard_init(void);          /* PS/2 কন্ট্রোলার না থাকলে 0 */
int      keyboard_try_getkey(int *key); /* বাফারে কী থাকলে 1 */
int      keyboard_getkey(void);         /* কী আসা পর্যন্ত অপেক্ষা (ইন্টারাপ্ট চালু থাকতে হবে) */
uint8_t  keyboard_modifiers(void);
uint8_t  keyboard_last_scancode(void);  /* ডিবাগিং */
const char *keyboard_key_name(int key);

#endif
