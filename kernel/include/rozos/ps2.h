/* rozOS — 8042 PS/2 কন্ট্রোলার (কিবোর্ড = প্রথম পোর্ট, মাউস = দ্বিতীয় পোর্ট)
 *
 * সব অপেক্ষায় timeout আছে, যাতে কন্ট্রোলার না থাকা (USB-only) মেশিনে কার্নেল আটকে
 * না যায়। কন্ট্রোলারের "translation" চালু রাখা হয়, ফলে কিবোর্ড scancode set 1 পাঠায়। */
#ifndef ROZOS_PS2_H
#define ROZOS_PS2_H

#include <stdint.h>

#define PS2_DATA_PORT    0x60
#define PS2_STATUS_PORT  0x64
#define PS2_COMMAND_PORT 0x64

#define PS2_DEV_ACK      0xFA
#define PS2_DEV_RESEND   0xFE

/* কন্ট্রোলার চালু করে; ফেরত দেয় 1 (কন্ট্রোলার আছে) অথবা 0 */
int  ps2_init(void);
int  ps2_present(void);
int  ps2_has_second_port(void);

int  ps2_wait_write(void);             /* input buffer খালি হওয়া পর্যন্ত; সফল হলে 1 */
int  ps2_wait_read(void);              /* output buffer-এ ডেটা আসা পর্যন্ত */
void ps2_write_command(uint8_t cmd);
int  ps2_read_data(uint8_t *out);      /* timeout হলে 0 */
void ps2_write_data(uint8_t data);     /* প্রথম পোর্টের ডিভাইসে */
void ps2_write_port2(uint8_t data);    /* দ্বিতীয় পোর্টের ডিভাইসে (0xD4 prefix) */
void ps2_flush(void);

/* কন্ট্রোলারের configuration byte পড়া/লেখা */
uint8_t ps2_read_config(void);
void    ps2_write_config(uint8_t cfg);

/* CPU রিসেট (কন্ট্রোলারের 0xFE কমান্ড) — kmon-এর reboot */
void ps2_reset_cpu(void);

#endif
