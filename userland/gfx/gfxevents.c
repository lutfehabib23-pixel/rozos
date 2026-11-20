/* rozOS — gfxevents: librozgfx-এর ইনপুট-ঘটনা লেখা হিসেবে দেখানো (ধাপ ১৩)
 *   পর্দা গ্রাফিক্স মোডে যায় না — শুধু gfx_wait_event যা দেয় তা ছাপে (কী, মাউস নড়া,
 *   বোতাম, চাকা)। নিজের গ্রাফিক্স প্রোগ্রামের ইনপুট বুঝতে কাজে লাগে। q = বের হওয়া,
 *   ৫ সেকেন্ড কিছু না এলেও শেষ। */

#include <stdio.h>
#include <rozgfx.h>

int main(void)
{
    static const char *names[] = { "none", "key", "mouse-move", "mouse-down", "mouse-up", "wheel" };
    gfx_event ev;
    printf("gfxevents: press keys, move/click the mouse (q quits)\n");
    while (gfx_wait_event(&ev, 5000)) {
        printf("%-10s key=%-4d x=%-4d y=%-4d dx=%-4d dy=%-4d buttons=%d button=%d wheel=%d\n",
               ev.type >= 0 && ev.type <= 5 ? names[ev.type] : "?", ev.key, ev.x, ev.y,
               ev.dx, ev.dy, ev.buttons, ev.button, ev.wheel);
        if (ev.type == GFX_EV_KEY && ev.key == 'q') break;
    }
    return 0;
}
