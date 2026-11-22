// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Real-time safe hand-off of immutable state objects from a control thread to
// the audio thread, with deferred destruction on the control thread.
//
//   control thread                       audio thread
//   --------------                       ------------
//   publish(new)  -> pending_ ---------> acquire(): adopt pending, push old
//                                         current_ onto retired_ queue
//   collectGarbage() <- retired_ <-------
//
// Guarantees:
//  * acquire() never allocates, frees, locks or blocks.
//  * An object is only destroyed on the control thread, after the audio thread
//    has stopped referencing it.
//  * If the control thread publishes several times before the audio thread
//    runs, intermediate objects are destroyed directly (never observed).
//
// publish() drains the retired queue first. Each adoption retires at most one
// object, and adoption only follows a publish, so the retired queue holds at
// most two entries between publishes; a full queue is therefore impossible in
// correct use. If it ever happens, the audio thread keeps its current object
// and the stale pending object is leaked and counted (never freed on the RT
// thread).

#include <daw/core/SpscQueue.h>

#include <atomic>
#include <cstdint>
#include <memory>

namespace daw {

template <typename T>
class SnapshotExchange {
public:
    explicit SnapshotExchange(std::size_t retireCapacity = 64) : retired_(retireCapacity) {}

    /// Must only run once the consumer thread has stopped calling acquire().
    ~SnapshotExchange()
    {
        collectGarbage();
        delete pending_.exchange(nullptr, std::memory_order_acq_rel);
        delete current_;
    }

    SnapshotExchange(const SnapshotExchange&) = delete;
    SnapshotExchange& operator=(const SnapshotExchange&) = delete;

    // ---- control thread (single producer) ---------------------------------

    void publish(std::unique_ptr<T> next)
    {
        collectGarbage();
        T* superseded = pending_.exchange(next.release(), std::memory_order_acq_rel);
        delete superseded; // never seen by the consumer
    }

    /// Destroys objects the consumer has retired. Returns how many were freed.
    std::size_t collectGarbage()
    {
        std::size_t freed = 0;
        T* retired = nullptr;
        while (retired_.pop(retired)) {
            delete retired;
            ++freed;
        }
        return freed;
    }

    std::uint64_t swapCount() const noexcept { return swaps_.load(std::memory_order_relaxed); }
    std::uint64_t leakCount() const noexcept { return leaks_.load(std::memory_order_relaxed); }

    // ---- consumer thread (real-time) --------------------------------------

    /// Adopts the most recently published object, if any, and returns the
    /// current one (may be null before the first publish).
    const T* acquire() noexcept
    {
        if (pending_.load(std::memory_order_relaxed) == nullptr)
            return current_;

        T* next = pending_.exchange(nullptr, std::memory_order_acq_rel);
        if (next == nullptr)
            return current_;

        if (current_ == nullptr || retired_.push(current_)) {
            current_ = next;
            swaps_.fetch_add(1, std::memory_order_relaxed);
            return current_;
        }

        // Retire queue full: try to hand `next` back; if the producer already
        // published something newer, `next` is stale and must not be freed here.
        T* expected = nullptr;
        if (!pending_.compare_exchange_strong(expected, next, std::memory_order_acq_rel))
            leaks_.fetch_add(1, std::memory_order_relaxed);
        return current_;
    }

    /// Consumer-thread view of the current object without adopting.
    const T* current() const noexcept { return current_; }

private:
    std::atomic<T*> pending_{nullptr};
    T* current_ = nullptr; // owned by the consumer thread
    SpscQueue<T*> retired_;
    std::atomic<std::uint64_t> swaps_{0};
    std::atomic<std::uint64_t> leaks_{0};
};

} // namespace daw
