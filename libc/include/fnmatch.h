/* rozlibc — fnmatch.h (ধাপ ১১): শেল-ধাঁচের প্যাটার্ন  * ? [abc] [a-z] [!x] */
#ifndef _FNMATCH_H
#define _FNMATCH_H
#ifdef __cplusplus
extern "C" {
#endif

#define FNM_NOMATCH  1
#define FNM_NOESCAPE 0x01
#define FNM_PATHNAME 0x02     /* '/' শুধু '/'-এর সাথে মেলে */
#define FNM_PERIOD   0x04     /* শুরুর '.' শুধু '.'-এর সাথে মেলে */

int fnmatch(const char *pattern, const char *string, int flags);

#ifdef __cplusplus
}
#endif
#endif
