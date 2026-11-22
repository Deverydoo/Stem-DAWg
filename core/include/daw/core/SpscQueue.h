// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Bounded, wait-free single-producer / single-consumer ring buffer.
//
// Real-time contract: push() and pop() never allocate, lock or block. All
// storage is allocated in the constructor. Exactly one thread may push and
// exactly one (possibly different) thread may pop.

#include <atomic>
#include <cstddef>
#include <utility>
#include <vector>

namespace daw {

template <typename T>
class SpscQueue {
public:
    explicit SpscQueue(std::size_t capacity) : slots_(roundUpToPowerOfTwo(capacity + 1)), mask_(slots_.size() - 1) {}

    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;

    /// Producer side. Returns false (and leaves the queue unchanged) when full.
    bool push(T value) noexcept(std::is_nothrow_move_assignable_v<T>)
    {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = (head + 1) & mask_;
        if (next == tail_.load(std::memory_order_acquire))
            return false;
        slots_[head] = std::move(value);
        head_.store(next, std::memory_order_release);
        return true;
    }

    /// Consumer side. Returns false when empty.
    bool pop(T& out) noexcept(std::is_nothrow_move_assignable_v<T>)
    {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire))
            return false;
        out = std::move(slots_[tail]);
        tail_.store((tail + 1) & mask_, std::memory_order_release);
        return true;
    }

    bool empty() const noexcept
    {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    /// Approximate when called concurrently with push/pop.
    std::size_t sizeApprox() const noexcept
    {
        const std::size_t head = head_.load(std::memory_order_acquire);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        return (head - tail) & mask_;
    }

    std::size_t capacity() const noexcept { return mask_; }

private:
    static std::size_t roundUpToPowerOfTwo(std::size_t value)
    {
        std::size_t result = 2;
        while (result < value)
            result <<= 1;
        return result;
    }

    std::vector<T> slots_;
    std::size_t mask_;
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};

} // namespace daw
