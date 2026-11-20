/* rozOS — lwIP-এর জন্য <stdlib.h> (কার্নেলের heap; lwIP নিজের heap ব্যবহার করে, এগুলো শুধু সংজ্ঞার জন্য) */
#ifndef ROZOS_LWIP_STDLIB_H
#define ROZOS_LWIP_STDLIB_H
#include <stddef.h>
#include <rozos/heap.h>
#define malloc  kmalloc
#define calloc  kcalloc
#define free    kfree
#define realloc krealloc
static inline int abs(int v) { return v < 0 ? -v : v; }

/* netif_find() ব্যবহার করে ("en0" → 0) */
static inline int atoi(const char *s)
{
    int v = 0, neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-' || *s == '+') neg = (*s++ == '-');
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}
#endif
