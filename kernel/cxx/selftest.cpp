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
