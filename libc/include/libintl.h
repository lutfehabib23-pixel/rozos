/* rozlibc — libintl.h (ধাপ ১৬ক): অনুবাদ নেই — gettext মূল লেখাটাই ফেরত দেয় */
#ifndef _LIBINTL_H
#define _LIBINTL_H
#define gettext(s)                ((char *)(s))
#define dgettext(d, s)            ((char *)(s))
#define dcgettext(d, s, c)        ((char *)(s))
#define ngettext(s, p, n)         ((char *)((n) == 1 ? (s) : (p)))
#define dngettext(d, s, p, n)     ((char *)((n) == 1 ? (s) : (p)))
#define dcngettext(d, s, p, n, c) ((char *)((n) == 1 ? (s) : (p)))
#define textdomain(d)             ((char *)"messages")
#define bindtextdomain(d, dir)    ((char *)(dir))
#define bind_textdomain_codeset(d, c) ((char *)(c))
#endif
