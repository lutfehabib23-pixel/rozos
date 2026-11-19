// rozOS — কার্নেলের ন্যূনতম C++ রানটাইম
//
// কম্পাইল ফ্ল্যাগ (config.mk): -fno-exceptions -fno-rtti -fno-threadsafe-statics
// তাই exception/RTTI/guard-এর রানটাইম লাগে না। যা লাগে:
//   - গ্লোবাল কনস্ট্রাক্টর চালানো (.init_array, linker.ld সংগ্রহ করে)
//   - operator new / delete (heap-এর মাধ্যমে)
//   - __cxa_pure_virtual, __cxa_atexit, __dso_handle

#include <stddef.h>
#include <stdint.h>

extern "C" {
#include <rozos/kcxx.h>
#include <rozos/panic.h>
}

// linker.ld-এ সংজ্ঞায়িত
extern "C" void (*__init_array_start[])(void);
extern "C" void (*__init_array_end[])(void);

static kcxx_alloc_fn heap_alloc = nullptr;
static kcxx_free_fn  heap_free  = nullptr;

extern "C" size_t kcxx_init(void)
{
    size_t count = 0;
    for (void (**ctor)(void) = __init_array_start; ctor < __init_array_end; ctor++) {
        (*ctor)();
        count++;
    }
    return count;
}

extern "C" void kcxx_set_allocator(kcxx_alloc_fn alloc, kcxx_free_fn free_fn)
{
    heap_alloc = alloc;
    heap_free  = free_fn;
}

extern "C" int kcxx_has_allocator(void)
{
    return heap_alloc != nullptr && heap_free != nullptr;
}

// ---------- new / delete ----------

static void *cxx_alloc(size_t size)
{
    if (!heap_alloc) {
        kpanic("C++ new called before the kernel heap is ready (size=%zu)", size);
    }
    void *p = heap_alloc(size ? size : 1);
    if (!p) {
        kpanic("C++ new: out of memory (size=%zu)", size);
    }
    return p;
}

static void cxx_free(void *p)
{
    if (p && heap_free) {
        heap_free(p);
    }
}

void *operator new(size_t size) { return cxx_alloc(size); }
void *operator new[](size_t size) { return cxx_alloc(size); }
void operator delete(void *p) noexcept { cxx_free(p); }
void operator delete[](void *p) noexcept { cxx_free(p); }
void operator delete(void *p, size_t) noexcept { cxx_free(p); }
void operator delete[](void *p, size_t) noexcept { cxx_free(p); }

// ---------- ABI সহায়ক ----------

extern "C" void __cxa_pure_virtual(void)
{
    kpanic("pure virtual function called");
}

// কার্নেল কখনো "exit" করে না, তাই গ্লোবাল ডেস্ট্রাক্টর নিবন্ধনের দরকার নেই
extern "C" int __cxa_atexit(void (*)(void *), void *, void *)
{
    return 0;
}

extern "C" {
void *__dso_handle = nullptr;
}
