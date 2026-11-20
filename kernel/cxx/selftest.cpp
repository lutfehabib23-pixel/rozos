// rozOS — কার্নেল C++ সাপোর্টের স্বয়ং-পরীক্ষা
// একটা গ্লোবাল অবজেক্টের কনস্ট্রাক্টর kcxx_init()-এর সময় চলা উচিত; না চললে
// linker.ld-এর .init_array বা kcxx_init-এ সমস্যা আছে।

#include <stdint.h>
#include "new.hpp"

extern "C" {
#include <rozos/kcxx.h>
}

namespace {

class CtorProbe {
public:
    CtorProbe() : magic_(0x726F7A4F53ULL) {}   // "rozOS"
    bool ok() const { return magic_ == 0x726F7A4F53ULL; }
private:
    volatile uint64_t magic_;
};

class Base {
public:
    virtual ~Base() {}
    virtual int value() const = 0;
};

class Derived : public Base {
public:
    int value() const override { return 42; }
};

CtorProbe probe;

} // namespace

extern "C" int kcxx_selftest(void)
{
    // virtual dispatch যাচাই — placement new দিয়ে স্ট্যাকে (heap লাগে না)
    alignas(Derived) unsigned char storage[sizeof(Derived)];
    Base *obj = new (storage) Derived();
    int virt_ok = (obj->value() == 42);
    obj->~Base();

    return probe.ok() && virt_ok;
}

// ---------- heap চালু হওয়ার পরের পরীক্ষা (ধাপ ৩) ----------

namespace {

int live_objects = 0;

class Tracked {
public:
    explicit Tracked(int v) : value_(v) { live_objects++; }
    virtual ~Tracked() { live_objects--; }
    virtual int get() const { return value_; }
private:
    int value_;
};

class TrackedChild : public Tracked {
public:
    explicit TrackedChild(int v) : Tracked(v), extra_(v * 2) { live_objects++; }
    ~TrackedChild() override { live_objects--; }
    int get() const override { return Tracked::get() + extra_; }
private:
    int extra_;
};

} // namespace

extern "C" int kcxx_heap_selftest(void)
{
    if (!kcxx_has_allocator()) {
        return 0;
    }

    // একক অবজেক্ট + virtual destructor (বেস পয়েন্টার দিয়ে delete)
    Tracked *t = new TrackedChild(7);
    int ok = (t->get() == 7 + 14) && (live_objects == 2);
    delete t;
    ok = ok && (live_objects == 0);

    // অ্যারে
    int *arr = new int[64];
    for (int i = 0; i < 64; i++) {
        arr[i] = i * i;
    }
    for (int i = 0; i < 64; i++) {
        if (arr[i] != i * i) {
            ok = 0;
        }
    }
    delete[] arr;

    return ok;
}
