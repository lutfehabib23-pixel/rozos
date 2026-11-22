/* rozlibc — সময়ের ফাংশন (ধাপ ১৬ক; আগে misc.c-তে শুধু UTC ছিল)
 *
 * সময় অঞ্চল: TZ পরিবেশ-চলক, না থাকলে /etc/TZ ফাইলের প্রথম লাইন, তাও না থাকলে UTC।
 * মান হতে পারে POSIX রূপে ("<+06>-6", "EST5EDT,M3.2.0,M11.1.0", "IST-5:30") বা কয়েকটা
 * চেনা নামে ("Asia/Dhaka", "Europe/London" …) — নামগুলো ভেতরের ছোট তালিকা থেকে POSIX
 * রূপে বদলে যায় (rozOS-এ tzdata নেই)। গ্রীষ্মকালীন সময় (DST) M/J/n নিয়মে হিসাব হয়।
 *
 * strftime-এর সব C99/POSIX রূপান্তর (+ GNU-র %k %l %P %s ও - _ 0 ^ # ফ্ল্যাগ), strptime,
 * timegm, localtime_r, asctime, ctime_r, clock_getres, clock_nanosleep, timespec_get। */

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

/* ---------- দিন-তারিখের গণিত (Howard Hinnant-এর civil দিন-গণনা) ---------- */

static int is_leap(long y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
static const int ydays[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };

/* ১৯৭০-০১-০১ থেকে y-m-d (m ১..১২) পর্যন্ত দিন */
static long days_from_civil(long y, int m, int d)
{
    long era, yoe, doy, doe;
    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(long z, long *y, int *m, int *d)
{
    long era, doe, yoe, doy, mp;
    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    *y = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y += *m <= 2;
}

struct tm *gmtime_r(const time_t *t, struct tm *tm)
{
    long days = *t / 86400, rem = *t % 86400, y;
    int m, d;
    if (rem < 0) { rem += 86400; days--; }
    tm->tm_hour = (int)(rem / 3600);
    tm->tm_min = (int)(rem % 3600 / 60);
    tm->tm_sec = (int)(rem % 60);
    tm->tm_wday = (int)((days + 4) % 7);
    if (tm->tm_wday < 0) tm->tm_wday += 7;
    civil_from_days(days, &y, &m, &d);
    tm->tm_year = (int)(y - 1900);
    tm->tm_mon = m - 1;
    tm->tm_mday = d;
    tm->tm_yday = ydays[m - 1] + d - 1 + (m > 2 && is_leap(y));
    tm->tm_isdst = 0;
    tm->tm_gmtoff = 0;
    tm->tm_zone = "UTC";
    return tm;
}

struct tm *gmtime(const time_t *t)
{
    static struct tm tm;
    return gmtime_r(t, &tm);
}

/* ক্ষেত্রগুলো সীমার বাইরে হলেও (যেমন tm_mday = 40) ঠিকঠাক যোগ করে UTC সেকেন্ড */
static time_t tm_to_utc(const struct tm *tm)
{
    long y = 1900L + tm->tm_year, mon = tm->tm_mon;
    long days;
    y += mon / 12;
    mon %= 12;
    if (mon < 0) { mon += 12; y--; }
    days = days_from_civil(y, (int)mon + 1, 1) + tm->tm_mday - 1;
    return (time_t)days * 86400 + (time_t)tm->tm_hour * 3600 + (time_t)tm->tm_min * 60 + tm->tm_sec;
}

time_t timegm(struct tm *tm)
{
    time_t t = tm_to_utc(tm);
    gmtime_r(&t, tm);
    return t;
}

/* ---------- সময় অঞ্চল ---------- */

struct tzrule {
    char type;                   /* 'M', 'J', 'D' */
    int m, w, d, n;
    long time;                   /* স্থানীয় সময়ে দিনের কত সেকেন্ডে */
};

static struct {
    char spec[64];               /* শেষ পার্স করা TZ মান */
    int loaded;
    long std_off, dst_off;       /* UTC-র কত সেকেন্ড পূর্বে (বাংলাদেশ = +21600) */
    char std_name[16], dst_name[16];
    int has_dst;
    struct tzrule start, end;
} tz;

char *tzname[2] = { (char *)"UTC", (char *)"UTC" };
long timezone;
int daylight;

/* ছোট তালিকা: tzdata না থাকায় সাধারণ কিছু অঞ্চলের নাম → POSIX রূপ */
static const char *const zone_table[][2] = {
    { "UTC", "UTC0" }, { "GMT", "GMT0" }, { "Etc/UTC", "UTC0" }, { "Universal", "UTC0" },
    { "Asia/Dhaka", "<+06>-6" }, { "Asia/Kolkata", "IST-5:30" }, { "Asia/Calcutta", "IST-5:30" },
    { "Asia/Kathmandu", "<+0545>-5:45" }, { "Asia/Karachi", "PKT-5" }, { "Asia/Thimphu", "<+06>-6" },
    { "Asia/Yangon", "<+0630>-6:30" }, { "Asia/Bangkok", "<+07>-7" }, { "Asia/Jakarta", "WIB-7" },
    { "Asia/Shanghai", "CST-8" }, { "Asia/Hong_Kong", "HKT-8" }, { "Asia/Singapore", "<+08>-8" },
    { "Asia/Kuala_Lumpur", "<+08>-8" }, { "Asia/Tokyo", "JST-9" }, { "Asia/Seoul", "KST-9" },
    { "Asia/Dubai", "<+04>-4" }, { "Asia/Riyadh", "<+03>-3" }, { "Asia/Tehran", "<+0330>-3:30" },
    { "Europe/London", "GMT0BST,M3.5.0/1,M10.5.0" }, { "Europe/Dublin", "IST-1GMT0,M10.5.0,M3.5.0/1" },
    { "Europe/Paris", "CET-1CEST,M3.5.0,M10.5.0/3" }, { "Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Rome", "CET-1CEST,M3.5.0,M10.5.0/3" }, { "Europe/Madrid", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Amsterdam", "CET-1CEST,M3.5.0,M10.5.0/3" }, { "Europe/Stockholm", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Athens", "EET-2EEST,M3.5.0/3,M10.5.0/4" }, { "Europe/Istanbul", "<+03>-3" },
    { "Europe/Moscow", "MSK-3" }, { "Africa/Cairo", "EET-2EEST,M4.5.5/0,M10.5.4/24" },
    { "Africa/Johannesburg", "SAST-2" }, { "Africa/Lagos", "WAT-1" }, { "Africa/Nairobi", "EAT-3" },
    { "America/New_York", "EST5EDT,M3.2.0,M11.1.0" }, { "America/Chicago", "CST6CDT,M3.2.0,M11.1.0" },
    { "America/Denver", "MST7MDT,M3.2.0,M11.1.0" }, { "America/Phoenix", "MST7" },
    { "America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0" }, { "America/Anchorage", "AKST9AKDT,M3.2.0,M11.1.0" },
    { "America/Toronto", "EST5EDT,M3.2.0,M11.1.0" }, { "America/Mexico_City", "CST6" },
    { "America/Sao_Paulo", "<-03>3" }, { "America/Argentina/Buenos_Aires", "<-03>3" },
    { "Pacific/Honolulu", "HST10" }, { "Pacific/Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3" },
    { "Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3" }, { "Australia/Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "Australia/Brisbane", "AEST-10" }, { "Australia/Perth", "AWST-8" },
    { "Australia/Adelaide", "ACST-9:30ACDT,M10.1.0,M4.1.0/3" },
};

static const char *tz_name(const char *p, char *out, size_t n)
{
    size_t k = 0;
    if (*p == '<') {
        p++;
        while (*p && *p != '>') { if (k + 1 < n) out[k++] = *p; p++; }
        if (*p == '>') p++;
    } else {
        while (isalpha((unsigned char)*p)) { if (k + 1 < n) out[k++] = *p; p++; }
    }
    out[k] = '\0';
    return p;
}

/* [+-]hh[:mm[:ss]] → সেকেন্ড; *ok = 0 হলে সংখ্যা ছিল না */
static const char *tz_hms(const char *p, long *out, int *ok)
{
    long sign = 1, h = 0, m = 0, s = 0;
    *ok = 0;
    if (*p == '+') p++;
    else if (*p == '-') { sign = -1; p++; }
    if (!isdigit((unsigned char)*p)) return p;
    *ok = 1;
    while (isdigit((unsigned char)*p)) h = h * 10 + (*p++ - '0');
    if (*p == ':') {
        p++;
        while (isdigit((unsigned char)*p)) m = m * 10 + (*p++ - '0');
        if (*p == ':') {
            p++;
            while (isdigit((unsigned char)*p)) s = s * 10 + (*p++ - '0');
        }
    }
    *out = sign * (h * 3600 + m * 60 + s);
    return p;
}

static const char *tz_rule(const char *p, struct tzrule *r)
{
    int ok;
    r->time = 7200;
    if (*p == 'M') {
        r->type = 'M';
        r->m = (int)strtol(p + 1, (char **)&p, 10);
        if (*p == '.') r->w = (int)strtol(p + 1, (char **)&p, 10);
        if (*p == '.') r->d = (int)strtol(p + 1, (char **)&p, 10);
    } else if (*p == 'J') {
        r->type = 'J';
        r->n = (int)strtol(p + 1, (char **)&p, 10);
    } else {
        r->type = 'D';
        r->n = (int)strtol(p, (char **)&p, 10);
    }
    if (*p == '/') p = tz_hms(p + 1, &r->time, &ok);
    return p;
}

static void tz_parse(const char *spec)
{
    const char *p = spec;
    long off;
    int ok;
    size_t i;
    tz.has_dst = 0;
    tz.std_off = tz.dst_off = 0;
    strcpy(tz.std_name, "UTC");
    tz.dst_name[0] = '\0';
    if (*p == ':') p++;
    for (i = 0; i < sizeof(zone_table) / sizeof(zone_table[0]); i++) {
        if (!strcmp(p, zone_table[i][0])) { p = zone_table[i][1]; break; }
    }
    if (!*p) goto done;
    p = tz_name(p, tz.std_name, sizeof(tz.std_name));
    if (!tz.std_name[0]) strcpy(tz.std_name, "UTC");
    p = tz_hms(p, &off, &ok);
    if (!ok) goto done;
    tz.std_off = -off;                       /* POSIX-এ ধনাত্মক = পশ্চিমে */
    if (!*p) goto done;
    p = tz_name(p, tz.dst_name, sizeof(tz.dst_name));
    if (!tz.dst_name[0]) goto done;
    tz.has_dst = 1;
    tz.dst_off = tz.std_off + 3600;
    if (*p && *p != ',') {
        p = tz_hms(p, &off, &ok);
        if (ok) tz.dst_off = -off;
    }
    if (*p == ',') {
        p = tz_rule(p + 1, &tz.start);
        if (*p == ',') tz_rule(p + 1, &tz.end);
    } else {
        /* নিয়ম দেওয়া নেই: আমেরিকার এখনকার নিয়ম (POSIX-এর ডিফল্ট নেই, glibc-ও তাই করে) */
        tz.start.type = 'M'; tz.start.m = 3; tz.start.w = 2; tz.start.d = 0; tz.start.time = 7200;
        tz.end.type = 'M'; tz.end.m = 11; tz.end.w = 1; tz.end.d = 0; tz.end.time = 7200;
    }
done:
    tzname[0] = tz.std_name;
    tzname[1] = tz.has_dst ? tz.dst_name : tz.std_name;
    timezone = -tz.std_off;
    daylight = tz.has_dst;
}

void tzset(void)
{
    const char *spec = getenv("TZ");
    char buf[64];
    if (!spec) {
        int fd = open("/etc/TZ", O_RDONLY, 0);
        buf[0] = '\0';
        if (fd >= 0) {
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n < 0) n = 0;
            buf[n] = '\0';
            buf[strcspn(buf, "\r\n \t")] = '\0';
        }
        spec = buf;
    }
    if (tz.loaded && !strcmp(spec, tz.spec)) return;
    strncpy(tz.spec, spec, sizeof(tz.spec) - 1);
    tz.spec[sizeof(tz.spec) - 1] = '\0';
    tz.loaded = 1;
    tz_parse(tz.spec);
}

/* বছরের শুরু থেকে (স্থানীয় সময়ে) নিয়মের মুহূর্ত, সেকেন্ডে */
static long rule_secs(long year, const struct tzrule *r)
{
    long day;
    if (r->type == 'J') {
        day = r->n - 1;
        if (is_leap(year) && r->n >= 60) day++;
    } else if (r->type == 'D') {
        day = r->n;
    } else {
        long first = days_from_civil(year, r->m, 1);
        int wd = (int)((first + 4) % 7), mlen;
        if (wd < 0) wd += 7;
        mlen = mdays[r->m - 1] + (r->m == 2 && is_leap(year));
        day = (r->d - wd + 7) % 7 + (r->w - 1) * 7;
        while (day >= mlen) day -= 7;
        day += ydays[r->m - 1] + (r->m > 2 && is_leap(year));
    }
    return day * 86400 + r->time;
}

static int is_dst_at(time_t t)
{
    long y, ys;
    int m, d;
    time_t start, end;
    if (!tz.has_dst) return 0;
    civil_from_days((long)((t + tz.std_off) / 86400 - ((t + tz.std_off) % 86400 < 0)), &y, &m, &d);
    ys = days_from_civil(y, 1, 1) * 86400L;
    start = ys + rule_secs(y, &tz.start) - tz.std_off;
    end = ys + rule_secs(y, &tz.end) - tz.dst_off;
    if (start < end) return t >= start && t < end;
    return !(t >= end && t < start);           /* দক্ষিণ গোলার্ধ */
}

struct tm *localtime_r(const time_t *t, struct tm *out)
{
    int dst;
    long off;
    time_t lt;
    tzset();
    dst = is_dst_at(*t);
    off = dst ? tz.dst_off : tz.std_off;
    lt = *t + off;
    gmtime_r(&lt, out);
    out->tm_isdst = dst;
    out->tm_gmtoff = off;
    out->tm_zone = dst ? tz.dst_name : tz.std_name;
    return out;
}

struct tm *localtime(const time_t *t)
{
    static struct tm tm;
    return localtime_r(t, &tm);
}

/* স্থানীয় সময় (সীমার বাইরের ক্ষেত্রও চলে) → time_t; tm-কে স্বাভাবিক করে দেয় */
time_t mktime(struct tm *tm)
{
    time_t tg, t;
    tzset();
    tg = tm_to_utc(tm);
    t = tg - tz.std_off;
    if (tz.has_dst) {
        if (tm->tm_isdst > 0) t = tg - tz.dst_off;
        else if (tm->tm_isdst < 0 && is_dst_at(tg - tz.dst_off)) t = tg - tz.dst_off;
    }
    localtime_r(&t, tm);
    return t;
}

double difftime(time_t a, time_t b) { return (double)(a - b); }

/* ---------- লেখা ---------- */

static const char *const wday_name[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const mon_name[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static const char *const wday_full[7] = { "Sunday", "Monday", "Tuesday", "Wednesday",
                                          "Thursday", "Friday", "Saturday" };
static const char *const mon_full[12] = { "January", "February", "March", "April", "May", "June",
                                          "July", "August", "September", "October", "November",
                                          "December" };

/* ISO 8601 সপ্তাহ: *year-এ ISO বছর, ফেরত সপ্তাহ (১..৫৩) */
static int iso_week(const struct tm *tm, long *year)
{
    long y = 1900L + tm->tm_year;
    int wday = (tm->tm_wday + 6) % 7;            /* সোমবার = ০ */
    int yday = tm->tm_yday;
    int wk = (yday - wday + 10) / 7;
    if (wk < 1) {
        long py = y - 1;
        int pdays = is_leap(py) ? 366 : 365;
        wk = (yday + pdays - wday + 10) / 7;
        y = py;
    } else {
        int days = is_leap(y) ? 366 : 365;
        if (yday - wday + 3 >= days) { wk = 1; y++; }
    }
    *year = y;
    return wk;
}

size_t strftime(char *buf, size_t size, const char *fmt, const struct tm *tm)
{
    size_t n = 0;
    char tmp[64];
    if (!size) return 0;
    for (; *fmt; fmt++) {
        const char *add = tmp;
        long num = 0;
        int numw = 0, isnum = 0, padc = '0', flag = 0, width = -1, upper = 0, len;
        if (*fmt != '%') {
            if (n + 1 >= size) return 0;
            buf[n++] = *fmt;
            continue;
        }
        fmt++;
        /* GNU ফ্ল্যাগ ও প্রস্থ: %-d %_d %0d %^a %#a %10Y */
        while (*fmt == '-' || *fmt == '_' || *fmt == '0' || *fmt == '^' || *fmt == '#') {
            if (*fmt == '^') upper = 1;
            else if (*fmt != '#') flag = *fmt;
            fmt++;
        }
        if (isdigit((unsigned char)*fmt)) {
            width = 0;
            while (isdigit((unsigned char)*fmt)) width = width * 10 + (*fmt++ - '0');
        }
        if (*fmt == 'E' || *fmt == 'O') fmt++;          /* বিকল্প রূপ: C locale-এ একই */
        tmp[0] = '\0';
        switch (*fmt) {
        case 'Y': num = 1900L + tm->tm_year; numw = 1; isnum = 1; break;
        case 'C': num = (1900L + tm->tm_year) / 100; numw = 2; isnum = 1; break;
        case 'y': num = ((tm->tm_year % 100) + 100) % 100; numw = 2; isnum = 1; break;
        case 'G': { long y; iso_week(tm, &y); num = y; numw = 1; isnum = 1; break; }
        case 'g': { long y; iso_week(tm, &y); num = ((y % 100) + 100) % 100; numw = 2; isnum = 1; break; }
        case 'V': { long y; num = iso_week(tm, &y); numw = 2; isnum = 1; break; }
        case 'U': num = (tm->tm_yday + 7 - tm->tm_wday) / 7; numw = 2; isnum = 1; break;
        case 'W': num = (tm->tm_yday + 7 - (tm->tm_wday + 6) % 7) / 7; numw = 2; isnum = 1; break;
        case 'm': num = tm->tm_mon + 1; numw = 2; isnum = 1; break;
        case 'd': num = tm->tm_mday; numw = 2; isnum = 1; break;
        case 'e': num = tm->tm_mday; numw = 2; isnum = 1; padc = ' '; break;
        case 'H': num = tm->tm_hour; numw = 2; isnum = 1; break;
        case 'k': num = tm->tm_hour; numw = 2; isnum = 1; padc = ' '; break;
        case 'I': num = tm->tm_hour % 12 ? tm->tm_hour % 12 : 12; numw = 2; isnum = 1; break;
        case 'l': num = tm->tm_hour % 12 ? tm->tm_hour % 12 : 12; numw = 2; isnum = 1; padc = ' '; break;
        case 'M': num = tm->tm_min; numw = 2; isnum = 1; break;
        case 'S': num = tm->tm_sec; numw = 2; isnum = 1; break;
        case 'j': num = tm->tm_yday + 1; numw = 3; isnum = 1; break;
        case 'u': num = tm->tm_wday ? tm->tm_wday : 7; numw = 1; isnum = 1; break;
        case 'w': num = tm->tm_wday; numw = 1; isnum = 1; break;
        case 's': {
            struct tm c = *tm;
            num = (long)mktime(&c);
            numw = 1;
            isnum = 1;
            break;
        }
        case 'a': add = wday_name[((tm->tm_wday % 7) + 7) % 7]; break;
        case 'A': add = wday_full[((tm->tm_wday % 7) + 7) % 7]; break;
        case 'b':
        case 'h': add = mon_name[((tm->tm_mon % 12) + 12) % 12]; break;
        case 'B': add = mon_full[((tm->tm_mon % 12) + 12) % 12]; break;
        case 'p': add = tm->tm_hour < 12 ? "AM" : "PM"; break;
        case 'P': add = tm->tm_hour < 12 ? "am" : "pm"; break;
        case 'c': strftime(tmp, sizeof(tmp), "%a %b %e %H:%M:%S %Y", tm); break;
        case 'x':
        case 'D': strftime(tmp, sizeof(tmp), "%m/%d/%y", tm); break;
        case 'X':
        case 'T': strftime(tmp, sizeof(tmp), "%H:%M:%S", tm); break;
        case 'F': strftime(tmp, sizeof(tmp), "%Y-%m-%d", tm); break;
        case 'R': strftime(tmp, sizeof(tmp), "%H:%M", tm); break;
        case 'r': strftime(tmp, sizeof(tmp), "%I:%M:%S %p", tm); break;
        case 'z': {
            long off = tm->tm_gmtoff, a = off < 0 ? -off : off;
            snprintf(tmp, sizeof(tmp), "%c%02ld%02ld", off < 0 ? '-' : '+', a / 3600, a % 3600 / 60);
            break;
        }
        case 'Z': add = tm->tm_zone ? tm->tm_zone : "UTC"; break;
        case 'n': add = "\n"; break;
        case 't': add = "\t"; break;
        case '%': add = "%"; break;
        case '\0': fmt--; add = "%"; break;
        default: tmp[0] = '%'; tmp[1] = *fmt; tmp[2] = '\0'; break;
        }
        if (isnum) {
            int neg = num < 0, w = width >= 0 ? width : numw;
            char digs[24];
            int dl = 0, k = 0;
            unsigned long u = neg ? (unsigned long)(-num) : (unsigned long)num;
            do { digs[dl++] = (char)('0' + u % 10); u /= 10; } while (u);
            if (flag == '-') w = 0;
            else if (flag == '_') padc = ' ';
            else if (flag == '0') padc = '0';
            if (neg) tmp[k++] = '-';
            while (dl + k < w && k < 40) tmp[k++] = (char)padc;
            while (dl) tmp[k++] = digs[--dl];
            tmp[k] = '\0';
            add = tmp;
        } else if (width > 0) {
            /* লেখার প্রস্থ: বাঁয়ে ফাঁকা (বা 0 ফ্ল্যাগে শূন্য) */
            char t2[64];
            int al = (int)strlen(add), k = 0;
            while (al + k < width && k < 40) t2[k++] = flag == '0' ? '0' : ' ';
            snprintf(t2 + k, sizeof(t2) - (size_t)k, "%s", add);
            strcpy(tmp, t2);
            add = tmp;
        }
        len = (int)strlen(add);
        if (n + (size_t)len >= size) return 0;
        while (*add) {
            char ch = *add++;
            if (upper && ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
            buf[n++] = ch;
        }
    }
    buf[n] = '\0';
    return n;
}

char *asctime_r(const struct tm *tm, char *buf)
{
    snprintf(buf, 26, "%.3s %.3s %2d %02d:%02d:%02d %d\n", wday_name[((tm->tm_wday % 7) + 7) % 7],
             mon_name[((tm->tm_mon % 12) + 12) % 12], tm->tm_mday, tm->tm_hour, tm->tm_min,
             tm->tm_sec, tm->tm_year + 1900);
    return buf;
}

char *asctime(const struct tm *tm)
{
    static char buf[32];
    return asctime_r(tm, buf);
}

char *ctime_r(const time_t *t, char *buf)
{
    struct tm tm;
    localtime_r(t, &tm);
    return asctime_r(&tm, buf);
}

char *ctime(const time_t *t)
{
    static char buf[32];
    return ctime_r(t, buf);
}

/* ---------- strptime ---------- */

static const char *match_name(const char *s, const char *const *names, int count, int *out)
{
    int i;
    for (i = 0; i < count; i++) {
        size_t n = strlen(names[i]);
        if (!strncasecmp(s, names[i], n)) { *out = i; return s + n; }
    }
    return NULL;
}

static const char *get_num(const char *s, int maxdig, long lo, long hi, int *out)
{
    long v = 0;
    int k = 0, neg = 0;
    if (*s == '-' || *s == '+') { neg = *s == '-'; s++; }
    while (k < maxdig && isdigit((unsigned char)*s)) { v = v * 10 + (*s++ - '0'); k++; }
    if (!k) return NULL;
    if (neg) v = -v;
    if (v < lo || v > hi) return NULL;
    *out = (int)v;
    return s;
}

char *strptime(const char *s, const char *fmt, struct tm *tm)
{
    int pm = -1, century = -1, yy = -1, v;
    for (; *fmt && s; fmt++) {
        if (isspace((unsigned char)*fmt)) {
            while (isspace((unsigned char)*s)) s++;
            continue;
        }
        if (*fmt != '%') {
            if (*s != *fmt) return NULL;
            s++;
            continue;
        }
        fmt++;
        if (*fmt == 'E' || *fmt == 'O') fmt++;
        switch (*fmt) {
        case 'Y': s = get_num(s, 4, -9999, 9999, &v); if (s) tm->tm_year = v - 1900; break;
        case 'C': s = get_num(s, 2, 0, 99, &century); break;
        case 'y': s = get_num(s, 2, 0, 99, &yy); break;
        case 'm': s = get_num(s, 2, 1, 12, &v); if (s) tm->tm_mon = v - 1; break;
        case 'd':
        case 'e': while (*s == ' ') s++; s = get_num(s, 2, 1, 31, &tm->tm_mday); break;
        case 'H':
        case 'k': while (*s == ' ') s++; s = get_num(s, 2, 0, 23, &tm->tm_hour); break;
        case 'I':
        case 'l': while (*s == ' ') s++; s = get_num(s, 2, 1, 12, &tm->tm_hour); break;
        case 'M': s = get_num(s, 2, 0, 59, &tm->tm_min); break;
        case 'S': s = get_num(s, 2, 0, 61, &tm->tm_sec); break;
        case 'j': s = get_num(s, 3, 1, 366, &v); if (s) tm->tm_yday = v - 1; break;
        case 'w': s = get_num(s, 1, 0, 6, &tm->tm_wday); break;
        case 'u': s = get_num(s, 1, 1, 7, &v); if (s) tm->tm_wday = v % 7; break;
        case 'a':
        case 'A': {
            const char *r = match_name(s, wday_full, 7, &tm->tm_wday);
            s = r ? r : match_name(s, wday_name, 7, &tm->tm_wday);
            break;
        }
        case 'b':
        case 'B':
        case 'h': {
            const char *r = match_name(s, mon_full, 12, &tm->tm_mon);
            s = r ? r : match_name(s, mon_name, 12, &tm->tm_mon);
            break;
        }
        case 'p':
        case 'P':
            if (!strncasecmp(s, "AM", 2)) { pm = 0; s += 2; }
            else if (!strncasecmp(s, "PM", 2)) { pm = 1; s += 2; }
            else s = NULL;
            break;
        case 'D': s = strptime(s, "%m/%d/%y", tm); break;
        case 'x': s = strptime(s, "%m/%d/%y", tm); break;
        case 'T':
        case 'X': s = strptime(s, "%H:%M:%S", tm); break;
        case 'R': s = strptime(s, "%H:%M", tm); break;
        case 'r': s = strptime(s, "%I:%M:%S %p", tm); break;
        case 'F': s = strptime(s, "%Y-%m-%d", tm); break;
        case 'c': s = strptime(s, "%a %b %e %H:%M:%S %Y", tm); break;
        case 's': {
            char *end;
            time_t t = (time_t)strtol(s, &end, 10);
            if (end == s) { s = NULL; break; }
            s = end;
            localtime_r(&t, tm);
            break;
        }
        case 'z': {
            int sign, hh, mm = 0;
            if (*s == 'Z') { tm->tm_gmtoff = 0; s++; break; }
            if (*s != '+' && *s != '-') { s = NULL; break; }
            sign = *s++ == '-' ? -1 : 1;
            s = get_num(s, 2, 0, 24, &hh);
            if (!s) break;
            if (*s == ':') s++;
            if (isdigit((unsigned char)*s)) s = get_num(s, 2, 0, 59, &mm);
            if (s) tm->tm_gmtoff = sign * (hh * 3600L + mm * 60L);
            break;
        }
        case 'Z':
            while (isalpha((unsigned char)*s)) s++;
            break;
        case 'n':
        case 't':
            while (isspace((unsigned char)*s)) s++;
            break;
        case '%':
            if (*s != '%') return NULL;
            s++;
            break;
        default:
            return NULL;
        }
    }
    if (!s) return NULL;
    if (yy >= 0) {
        if (century >= 0) tm->tm_year = century * 100 + yy - 1900;
        else tm->tm_year = yy < 69 ? yy + 100 : yy;        /* POSIX: ৬৯–৯৯ = ১৯xx */
    } else if (century >= 0) {
        tm->tm_year = century * 100 - 1900;
    }
    if (pm >= 0) {
        if (tm->tm_hour == 12) tm->tm_hour = 0;
        if (pm) tm->tm_hour += 12;
    }
    return (char *)s;
}

/* ---------- ঘড়ি ---------- */

int clock_getres(clockid_t clk, struct timespec *ts)
{
    (void)clk;
    if (ts) {
        ts->tv_sec = 0;
        ts->tv_nsec = 1000000;                  /* কার্নেলের টাইমার মিলিসেকেন্ডে */
    }
    return 0;
}

int clock_settime(clockid_t clk, const struct timespec *ts)
{
    (void)clk;
    (void)ts;
    errno = EPERM;                              /* RTC-তে লেখা এখনো নেই */
    return -1;
}

int clock_nanosleep(clockid_t clk, int flags, const struct timespec *req, struct timespec *rem)
{
    struct timespec now, d;
    if (!(flags & TIMER_ABSTIME)) return nanosleep(req, rem) < 0 ? errno : 0;
    if (clock_gettime(clk, &now) < 0) return errno;
    d.tv_sec = req->tv_sec - now.tv_sec;
    d.tv_nsec = req->tv_nsec - now.tv_nsec;
    if (d.tv_nsec < 0) { d.tv_nsec += 1000000000L; d.tv_sec--; }
    if (d.tv_sec < 0) return 0;
    return nanosleep(&d, NULL) < 0 ? errno : 0;
}

int timespec_get(struct timespec *ts, int base)
{
    if (base != TIME_UTC || clock_gettime(CLOCK_REALTIME, ts) < 0) return 0;
    return base;
}
