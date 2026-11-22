/* rozlibc — search.h (ধাপ ১৬ক): tsearch পরিবার (বাইনারি গাছ), lsearch/lfind,
 * hsearch (একটাই টেবিল), insque/remque */
#ifndef _SEARCH_H
#define _SEARCH_H
#include <stddef.h>

typedef enum { FIND, ENTER } ACTION;
typedef enum { preorder, postorder, endorder, leaf } VISIT;
typedef struct entry { char *key; void *data; } ENTRY;

void *tsearch(const void *key, void **root, int (*cmp)(const void *, const void *));
void *tfind(const void *key, void *const *root, int (*cmp)(const void *, const void *));
void *tdelete(const void *key, void **root, int (*cmp)(const void *, const void *));
void  twalk(const void *root, void (*fn)(const void *node, VISIT v, int depth));
void  tdestroy(void *root, void (*freefn)(void *));
void *lfind(const void *key, const void *base, size_t *n, size_t size,
            int (*cmp)(const void *, const void *));
void *lsearch(const void *key, void *base, size_t *n, size_t size,
              int (*cmp)(const void *, const void *));
int    hcreate(size_t n);
void   hdestroy(void);
ENTRY *hsearch(ENTRY e, ACTION a);
void   insque(void *elem, void *prev);
void   remque(void *elem);

#endif
