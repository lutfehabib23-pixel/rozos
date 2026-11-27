/* rozlibc — iconv.h (ধাপ ১৬ক): UTF-8, ASCII, ISO-8859-1 (LATIN1), UTF-16/32 (LE/BE,
 * BOM সহ), WCHAR_T, CP1252; শেষে "//TRANSLIT" বা "//IGNORE" বসানো যায় */
#ifndef _ICONV_H
#define _ICONV_H
#ifdef __cplusplus
extern "C" {
#endif
#include <stddef.h>

typedef void *iconv_t;
iconv_t iconv_open(const char *to, const char *from);
size_t  iconv(iconv_t cd, char **in, size_t *inleft, char **out, size_t *outleft);
int     iconv_close(iconv_t cd);

#ifdef __cplusplus
}
#endif
#endif
