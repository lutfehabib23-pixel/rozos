/* rozlibc — wctype.h (ধাপ ১৬ক): ASCII-র বাইরে Latin-1, Latin Extended, গ্রিক,
 * সিরিলিক, বাংলা ও CJK-র মোটামুটি শ্রেণি; বড়/ছোট হাতের রূপান্তর ইউরোপীয় বর্ণমালায় */
#ifndef _WCTYPE_H
#define _WCTYPE_H
#include <stddef.h>

#ifndef _ROZLIBC_WINT
#define _ROZLIBC_WINT
typedef unsigned int wint_t;
#endif
#ifndef WEOF
#define WEOF ((wint_t)-1)
#endif
typedef unsigned long wctype_t;
typedef const int *wctrans_t;

int iswalnum(wint_t c);
int iswalpha(wint_t c);
int iswblank(wint_t c);
int iswcntrl(wint_t c);
int iswdigit(wint_t c);
int iswgraph(wint_t c);
int iswlower(wint_t c);
int iswprint(wint_t c);
int iswpunct(wint_t c);
int iswspace(wint_t c);
int iswupper(wint_t c);
int iswxdigit(wint_t c);
wint_t towlower(wint_t c);
wint_t towupper(wint_t c);
wctype_t wctype(const char *name);
int      iswctype(wint_t c, wctype_t t);
wctrans_t wctrans(const char *name);
wint_t   towctrans(wint_t c, wctrans_t t);

#endif
