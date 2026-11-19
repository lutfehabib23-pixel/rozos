// rozOS — কার্নেলের C++ কোডের জন্য placement new।
// ক্রস-কম্পাইলারে hosted libstdc++ নেই (--disable-hosted-libstdcxx), তাই <new>
// ব্যবহার না করে এখানে নিজেরাই সংজ্ঞায়িত করা হলো।
#ifndef ROZOS_CXX_NEW_HPP
#define ROZOS_CXX_NEW_HPP

#include <stddef.h>

inline void *operator new(size_t, void *where) noexcept { return where; }
inline void *operator new[](size_t, void *where) noexcept { return where; }
inline void operator delete(void *, void *) noexcept {}
inline void operator delete[](void *, void *) noexcept {}

#endif
