/* rozOS — কার্নেলের printf বাস্তবায়ন (heap ছাড়া, পোর্টেবল C) */

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <rozos/printf.h>
#include <rozos/string.h>
#include <rozos/console.h>

/* আউটপুট বাফার: শেষে '\0' নিশ্চিত, size ছাড়িয়ে গেলেও মোট দৈর্ঘ্য গোনা চলে।
 * flush থাকলে (kvprintf) বাফার ভরে গেলে লেখা সেখানে পাঠিয়ে বাফার খালি করা হয় —
 * ফলে যত লম্বা আউটপুটই হোক কাটা পড়ে না। ksnprintf-এ flush নেই (আগের মতো কাটে)। */
struct outbuf {
    char  *buf;
    size_t size;
    size_t len;        /* মোট (ফ্লাশ করা সহ) */
    size_t pos;        /* বাফারে এখন কত */
    void (*flush)(const char *s, size_t n);
};

static void out_char(struct outbuf *o, char c)
{
    if (o->flush) {
        if (o->pos + 1 >= o->size) {
            o->flush(o->buf, o->pos);
            o->pos = 0;
        }
        o->buf[o->pos++] = c;
    } else if (o->len + 1 < o->size) {
        o->buf[o->len] = c;
    }
    o->len++;
}

static void out_pad(struct outbuf *o, char c, int count)
{
    while (count-- > 0) {
        out_char(o, c);
    }
}

static void out_str(struct outbuf *o, const char *s, int width, int precision, int left)
{
    size_t n;
    int pad;

    if (!s) {
        s = "(null)";
    }
    n = (precision >= 0) ? strnlen(s, (size_t)precision) : strlen(s);
    pad = width - (int)n;

    if (!left) {
        out_pad(o, ' ', pad);
    }
    while (n--) {
        out_char(o, *s++);
    }
    if (left) {
        out_pad(o, ' ', pad);
    }
}

static void out_num(struct outbuf *o, uint64_t value, int is_negative, unsigned base,
                    int upper, int width, int zero_pad, int left, const char *prefix)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    int n = 0;
    int prefix_len = prefix ? (int)strlen(prefix) : 0;
    int total;

    do {
        tmp[n++] = digits[value % base];
        value /= base;
    } while (value);

    total = n + (is_negative ? 1 : 0) + prefix_len;

    if (!left && !zero_pad) {
        out_pad(o, ' ', width - total);
    }
    if (is_negative) {
        out_char(o, '-');
    }
    while (prefix && *prefix) {
        out_char(o, *prefix++);
    }
    if (!left && zero_pad) {
        out_pad(o, '0', width - total);
    }
    while (n) {
        out_char(o, tmp[--n]);
    }
    if (left) {
        out_pad(o, ' ', width - total);
    }
}

static int format_into(struct outbuf *op, const char *fmt, va_list ap);

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct outbuf o;
    int n;
    o.buf = buf;
    o.size = size;
    o.len = 0;
    o.pos = 0;
    o.flush = NULL;
    n = format_into(&o, fmt, ap);
    if (size) {
        o.buf[(o.len < size) ? o.len : size - 1] = '\0';
    }
    return n;
}

static int format_into(struct outbuf *op, const char *fmt, va_list ap)
{

    while (*fmt) {
        int left = 0, zero_pad = 0, width = 0, precision = -1, length = 0;

        if (*fmt != '%') {
            out_char(op, *fmt++);
            continue;
        }
        fmt++;

        /* flag */
        for (;;) {
            if (*fmt == '-') { left = 1; fmt++; }
            else if (*fmt == '0') { zero_pad = 1; fmt++; }
            else break;
        }
        /* width */
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) { left = 1; width = -width; }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt++ - '0');
            }
        }
        /* precision */
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            if (*fmt == '*') {
                precision = va_arg(ap, int);
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9') {
                    precision = precision * 10 + (*fmt++ - '0');
                }
            }
        }
        /* length: 0 = int, 1 = long, 2 = long long, 3 = size_t */
        if (*fmt == 'l') {
            length = 1;
            fmt++;
            if (*fmt == 'l') { length = 2; fmt++; }
        } else if (*fmt == 'z') {
            length = 3;
            fmt++;
        }

        switch (*fmt) {
        case 'd':
        case 'i': {
            int64_t v;
            if (length == 1)      v = va_arg(ap, long);
            else if (length == 2) v = va_arg(ap, long long);
            else if (length == 3) v = (int64_t)va_arg(ap, size_t);
            else                  v = va_arg(ap, int);
            if (v < 0) {
                out_num(op, (uint64_t)0 - (uint64_t)v, 1, 10, 0, width, zero_pad, left, NULL);
            } else {
                out_num(op, (uint64_t)v, 0, 10, 0, width, zero_pad, left, NULL);
            }
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            uint64_t v;
            unsigned base = (*fmt == 'u') ? 10 : (*fmt == 'o') ? 8 : 16;
            if (length == 1)      v = va_arg(ap, unsigned long);
            else if (length == 2) v = va_arg(ap, unsigned long long);
            else if (length == 3) v = va_arg(ap, size_t);
            else                  v = va_arg(ap, unsigned int);
            out_num(op, v, 0, base, *fmt == 'X', width, zero_pad, left, NULL);
            break;
        }
        case 'p': {
            uintptr_t v = (uintptr_t)va_arg(ap, void *);
            out_num(op, v, 0, 16, 0, width ? width : 18, 1, left, "0x");
            break;
        }
        case 's':
            out_str(op, va_arg(ap, const char *), width, precision, left);
            break;
        case 'c':
            if (!left) out_pad(op, ' ', width - 1);
            out_char(op, (char)va_arg(ap, int));
            if (left) out_pad(op, ' ', width - 1);
            break;
        case '%':
            out_char(op, '%');
            break;
        case '\0':
            /* ফরম্যাটের শেষে একা '%' */
            goto done;
        default:
            /* অচেনা specifier — যেমন আছে তেমন দেখাই */
            out_char(op, '%');
            out_char(op, *fmt);
            break;
        }
        fmt++;
    }

done:
    return (int)op->len;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

int kvprintf(const char *fmt, va_list ap)
{
    char buf[256];
    struct outbuf o;
    int n;
    o.buf = buf;
    o.size = sizeof(buf);
    o.len = 0;
    o.pos = 0;
    o.flush = tty_write;
    n = format_into(&o, fmt, ap);
    if (o.pos) {
        tty_write(buf, o.pos);
    }
    return n;
}

int kprintf(const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = kvprintf(fmt, ap);
    va_end(ap);
    return n;
}
