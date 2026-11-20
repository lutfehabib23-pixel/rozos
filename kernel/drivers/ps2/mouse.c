/* rozOS — PS/2 মাউস ড্রাইভার
 *
 * প্যাকেট (৩ বাইট):
 *   বাইট ০: Y overflow | X overflow | Y sign | X sign | 1 | মাঝ | ডান | বাম
 *   বাইট ১: X নড়াচড়া (sign বাইট ০-এ)
 *   বাইট ২: Y নড়াচড়া (sign বাইট ০-এ; উপরে = ধনাত্মক)
 * বাইট ০-এর বিট ৩ সবসময় 1 — এটা দিয়ে প্যাকেটের শুরু মিলিয়ে নেওয়া হয়। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/mouse.h>
#include <rozos/ps2.h>
#include <arch/x86_64/pic.h>
#include <arch/x86_64/io.h>
#include <arch/x86_64/cpu.h>

#define MOUSE_CMD_SET_DEFAULTS   0xF6
#define MOUSE_CMD_ENABLE_STREAM  0xF4

static int present;
static volatile int32_t pos_x, pos_y;
static volatile uint8_t buttons;
static volatile uint64_t packet_count;
static int32_t max_x = 1023, max_y = 767;
static uint8_t packet[3];
static int packet_index;

static int mouse_command(uint8_t cmd)
{
    uint8_t ack = 0;
    ps2_write_port2(cmd);
    return ps2_read_data(&ack) && ack == PS2_DEV_ACK;
}

static int32_t clamp(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void handle_packet(void)
{
    int32_t dx, dy;
    uint8_t b0 = packet[0];

    if (b0 & 0xC0) {
        return;                                    /* overflow — প্যাকেট বাদ */
    }
    dx = (int32_t)packet[1] - ((b0 & 0x10) ? 256 : 0);
    dy = (int32_t)packet[2] - ((b0 & 0x20) ? 256 : 0);

    pos_x = clamp(pos_x + dx, 0, max_x);
    pos_y = clamp(pos_y - dy, 0, max_y);           /* স্ক্রিনে Y নিচের দিকে বাড়ে */
    buttons = b0 & 0x07;
    packet_count++;
}

static void mouse_irq(uint8_t irq, struct interrupt_frame *frame)
{
    uint8_t data;
    (void)irq;
    (void)frame;

    data = inb(PS2_DATA_PORT);
    if (packet_index == 0 && !(data & 0x08)) {
        return;                                    /* সিঙ্ক হারিয়েছে — পরের শুরু খুঁজি */
    }
    packet[packet_index++] = data;
    if (packet_index == 3) {
        packet_index = 0;
        handle_packet();
    }
}

int mouse_init(void)
{
    present = 0;
    if (!ps2_present() || !ps2_has_second_port()) {
        return 0;
    }
    ps2_flush();                                   /* আগের কোনো বাইট পড়ে না থাকে */
    if (!mouse_command(MOUSE_CMD_SET_DEFAULTS)) {
        return 0;
    }
    if (!mouse_command(MOUSE_CMD_ENABLE_STREAM)) {
        return 0;
    }

    pos_x = max_x / 2;
    pos_y = max_y / 2;
    packet_index = 0;

    irq_register(IRQ_MOUSE, mouse_irq);
    ps2_write_config(ps2_read_config() | 0x02);    /* কন্ট্রোলারে IRQ12 চালু */
    present = 1;
    return 1;
}

int mouse_present(void)
{
    return present;
}

void mouse_set_bounds(int32_t width, int32_t height)
{
    uint64_t flags = cpu_irq_save();
    if (width > 0)  max_x = width - 1;
    if (height > 0) max_y = height - 1;
    if (packet_count == 0) {
        pos_x = max_x / 2;                         /* এখনো নড়েনি — নতুন স্ক্রিনের মাঝখানে */
        pos_y = max_y / 2;
    }
    pos_x = clamp(pos_x, 0, max_x);
    pos_y = clamp(pos_y, 0, max_y);
    cpu_irq_restore(flags);
}

void mouse_get_state(struct mouse_state *out)
{
    uint64_t flags = cpu_irq_save();
    out->x = pos_x;
    out->y = pos_y;
    out->buttons = buttons;
    out->packets = packet_count;
    cpu_irq_restore(flags);
}
