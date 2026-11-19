/* rozOS — 16550 UART serial ড্রাইভার
 * QEMU-তে `-serial stdio` দিলে এই আউটপুট সরাসরি হোস্টের টার্মিনালে আসে।
 * ইন্টারাপ্ট ছাড়া (polling) কাজ করে, তাই IDT তৈরির আগেই ব্যবহার করা যায়। */

#include <rozos/serial.h>
#include <arch/x86_64/io.h>

/* UART রেজিস্টার offset */
#define UART_DATA        0   /* DLAB=0: data;  DLAB=1: divisor low  */
#define UART_IER         1   /* DLAB=0: interrupt enable; DLAB=1: divisor high */
#define UART_FCR         2   /* FIFO control */
#define UART_LCR         3   /* line control */
#define UART_MCR         4   /* modem control */
#define UART_LSR         5   /* line status */

#define LSR_DATA_READY   0x01
#define LSR_THR_EMPTY    0x20

static uint16_t serial_port;
static int      serial_ready;

int serial_init(uint16_t port)
{
    serial_port = port;
    serial_ready = 0;

    outb(port + UART_IER, 0x00);    /* সব ইন্টারাপ্ট বন্ধ */
    outb(port + UART_LCR, 0x80);    /* DLAB চালু — divisor সেট করার জন্য */
    outb(port + UART_DATA, 0x01);   /* divisor = 1 → 115200 baud */
    outb(port + UART_IER, 0x00);
    outb(port + UART_LCR, 0x03);    /* 8 bit, no parity, 1 stop bit; DLAB বন্ধ */
    outb(port + UART_FCR, 0xC7);    /* FIFO চালু, খালি করা, 14-byte threshold */
    outb(port + UART_MCR, 0x1E);    /* loopback মোডে টেস্ট */

    outb(port + UART_DATA, 0xAE);
    if (inb(port + UART_DATA) != 0xAE) {
        return 0;                   /* পোর্ট নেই বা ত্রুটিপূর্ণ */
    }

    outb(port + UART_MCR, 0x0F);    /* স্বাভাবিক মোড: DTR, RTS, OUT1, OUT2 */
    serial_ready = 1;
    return 1;
}

int serial_is_ready(void)
{
    return serial_ready;
}

static void serial_putc_raw(char c)
{
    unsigned spins = 100000;        /* হার্ডওয়্যার সাড়া না দিলে চিরতরে আটকে না থাকি */
    while (!(inb(serial_port + UART_LSR) & LSR_THR_EMPTY) && --spins) {
    }
    outb(serial_port + UART_DATA, (uint8_t)c);
}

void serial_putc(char c)
{
    if (!serial_ready) {
        return;
    }
    if (c == '\n') {
        serial_putc_raw('\r');      /* টার্মিনালের জন্য CRLF */
    }
    serial_putc_raw(c);
}

void serial_write(const char *s, size_t n)
{
    while (n--) {
        serial_putc(*s++);
    }
}

int serial_try_getc(char *c)
{
    if (!serial_ready || !(inb(serial_port + UART_LSR) & LSR_DATA_READY)) {
        return 0;
    }
    *c = (char)inb(serial_port + UART_DATA);
    return 1;
}
