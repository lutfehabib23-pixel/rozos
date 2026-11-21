/* rozOS — PS/2 মাউস ড্রাইভার
 *
 * প্যাকেট (৩ বাইট):
 *   বাইট ০: Y overflow | X overflow | Y sign | X sign | 1 | মাঝ | ডান | বাম
 *   বাইট ১: X নড়াচড়া (sign বাইট ০-এ)
 *   বাইট ২: Y নড়াচড়া (sign বাইট ০-এ; উপরে = ধনাত্মক)
 * বাইট ০-এর বিট ৩ সবসময় 1 — এটা দিয়ে প্যাকেটের শুরু মিলিয়ে নেওয়া হয়।
 *
 * ধাপ ১২: IntelliMouse (স্ক্রল-চাকা) — sample rate 200, 100, 80 পাঠিয়ে ID 3 পেলে
 * প্যাকেট ৪ বাইট, বাইট ৩ = চাকা (signed)। প্রতিটি প্যাকেট দুটো ঘটনা-তালিকায় যায়:
 * একটা কনসোলের (devfs → xterm মাউস-ভাষা), একটা /dev/mouse-এর। */

#include <stdint.h>
#include <stddef.h>
#include <rozos/mouse.h>
#include <rozos/ps2.h>
#include <arch/x86_64/pic.h>
#include <arch/x86_64/io.h>
#include <arch/x86_64/cpu.h>

#define MOUSE_CMD_SET_DEFAULTS   0xF6
#define MOUSE_CMD_ENABLE_STREAM  0xF4
#define MOUSE_CMD_SET_RATE       0xF3
#define MOUSE_CMD_GET_ID         0xF2
#define MOUSE_EVQ                64

static int present;
static volatile int32_t pos_x, pos_y;
static volatile uint8_t buttons;
static volatile uint64_t packet_count;
static int32_t max_x = 1023, max_y = 767;
static uint8_t packet[4];
static int packet_index;
static int packet_size = 3;
static int has_wheel;
/* ঘটনা-তালিকা: [0] কনসোল, [1] /dev/mouse */
static struct mouse_event evq[2][MOUSE_EVQ];
static volatile uint32_t evq_head[2], evq_tail[2];

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
    {
        struct mouse_event e;
        int q;
        e.x = pos_x;
        e.y = pos_y;
        e.dx = dx;
        e.dy = -dy;
        e.buttons = buttons;
        e.wheel = 0;
        if (packet_size == 4) {
            int8_t z = (int8_t)(packet[3] << 4) >> 4;   /* নিচের ৪ বিট, signed */
            e.wheel = z;
        }
        for (q = 0; q < 2; q++) {
            uint32_t next = (evq_head[q] + 1) % MOUSE_EVQ;
            if (next == evq_tail[q]) {
                evq_tail[q] = (evq_tail[q] + 1) % MOUSE_EVQ;   /* পূর্ণ: সবচেয়ে পুরোনোটা বাদ */
            }
            evq[q][evq_head[q]] = e;
            evq_head[q] = next;
        }
    }
}

/* ধাপ ১৫খ: USB মাউস — dx ডানে, dy নিচে (+), buttons বিট ০ বাঁ ১ ডান ২ মাঝ, wheel ওপরে − */
void mouse_inject(int dx, int dy, uint8_t btn, int wheel)
{
    uint64_t f = cpu_irq_save();
    packet[0] = (uint8_t)(0x08 | (btn & 7) | (dx < 0 ? 0x10 : 0) | (-dy < 0 ? 0x20 : 0));
    if (dx > 255) dx = 255;
    if (dx < -256) dx = -256;
    if (dy > 256) dy = 256;
    if (dy < -255) dy = -255;
    packet[1] = (uint8_t)(dx & 0xFF);
    packet[2] = (uint8_t)((-dy) & 0xFF);
    packet[3] = (uint8_t)(wheel & 0x0F);
    {
        int saved = packet_size;
        packet_size = 4;                         /* চাকার মান সবসময় নেওয়া */
        handle_packet();
        packet_size = saved;
    }
    packet_index = 0;
    cpu_irq_restore(f);
}

int mouse_poll_event(int queue, struct mouse_event *out)
{
    uint64_t flags;
    int got = 0;
    if (queue < 0 || queue > 1) return 0;
    flags = cpu_irq_save();
    if (evq_tail[queue] != evq_head[queue]) {
        *out = evq[queue][evq_tail[queue]];
        evq_tail[queue] = (evq_tail[queue] + 1) % MOUSE_EVQ;
        got = 1;
    }
    cpu_irq_restore(flags);
    return got;
}

int mouse_has_wheel(void) { return has_wheel; }

/* ধাপ ১৩: poll()-এর জন্য — queue-তে ঘটনা আছে কিনা (না সরিয়ে) */
int mouse_event_pending(int queue)
{
    if (queue < 0 || queue > 1) return 0;
    return evq_tail[queue] != evq_head[queue];
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
    if (packet_index == packet_size) {
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
    /* ধাপ ১২: চাকা চালু করার "জাদু ক্রম" */
    {
        static const uint8_t rates[3] = { 200, 100, 80 };
        uint8_t id = 0;
        int i, ok = 1;
        for (i = 0; i < 3 && ok; i++) {
            ok = mouse_command(MOUSE_CMD_SET_RATE) && mouse_command(rates[i]);
        }
        if (ok && mouse_command(MOUSE_CMD_GET_ID) && ps2_read_data(&id) && id == 3) {
            has_wheel = 1;
            packet_size = 4;
        }
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
