/* rozOS — দেয়াল-ঘড়ি (rozos/clock.h দেখুন) */

#include <stdint.h>
#include <rozos/clock.h>
#include <rozos/bootinfo.h>
#include <rozos/timer.h>
#include <rozos/printf.h>

int64_t clock_unix_time(void)
{
    int64_t boot = bootinfo()->boot_unix_time;
    if (boot <= 0) {
        return 0;
    }
    return boot + (int64_t)(timer_uptime_ms() / 1000);
}

/* Howard Hinnant-এর "days from civil"-এর উল্টো: 1970-01-01 থেকে দিন → তারিখ */
void clock_to_tm(int64_t t, struct clock_tm *tm)
{
    int64_t days = t / 86400, rem = t % 86400, era, doe, yoe, doy, mp, y;
    if (rem < 0) {
        rem += 86400;
        days--;
    }
    tm->hour = (int)(rem / 3600);
    tm->minute = (int)(rem % 3600 / 60);
    tm->second = (int)(rem % 60);

    days += 719468;
    era = (days >= 0 ? days : days - 146096) / 146097;
    doe = days - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    tm->day = (int)(doy - (153 * mp + 2) / 5 + 1);
    tm->month = (int)(mp < 10 ? mp + 3 : mp - 9);
    tm->year = (int)(y + (tm->month <= 2));
}

void clock_format(int64_t t, char *buf, size_t size)
{
    struct clock_tm tm;
    if (t <= 0) {
        ksnprintf(buf, size, "(unknown)");
        return;
    }
    clock_to_tm(t, &tm);
    ksnprintf(buf, size, "%04d-%02d-%02d %02d:%02d:%02d UTC",
              tm.year, tm.month, tm.day, tm.hour, tm.minute, tm.second);
}
