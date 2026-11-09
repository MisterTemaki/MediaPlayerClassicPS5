// SPDX-License-Identifier: MIT
// Copyright (C) 2026 MisterTemaki
//
// The replaceable global allocation functions (C++17 [new.delete]) for a native PS5 title, on the system C library's
// malloc / posix_memalign / free:
//   - operator new / new[] (plain, aligned): retry through the new-handler while there is one, then throw
//     std::bad_alloc;
//   - the std::nothrow_t forms: the same, but a null pointer instead of the exception;
//   - operator delete / delete[] (plain, sized, aligned, sized + aligned, nothrow): free().
// Built with exceptions, so a failed allocation's std::bad_alloc reaches the program's catch (or std::terminate).
// The same file builds for the host (g++ / clang++ on Linux) for the tests.

#include <cstddef>
#include <cstdlib>
#include <new>

namespace {

void *allocate(std::size_t size)
{
    if (size == 0)
        size = 1;
    for (;;)
    {
        if (void *p = std::malloc(size))
            return p;
        std::new_handler handler = std::get_new_handler();
        if (handler == nullptr)
            throw std::bad_alloc();
        handler();
    }
}

void *allocate_aligned(std::size_t size, std::align_val_t align)
{
    std::size_t alignment = static_cast<std::size_t>(align);
    if (alignment < sizeof(void *))
        alignment = sizeof(void *);
    if (size == 0)
        size = 1;
    for (;;)
    {
        void *p = nullptr;
        if (posix_memalign(&p, alignment, size) == 0 && p != nullptr)
            return p;
        std::new_handler handler = std::get_new_handler();
        if (handler == nullptr)
            throw std::bad_alloc();
        handler();
    }
}

void *allocate_nothrow(std::size_t size) noexcept
{
    try
    {
        return allocate(size);
    }
    catch (...)
    {
        return nullptr;
    }
}

void *allocate_aligned_nothrow(std::size_t size, std::align_val_t align) noexcept
{
    try
    {
        return allocate_aligned(size, align);
    }
    catch (...)
    {
        return nullptr;
    }
}

} // namespace

void *operator new(std::size_t size)
{
    return allocate(size);
}

void *operator new[](std::size_t size)
{
    return allocate(size);
}

void *operator new(std::size_t size, const std::nothrow_t &) noexcept
{
    return allocate_nothrow(size);
}

void *operator new[](std::size_t size, const std::nothrow_t &) noexcept
{
    return allocate_nothrow(size);
}

void *operator new(std::size_t size, std::align_val_t align)
{
    return allocate_aligned(size, align);
}

void *operator new[](std::size_t size, std::align_val_t align)
{
    return allocate_aligned(size, align);
}

void *operator new(std::size_t size, std::align_val_t align, const std::nothrow_t &) noexcept
{
    return allocate_aligned_nothrow(size, align);
}

void *operator new[](std::size_t size, std::align_val_t align, const std::nothrow_t &) noexcept
{
    return allocate_aligned_nothrow(size, align);
}

void operator delete(void *p) noexcept
{
    std::free(p);
}

void operator delete[](void *p) noexcept
{
    std::free(p);
}

void operator delete(void *p, const std::nothrow_t &) noexcept
{
    std::free(p);
}

void operator delete[](void *p, const std::nothrow_t &) noexcept
{
    std::free(p);
}

void operator delete(void *p, std::size_t) noexcept
{
    std::free(p);
}

void operator delete[](void *p, std::size_t) noexcept
{
    std::free(p);
}

void operator delete(void *p, std::align_val_t) noexcept
{
    std::free(p);
}

void operator delete[](void *p, std::align_val_t) noexcept
{
    std::free(p);
}

void operator delete(void *p, std::align_val_t, const std::nothrow_t &) noexcept
{
    std::free(p);
}

void operator delete[](void *p, std::align_val_t, const std::nothrow_t &) noexcept
{
    std::free(p);
}

void operator delete(void *p, std::size_t, std::align_val_t) noexcept
{
    std::free(p);
}

void operator delete[](void *p, std::size_t, std::align_val_t) noexcept
{
    std::free(p);
}
