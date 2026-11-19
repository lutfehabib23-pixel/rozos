/* rozOS — 16550 UART serial পোর্ট ড্রাইভার (ডিবাগ লগের জন্য) */
#ifndef ROZOS_SERIAL_H
#define ROZOS_SERIAL_H

#include <stdint.h>
#include <stddef.h>

#define SERIAL_COM1 0x3F8
#define SERIAL_COM2 0x2F8

/* পোর্ট চালু করে; সফল হলে 1, পোর্ট না থাকলে/লুপব্যাক টেস্ট ব্যর্থ হলে 0 */
int  serial_init(uint16_t port);
int  serial_is_ready(void);
void serial_putc(char c);
void serial_write(const char *s, size_t n);
/* ইনপুট: ডেটা থাকলে 1 ফেরত দিয়ে *c-তে বসায় (non-blocking) */
int  serial_try_getc(char *c);

#endif
