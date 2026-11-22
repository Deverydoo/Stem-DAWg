// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

#include <cstddef>

namespace daw::test {

/// Counts heap allocations/deallocations made by the current thread while the
/// guard is alive. Only valid in test binaries that link AllocationGuard.cpp,
/// which replaces the global operator new/delete.
class AllocationGuard {
public:
    AllocationGuard() noexcept;
    ~AllocationGuard();

    AllocationGuard(const AllocationGuard&) = delete;
    AllocationGuard& operator=(const AllocationGuard&) = delete;

    std::size_t allocations() const noexcept;
    std::size_t deallocations() const noexcept;

private:
    std::size_t startAllocations_;
    std::size_t startDeallocations_;
};

} // namespace daw::test
