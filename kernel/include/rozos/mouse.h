/* rozOS — PS/2 মাউস (IRQ12, ৩-বাইট প্যাকেট)
 *
 * অবস্থান স্ক্রিনের পিক্সেলে রাখা হয় (mouse_set_bounds)। এখন শুধু অবস্থা ট্র্যাক
 * করা হয়; পরে GUI/টেক্সট এডিটর কার্সর আঁকার সময় mouse_get_state() ব্যবহার করবে। */
#ifndef ROZOS_MOUSE_H
#define ROZOS_MOUSE_H

#include <stdint.h>

#define MOUSE_BUTTON_LEFT   0x01
#define MOUSE_BUTTON_RIGHT  0x02
#define MOUSE_BUTTON_MIDDLE 0x04

struct mouse_state {
    int32_t  x, y;          /* পিক্সেলে, (0,0) = উপরে-বামে */
    uint8_t  buttons;       /* MOUSE_BUTTON_* */
    uint64_t packets;       /* মোট প্যাকেট (ডিবাগিং) */
};

int  mouse_init(void);      /* দ্বিতীয় PS/2 পোর্ট না থাকলে বা সাড়া না দিলে 0 */
int  mouse_present(void);
void mouse_set_bounds(int32_t width, int32_t height);
void mouse_get_state(struct mouse_state *out);

#endif
