/* rozOS — 8042 PS/2 কন্ট্রোলার ড্রাইভার */

#include <stdint.h>
#include <rozos/ps2.h>
#include <arch/x86_64/io.h>

#define PS2_STATUS_OUTPUT_FULL 0x01
#define PS2_STATUS_INPUT_FULL  0x02

#define PS2_CMD_READ_CONFIG    0x20
#define PS2_CMD_WRITE_CONFIG   0x60
#define PS2_CMD_DISABLE_PORT2  0xA7
#define PS2_CMD_ENABLE_PORT2   0xA8
#define PS2_CMD_TEST_PORT2     0xA9
#define PS2_CMD_SELF_TEST      0xAA
#define PS2_CMD_TEST_PORT1     0xAB
#define PS2_CMD_DISABLE_PORT1  0xAD
#define PS2_CMD_ENABLE_PORT1   0xAE
#define PS2_CMD_WRITE_PORT2    0xD4
#define PS2_CMD_PULSE_RESET    0xFE

#define PS2_CFG_IRQ1           0x01
#define PS2_CFG_IRQ12          0x02
#define PS2_CFG_PORT2_CLOCK_OFF 0x20
#define PS2_CFG_TRANSLATION    0x40

#define PS2_TIMEOUT 100000

static int controller_present;
static int second_port;

int ps2_wait_write(void)
{
    unsigned i;
    for (i = 0; i < PS2_TIMEOUT; i++) {
        if (!(inb(PS2_STATUS_PORT) & PS2_STATUS_INPUT_FULL)) {
            return 1;
        }
    }
    return 0;
}

int ps2_wait_read(void)
{
    unsigned i;
    for (i = 0; i < PS2_TIMEOUT; i++) {
        if (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL) {
            return 1;
        }
    }
    return 0;
}

void ps2_write_command(uint8_t cmd)
{
    ps2_wait_write();
    outb(PS2_COMMAND_PORT, cmd);
}

int ps2_read_data(uint8_t *out)
{
    if (!ps2_wait_read()) {
        return 0;
    }
    *out = inb(PS2_DATA_PORT);
    return 1;
}

void ps2_write_data(uint8_t data)
{
    ps2_wait_write();
    outb(PS2_DATA_PORT, data);
}

void ps2_write_port2(uint8_t data)
{
    ps2_write_command(PS2_CMD_WRITE_PORT2);
    ps2_write_data(data);
}

void ps2_flush(void)
{
    unsigned i;
    for (i = 0; i < 64 && (inb(PS2_STATUS_PORT) & PS2_STATUS_OUTPUT_FULL); i++) {
        (void)inb(PS2_DATA_PORT);
    }
}

uint8_t ps2_read_config(void)
{
    uint8_t cfg = 0;
    ps2_write_command(PS2_CMD_READ_CONFIG);
    ps2_read_data(&cfg);
    return cfg;
}

void ps2_write_config(uint8_t cfg)
{
    ps2_write_command(PS2_CMD_WRITE_CONFIG);
    ps2_write_data(cfg);
}

int ps2_init(void)
{
    uint8_t cfg, res;

    controller_present = 0;
    second_port = 0;

    /* কন্ট্রোলার না থাকলে status পোর্ট সাধারণত 0xFF পড়ে */
    if (inb(PS2_STATUS_PORT) == 0xFF) {
        return 0;
    }

    /* ১. দুটো পোর্টই বন্ধ, পুরনো ডেটা ফেলে দেওয়া */
    ps2_write_command(PS2_CMD_DISABLE_PORT1);
    ps2_write_command(PS2_CMD_DISABLE_PORT2);
    ps2_flush();

    /* ২. কনফিগারেশন: ইন্টারাপ্ট বন্ধ রেখে পরীক্ষা চালানো */
    cfg = ps2_read_config();
    cfg &= (uint8_t)~(PS2_CFG_IRQ1 | PS2_CFG_IRQ12);
    ps2_write_config(cfg);

    /* ৩. কন্ট্রোলার self-test (0x55 = ঠিক আছে)। কিছু কন্ট্রোলার এতে রিসেট হয়,
     *    তাই পরে কনফিগারেশন আবার লেখা হয়। */
    ps2_write_command(PS2_CMD_SELF_TEST);
    if (!ps2_read_data(&res) || res != 0x55) {
        return 0;
    }
    ps2_write_config(cfg);

    /* ৪. দ্বিতীয় পোর্ট আছে কিনা: চালু করলে port2 clock-off বিট মুছে যাওয়ার কথা */
    ps2_write_command(PS2_CMD_ENABLE_PORT2);
    if (!(ps2_read_config() & PS2_CFG_PORT2_CLOCK_OFF)) {
        second_port = 1;
        ps2_write_command(PS2_CMD_DISABLE_PORT2);
    }

    /* ৫. পোর্ট টেস্ট (0x00 = ঠিক আছে) */
    ps2_write_command(PS2_CMD_TEST_PORT1);
    if (!ps2_read_data(&res) || res != 0x00) {
        return 0;
    }
    if (second_port) {
        ps2_write_command(PS2_CMD_TEST_PORT2);
        if (!ps2_read_data(&res) || res != 0x00) {
            second_port = 0;
        }
    }

    /* ৬. পোর্ট চালু; IRQ বিট কিবোর্ড/মাউস ড্রাইভার নিজেরা চালু করবে */
    ps2_write_command(PS2_CMD_ENABLE_PORT1);
    if (second_port) {
        ps2_write_command(PS2_CMD_ENABLE_PORT2);
    }
    cfg = ps2_read_config();
    cfg |= PS2_CFG_TRANSLATION;       /* scancode set 1 */
    ps2_write_config(cfg);
    ps2_flush();

    controller_present = 1;
    return 1;
}

int ps2_present(void)
{
    return controller_present;
}

int ps2_has_second_port(void)
{
    return second_port;
}

void ps2_reset_cpu(void)
{
    ps2_wait_write();
    outb(PS2_COMMAND_PORT, PS2_CMD_PULSE_RESET);
}
