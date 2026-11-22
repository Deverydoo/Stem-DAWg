// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/core/SpscQueue.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <thread>

using daw::SpscQueue;

TEST_CASE("SpscQueue preserves FIFO order and reports full/empty", "[core][spsc]")
{
    SpscQueue<int> queue(4);
    REQUIRE(queue.capacity() >= 4);
    REQUIRE(queue.empty());

    for (int i = 0; i < static_cast<int>(queue.capacity()); ++i)
        REQUIRE(queue.push(i));
    REQUIRE_FALSE(queue.push(99)); // full

    int value = -1;
    for (int i = 0; i < static_cast<int>(queue.capacity()); ++i) {
        REQUIRE(queue.pop(value));
        REQUIRE(value == i);
    }
    REQUIRE_FALSE(queue.pop(value));
    REQUIRE(queue.empty());
}

TEST_CASE("SpscQueue wraps around many times", "[core][spsc]")
{
    SpscQueue<int> queue(3);
    int value = 0;
    for (int i = 0; i < 10000; ++i) {
        REQUIRE(queue.push(i));
        REQUIRE(queue.pop(value));
        REQUIRE(value == i);
    }
}

TEST_CASE("SpscQueue transfers every item exactly once across threads", "[core][spsc][threads]")
{
    constexpr std::int64_t count = 200000;
    SpscQueue<std::int64_t> queue(64);

    std::thread producer([&] {
        for (std::int64_t i = 0; i < count; ++i)
            while (!queue.push(i))
                std::this_thread::yield();
    });

    std::int64_t expected = 0;
    std::int64_t sum = 0;
    bool ordered = true;
    while (expected < count) {
        std::int64_t value;
        if (queue.pop(value)) {
            ordered = ordered && value == expected;
            sum += value;
            ++expected;
        } else {
            std::this_thread::yield();
        }
    }
    producer.join();

    REQUIRE(ordered);
    REQUIRE(sum == count * (count - 1) / 2);
}
