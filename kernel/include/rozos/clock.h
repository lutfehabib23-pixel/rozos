/* rozOS — দেয়াল-ঘড়ির সময় (UTC)
 *
 * বুটের মুহূর্তের সময় আসে Limine-এর "date at boot" feature থেকে (RTC পড়া), তার সাথে
 * টাইমারের uptime যোগ করে এখনকার সময়। নিজস্ব RTC ড্রাইভার পরে যোগ হতে পারে। */
#ifndef ROZOS_CLOCK_H
#define ROZOS_CLOCK_H

#include <stdint.h>
#include <stddef.h>

struct clock_tm {
    int year, month, day;          /* month 1-12 */
    int hour, minute, second;
};

int64_t clock_unix_time(void);                  /* 0 = অজানা */
void    clock_to_tm(int64_t t, struct clock_tm *tm);
/* "2026-11-20 06:30:00 UTC" */
void    clock_format(int64_t t, char *buf, size_t size);

#endif
