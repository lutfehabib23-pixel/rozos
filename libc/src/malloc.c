/* rozlibc — malloc/free/realloc/calloc
 *
 * ছোট ও সরল: brk দিয়ে বাড়ানো একটা heap, প্রতিটি টুকরোর আগে 16 বাইটের হেডার
 * (আকার + ব্যবহৃত কিনা), মুক্ত টুকরোগুলো ঠিকানার ক্রমে একটা তালিকায় — first-fit,
 * free-তে পাশের মুক্ত টুকরোর সাথে জোড়া লাগে। বড় অনুরোধ (≥ 256 KiB) সরাসরি mmap,
 * free-তে munmap। সব ঠিকানা 16-byte aligned। */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>

#define ALIGN        16
#define HDR          16
#define MMAP_LIMIT   (256 * 1024)
#define GROW_MIN     (64 * 1024)
#define FLAG_USED    1UL
#define FLAG_MMAP    2UL

struct chunk {
    size_t size;                 /* হেডার সহ মোট আকার | ফ্ল্যাগ (নিচের ২ বিট) */
    struct chunk *next_free;     /* শুধু মুক্ত অবস্থায় অর্থবহ */
};

static struct chunk *free_list;  /* ঠিকানার ক্রমে */
static char *heap_start, *heap_end;

#define CSIZE(c) ((c)->size & ~(size_t)(ALIGN - 1))

static size_t round_up(size_t n)
{
    return (n + ALIGN - 1) & ~(size_t)(ALIGN - 1);
}

/* ঠিকানার ক্রমে বসিয়ে দুই পাশের সাথে জোড়া লাগানো */
static void insert_free(struct chunk *c)
{
    struct chunk **pp = &free_list, *prev = NULL;
    while (*pp && *pp < c) {
        prev = *pp;
        pp = &(*pp)->next_free;
    }
    c->next_free = *pp;
    *pp = c;
    if (c->next_free && (char *)c + CSIZE(c) == (char *)c->next_free) {
        c->size = CSIZE(c) + CSIZE(c->next_free);
        c->next_free = c->next_free->next_free;
    }
    if (prev && (char *)prev + CSIZE(prev) == (char *)c) {
        prev->size = CSIZE(prev) + CSIZE(c);
        prev->next_free = c->next_free;
    }
}

static int grow(size_t need)
{
    size_t n = need < GROW_MIN ? GROW_MIN : need;
    char *p;
    if (!heap_start) {
        char *b = (char *)sbrk(0);
        size_t pad = (size_t)(-(unsigned long)b & (ALIGN - 1));
        if (pad && sbrk((long)pad) == (void *)-1) return 0;
        heap_start = heap_end = (char *)sbrk(0);
    }
    n = round_up(n);
    p = (char *)sbrk((long)n);
    if (p == (char *)-1) return 0;
    heap_end = p + n;
    {
        struct chunk *c = (struct chunk *)p;
        c->size = n;
        insert_free(c);
    }
    return 1;
}

void *malloc(size_t n)
{
    struct chunk **pp, *c;
    size_t need;

    if (n == 0) n = 1;
    if (n > ((size_t)-1) / 2) { errno = ENOMEM; return NULL; }
    need = round_up(n) + HDR;

    if (need >= MMAP_LIMIT) {
        size_t len = (need + 4095) & ~(size_t)4095;
        void *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) { errno = ENOMEM; return NULL; }
        c = (struct chunk *)m;
        c->size = len | FLAG_USED | FLAG_MMAP;
        return (char *)c + HDR;
    }

    for (;;) {
        for (pp = &free_list; *pp; pp = &(*pp)->next_free) {
            c = *pp;
            if (CSIZE(c) >= need) {
                if (CSIZE(c) - need >= HDR + ALIGN) {          /* ভাগ করা */
                    struct chunk *rest = (struct chunk *)((char *)c + need);
                    rest->size = CSIZE(c) - need;
                    rest->next_free = c->next_free;
                    *pp = rest;
                    c->size = need;
                } else {
                    *pp = c->next_free;
                }
                c->size |= FLAG_USED;
                return (char *)c + HDR;
            }
        }
        if (!grow(need)) { errno = ENOMEM; return NULL; }
    }
}

void free(void *p)
{
    struct chunk *c;
    if (!p) return;
    c = (struct chunk *)((char *)p - HDR);
    if (!(c->size & FLAG_USED)) {
        static const char msg[] = "rozlibc: free(): double free or invalid pointer\n";
        write(2, msg, sizeof(msg) - 1);
        abort();
    }
    if (c->size & FLAG_MMAP) {
        munmap(c, CSIZE(c));
        return;
    }
    c->size = CSIZE(c);
    insert_free(c);
}

void *calloc(size_t count, size_t n)
{
    size_t total;
    void *p;
    if (n && count > ((size_t)-1) / n) { errno = ENOMEM; return NULL; }
    total = count * n;
    p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *p, size_t n)
{
    struct chunk *c;
    size_t have;
    void *q;
    if (!p) return malloc(n);
    if (n == 0) { free(p); return NULL; }
    c = (struct chunk *)((char *)p - HDR);
    have = CSIZE(c) - HDR;
    if (n <= have) return p;
    q = malloc(n);
    if (!q) return NULL;
    memcpy(q, p, have);
    free(p);
    return q;
}
