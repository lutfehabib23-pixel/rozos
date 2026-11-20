/* rozOS — PS/2 কিবোর্ড ড্রাইভার (IRQ1, scancode set 1, US QWERTY)
 *
 * IRQ হ্যান্ডলার শুধু scancode অনুবাদ করে একটা রিং বাফারে রাখে — স্ক্রিনে কিছু
 * লেখে না। পড়ার দিক (keyboard_getkey) সাধারণ কোড থেকে ডাকা হয়।
 * এক লেখক (IRQ) ও এক পাঠক — তাই head/tail volatile হলেই যথেষ্ট। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/sched.h>
#include <rozos/keyboard.h>
#include <rozos/ps2.h>
#include <arch/x86_64/pic.h>
#include <arch/x86_64/io.h>
#include <arch/x86_64/cpu.h>

#define KBD_BUFFER_SIZE 256     /* 2-এর ঘাত হতে হবে */
#define KBD_CMD_SET_LEDS 0xED
#define KBD_CMD_ENABLE_SCANNING 0xF4

/* ---------- scancode set 1 → ASCII ---------- */

static const char map_normal[0x59] = {
    0,    27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,   /* 0x1D LCtrl */
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,   /* 0x2A LShift */
    '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',
    0,   /* 0x36 RShift */
    '*', /* 0x37 keypad * */
    0,   /* 0x38 LAlt */
    ' ',
    0,   /* 0x3A CapsLock */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,        /* F1-F10 */
    0,   /* 0x45 NumLock */
    0,   /* 0x46 ScrollLock */
    '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.',   /* keypad 0x47-0x53 */
    0, 0, 0,                             /* 0x54-0x56 */
    0, 0                                 /* F11, F12 */
};

static const char map_shift[0x59] = {
    0,    27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,
    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',
    0,
    '*',
    0,
    ' ',
    0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0,
    0,
    '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.',
    0, 0, 0,
    0, 0
};

/* NumLock বন্ধ থাকলে keypad-এর নেভিগেশন কী (0x47-0x53) */
static const int keypad_nav[13] = {
    KEY_HOME, KEY_UP, KEY_PAGE_UP, '-', KEY_LEFT, 0, KEY_RIGHT, '+',
    KEY_END, KEY_DOWN, KEY_PAGE_DOWN, KEY_INSERT, KEY_DELETE
};

/* ---------- অবস্থা ---------- */

static volatile int key_buffer[KBD_BUFFER_SIZE];
static volatile uint32_t buf_head, buf_tail;
static volatile uint8_t mods = KEYMOD_NUM;     /* NumLock ডিফল্ট চালু */
static volatile uint8_t last_scancode;
static int extended;                           /* আগের বাইট 0xE0 ছিল */
static int pause_skip;                         /* Pause কী-র 0xE1 সিকোয়েন্সের বাকি বাইট */
static int lshift, rshift, lctrl, rctrl, lalt, ralt;

static void push_key(int key)
{
    uint32_t next = (buf_head + 1) & (KBD_BUFFER_SIZE - 1);
    if (next == buf_tail) {
        return;                                /* বাফার পূর্ণ — কী হারাবে */
    }
    key_buffer[buf_head] = key;
    buf_head = next;
}

static void update_leds(void)
{
    uint8_t leds = 0;
    if (mods & KEYMOD_SCROLL) leds |= 0x01;
    if (mods & KEYMOD_NUM)    leds |= 0x02;
    if (mods & KEYMOD_CAPS)   leds |= 0x04;
    /* ACK (0xFA) পরে IRQ হয়ে আসবে, হ্যান্ডলার সেটা উপেক্ষা করে */
    ps2_write_data(KBD_CMD_SET_LEDS);
    ps2_write_data(leds);
}

/* init-এর সময় (ইন্টারাপ্ট বন্ধ): ACK নিজেই পড়ে নেয়, যাতে পরে মাউস ইনিশিয়ালাইজ
 * করার সময় কিবোর্ডের পুরনো ACK ভুল করে মাউসের উত্তর হিসেবে পড়া না হয় */
static void update_leds_sync(void)
{
    uint8_t leds = 0, ack;
    if (mods & KEYMOD_SCROLL) leds |= 0x01;
    if (mods & KEYMOD_NUM)    leds |= 0x02;
    if (mods & KEYMOD_CAPS)   leds |= 0x04;
    ps2_write_data(KBD_CMD_SET_LEDS);
    ps2_read_data(&ack);
    ps2_write_data(leds);
    ps2_read_data(&ack);
}

static void refresh_modifiers(void)
{
    uint8_t m = mods & (KEYMOD_CAPS | KEYMOD_NUM | KEYMOD_SCROLL);
    if (lshift || rshift) m |= KEYMOD_SHIFT;
    if (lctrl || rctrl)   m |= KEYMOD_CTRL;
    if (lalt || ralt)     m |= KEYMOD_ALT;
    mods = m;
}

static void handle_extended(uint8_t code, int released)
{
    int key = 0;

    switch (code) {
    case 0x1D: rctrl = !released; refresh_modifiers(); return;
    case 0x38: ralt  = !released; refresh_modifiers(); return;
    case 0x2A: case 0x36: return;              /* PrintScreen-এর ভুয়া shift */
    default: break;
    }
    if (released) {
        return;
    }
    switch (code) {
    case 0x1C: key = '\n'; break;              /* keypad Enter */
    case 0x35: key = '/'; break;               /* keypad / */
    case 0x47: key = KEY_HOME; break;
    case 0x48: key = KEY_UP; break;
    case 0x49: key = KEY_PAGE_UP; break;
    case 0x4B: key = KEY_LEFT; break;
    case 0x4D: key = KEY_RIGHT; break;
    case 0x4F: key = KEY_END; break;
    case 0x50: key = KEY_DOWN; break;
    case 0x51: key = KEY_PAGE_DOWN; break;
    case 0x52: key = KEY_INSERT; break;
    case 0x53: key = KEY_DELETE; break;
    default: break;
    }
    if (key) {
        push_key(key);
    }
}

static void handle_scancode(uint8_t sc)
{
    int released, key;
    uint8_t code;
    char c;

    last_scancode = sc;

    if (sc == PS2_DEV_ACK || sc == PS2_DEV_RESEND || sc == 0x00 || sc == 0xFF) {
        return;                                /* ACK/ত্রুটি — কী নয় */
    }
    if (pause_skip) {
        pause_skip--;
        return;
    }
    if (sc == 0xE1) {
        pause_skip = 5;                        /* Pause: E1 1D 45 E1 9D C5 */
        return;
    }
    if (sc == 0xE0) {
        extended = 1;
        return;
    }

    released = (sc & 0x80) != 0;
    code = sc & 0x7F;

    if (extended) {
        extended = 0;
        handle_extended(code, released);
        return;
    }

    /* modifier */
    switch (code) {
    case 0x2A: lshift = !released; refresh_modifiers(); return;
    case 0x36: rshift = !released; refresh_modifiers(); return;
    case 0x1D: lctrl  = !released; refresh_modifiers(); return;
    case 0x38: lalt   = !released; refresh_modifiers(); return;
    default: break;
    }
    if (released) {
        return;
    }

    /* lock কী */
    if (code == 0x3A) { mods ^= KEYMOD_CAPS;   update_leds(); return; }
    if (code == 0x45) { mods ^= KEYMOD_NUM;    update_leds(); return; }
    if (code == 0x46) { mods ^= KEYMOD_SCROLL; update_leds(); return; }

    /* ফাংশন কী */
    if (code >= 0x3B && code <= 0x44) { push_key(KEY_F1 + (code - 0x3B)); return; }
    if (code == 0x57) { push_key(KEY_F11); return; }
    if (code == 0x58) { push_key(KEY_F12); return; }

    /* keypad, NumLock বন্ধ */
    if (code >= 0x47 && code <= 0x53 && !(mods & KEYMOD_NUM)) {
        key = keypad_nav[code - 0x47];
        if (key) {
            push_key(key);
        }
        return;
    }

    if (code >= sizeof(map_normal)) {
        return;
    }
    c = (mods & KEYMOD_SHIFT) ? map_shift[code] : map_normal[code];
    if (!c) {
        return;
    }
    /* CapsLock শুধু অক্ষরে প্রভাব ফেলে, আর Shift-এর সাথে উল্টে যায় */
    if ((mods & KEYMOD_CAPS) && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
        c ^= 0x20;
    }
    /* Ctrl + অক্ষর → কন্ট্রোল কোড 1-26 */
    if (mods & KEYMOD_CTRL) {
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 1);
        else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 1);
    }
    push_key((unsigned char)c);
}

static void keyboard_irq(uint8_t irq, struct interrupt_frame *frame)
{
    (void)irq;
    (void)frame;
    handle_scancode(inb(PS2_DATA_PORT));
}

int keyboard_init(void)
{
    uint8_t ack = 0;

    if (!ps2_present()) {
        return 0;
    }
    buf_head = buf_tail = 0;

    /* scanning চালু (কিছু ফার্মওয়্যার বন্ধ রেখে দেয়) */
    ps2_write_data(KBD_CMD_ENABLE_SCANNING);
    ps2_read_data(&ack);

    update_leds_sync();
    ps2_flush();

    irq_register(IRQ_KEYBOARD, keyboard_irq);
    ps2_write_config(ps2_read_config() | 0x01);   /* কন্ট্রোলারে IRQ1 চালু */
    return 1;
}

int keyboard_try_getkey(int *key)
{
    if (buf_tail == buf_head) {
        return 0;
    }
    *key = key_buffer[buf_tail];
    buf_tail = (buf_tail + 1) & (KBD_BUFFER_SIZE - 1);
    return 1;
}

int keyboard_getkey(void)
{
    int key;
    for (;;) {
        cpu_cli();
        if (keyboard_try_getkey(&key)) {
            cpu_sti();
            return key;
        }
        sched_relax();
    }
}

uint8_t keyboard_modifiers(void)
{
    return mods;
}

uint8_t keyboard_last_scancode(void)
{
    return last_scancode;
}

const char *keyboard_key_name(int key)
{
    static const char *const names[] = {
        "Up", "Down", "Left", "Right", "Home", "End", "PageUp", "PageDown",
        "Insert", "Delete", "F1", "F2", "F3", "F4", "F5", "F6",
        "F7", "F8", "F9", "F10", "F11", "F12"
    };
    if (key >= KEY_UP && key <= KEY_F12) {
        return names[key - KEY_UP];
    }
    return "?";
}
