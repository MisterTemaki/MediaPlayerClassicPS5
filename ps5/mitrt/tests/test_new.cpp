// SPDX-License-Identifier: MIT
// Copyright (C) 2026 MisterTemaki
//
// Host test of src/ps5rt_new.cpp (linked in, it replaces the C++ library's global operator new / delete).

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <new>

namespace {

int failures;

void check(bool ok, const char *what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}

volatile std::size_t huge = SIZE_MAX / 2 + 1;
int handler_calls;

void handler_once()
{
    ++handler_calls;
    std::set_new_handler(nullptr);   // the next failure throws
}

void handler_throws()
{
    ++handler_calls;
    throw std::bad_alloc();
}

template <typename F> bool throws_bad_alloc(F f)
{
    try
    {
        f();
    }
    catch (const std::bad_alloc &)
    {
        return true;
    }
    return false;
}

} // namespace

int main()
{
    check(throws_bad_alloc([] { ::operator delete(::operator new(huge)); }), "operator new: std::bad_alloc");
    // A new-expression immediately deleted may be elided even if allocation would throw.
    // Call the replacement directly so optimization cannot remove the behavior under test.
    check(throws_bad_alloc([] { ::operator delete[](::operator new[](huge)); }), "operator new[]: std::bad_alloc");
    check(throws_bad_alloc([] { ::operator delete(::operator new(huge, std::align_val_t(64)), std::align_val_t(64)); }),
          "aligned operator new: std::bad_alloc");
    check(throws_bad_alloc([] { ::operator delete[](::operator new[](huge, std::align_val_t(64)), std::align_val_t(64)); }),
          "aligned operator new[]: std::bad_alloc");
    check(::operator new(huge, std::nothrow) == nullptr, "nothrow new: null");
    check(::operator new[](huge, std::nothrow) == nullptr, "nothrow new[]: null");
    check(::operator new(huge, std::align_val_t(64), std::nothrow) == nullptr, "aligned nothrow new: null");
    check(::operator new[](huge, std::align_val_t(64), std::nothrow) == nullptr, "aligned nothrow new[]: null");

    void *zero = ::operator new(0);
    void *zero2 = ::operator new(0);
    check(zero != nullptr && zero2 != nullptr && zero != zero2, "new of 0 bytes: distinct pointers");
    ::operator delete(zero);
    ::operator delete(zero2, std::size_t{0});

    bool aligned = true;
    for (std::size_t a : {std::size_t{1}, std::size_t{16}, std::size_t{64}, std::size_t{4096}})
    {
        void *p = ::operator new(100, std::align_val_t(a));
        aligned = aligned && p != nullptr && reinterpret_cast<std::uintptr_t>(p) % a == 0;
        ::operator delete(p, 100, std::align_val_t(a));
        void *q = ::operator new[](0, std::align_val_t(a), std::nothrow);
        aligned = aligned && q != nullptr && reinterpret_cast<std::uintptr_t>(q) % a == 0;
        ::operator delete[](q, std::align_val_t(a), std::nothrow);
    }
    check(aligned, "aligned new: 1, 16, 64, 4096");

    struct alignas(256) Big
    {
        char c[256];
    };
    Big *big = new Big[3];
    check(reinterpret_cast<std::uintptr_t>(big) % 256 == 0, "new of an over-aligned type");
    delete[] big;

    handler_calls = 0;
    std::set_new_handler(handler_once);
    check(throws_bad_alloc([] { ::operator delete(::operator new(huge)); }) && handler_calls == 1,
          "the new-handler is called, then std::bad_alloc when there is none");
    handler_calls = 0;
    std::set_new_handler(handler_throws);
    check(::operator new(huge, std::nothrow) == nullptr && handler_calls == 1,
          "nothrow new: a new-handler's std::bad_alloc becomes null");
    std::set_new_handler(nullptr);

    int *ints = new int[4]{1, 2, 3, 4};
    check(ints[3] == 4, "new[] / delete[]");
    delete[] ints;
    ::operator delete(nullptr);
    ::operator delete[](nullptr, std::nothrow);
    check(true, "delete of null");

    std::printf("test_new: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
