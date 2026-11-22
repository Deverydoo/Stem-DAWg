// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include "AllocationGuard.h"

#include <cstdlib>
#include <new>

namespace {
thread_local int tlsGuardDepth = 0;
thread_local std::size_t tlsAllocations = 0;
thread_local std::size_t tlsDeallocations = 0;

void* allocate(std::size_t size)
{
    if (tlsGuardDepth > 0)
        ++tlsAllocations;
    if (void* p = std::malloc(size == 0 ? 1 : size))
        return p;
    throw std::bad_alloc();
}

void release(void* p) noexcept
{
    if (p == nullptr)
        return;
    if (tlsGuardDepth > 0)
        ++tlsDeallocations;
    std::free(p);
}
} // namespace

void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    try {
        return allocate(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    try {
        return allocate(size);
    } catch (...) {
        return nullptr;
    }
}
void operator delete(void* p) noexcept { release(p); }
void operator delete[](void* p) noexcept { release(p); }
void operator delete(void* p, std::size_t) noexcept { release(p); }
void operator delete[](void* p, std::size_t) noexcept { release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { release(p); }

namespace daw::test {

AllocationGuard::AllocationGuard() noexcept : startAllocations_(tlsAllocations), startDeallocations_(tlsDeallocations)
{
    ++tlsGuardDepth;
}

AllocationGuard::~AllocationGuard()
{
    --tlsGuardDepth;
}

std::size_t AllocationGuard::allocations() const noexcept
{
    return tlsAllocations - startAllocations_;
}

std::size_t AllocationGuard::deallocations() const noexcept
{
    return tlsDeallocations - startDeallocations_;
}

} // namespace daw::test
