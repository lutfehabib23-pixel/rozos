/* rozOS — kpanic: বার্তা দেখিয়ে CPU চিরতরে থামায় */

#include <stdarg.h>
#include <rozos/panic.h>
#include <rozos/printf.h>
#include <rozos/console.h>
#include <arch/x86_64/cpu.h>

void kpanic(const char *fmt, ...)
{
    va_list ap;

    cpu_cli();
    tty_set_color(TTY_COLOR_WHITE, TTY_COLOR_RED);
    kprintf("\n*** KERNEL PANIC ***\n");
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    /* স্ক্রিনের VGA ফন্টে শুধু ASCII আছে, তাই কার্নেলের বার্তা ইংরেজিতে */
    kprintf("\nSystem halted.\n");
    cpu_halt_forever();
}
