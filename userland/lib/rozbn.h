/* rozOS — rozbn: অভ্র phonetic দিয়ে বাংলা লেখা (ধাপ ২৩গ)
 *
 *   rozbn_convert("ami", out, sizeof(out))   → "আমি"     (shikkha → শিক্ষা, sondhya → সন্ধ্যা)
 * নিয়ম হুবহু Avro Classic Phonetic-এর (libavrophonetic, MIT থেকে) — বড়/ছোট হাতের অক্ষর আলাদা
 * (T ট, t ত, S শ, Sh ষ, R ড়, O ও-কার ...)।
 *
 * টাইপ করার সময় (প্রোগ্রামে — librozui-র লেখার ঘর আর Code এটা ব্যবহার করে):
 *   Ctrl+Space বাংলা/ইংরেজি বদলায় — সব প্রোগ্রামে একসাথে (/tmp/.rozbn-mode);
 *   টাস্কবারের ডানে "বা" / "EN" দেখায়, ওতে ক্লিক করেও বদলানো যায়। (অভ্রের F12 নয় —
 *   Code-এ F12 আগে থেকেই "Save as"।)
 *   বাংলা মোডে ইংরেজি অক্ষর একটা শব্দে জমে; প্রতি অক্ষরে পুরো শব্দ আবার বাংলা হয়
 *   (rozbn_key বলে দেয় কার্সরের আগের কত বাইট মুছে কী বসাতে হবে)। স্পেস, Enter বা অন্য চিহ্নে
 *   শব্দ শেষ। Backspace শব্দ চলাকালীন শেষ ইংরেজি অক্ষরটা মোছে।
 *
 * লিংক: -lrozui (librozui.a-র ভেতরে)। */
#ifndef ROZBN_H
#define ROZBN_H

#include <stddef.h>

/* রোমান লেখা → বাংলা (UTF-8)। ফেরত লেখা বাইট (শেষের '\0' ছাড়া) */
int rozbn_convert(const char *roman, char *out, size_t size);

int  rozbn_mode(void);              /* 1 = বাংলা মোড */
void rozbn_set_mode(int on);
int  rozbn_toggle(void);            /* বদলে নতুন মোড ফেরত দেয় */
int  rozbn_is_toggle_key(int key, int mods);  /* Ctrl+Space (mods-এ GFX_MOD_CTRL) */

typedef struct rozbn_comp {
    char roman[64];                 /* এখনকার শব্দের ইংরেজি অক্ষর */
    int  rlen;
    int  out_len;                   /* সেই শব্দের বাংলা যত বাইট লেখায় বসানো আছে */
} rozbn_comp;

void rozbn_reset(rozbn_comp *c);
/* বাংলা মোডে key সামলানো: 1 = সামলানো হয়েছে — কার্সরের আগের *del বাইট মুছে ins বসাও;
 * 0 = সাধারণভাবে বসাও (শব্দ শেষ হয়ে গেছে, c খালি) */
int  rozbn_key(rozbn_comp *c, int key, int *del, char *ins, size_t size);

#endif
