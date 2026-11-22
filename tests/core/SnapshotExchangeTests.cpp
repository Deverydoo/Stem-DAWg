// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/core/SnapshotExchange.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>

namespace {

std::atomic<int> liveObjects{0};

struct Tracked {
    explicit Tracked(int v) : value(v) { ++liveObjects; }
    ~Tracked() { --liveObjects; }
    int value;
};

} // namespace

TEST_CASE("SnapshotExchange hands the latest object to the consumer", "[core][snapshot]")
{
    liveObjects = 0;
    {
        daw::SnapshotExchange<Tracked> exchange;
        REQUIRE(exchange.acquire() == nullptr);

        exchange.publish(std::make_unique<Tracked>(1));
        const Tracked* first = exchange.acquire();
        REQUIRE(first != nullptr);
        REQUIRE(first->value == 1);
        REQUIRE(exchange.acquire() == first); // stable until next publish

        exchange.publish(std::make_unique<Tracked>(2));
        REQUIRE(exchange.acquire()->value == 2);
        REQUIRE(liveObjects == 2); // old one retired, not yet freed

        REQUIRE(exchange.collectGarbage() == 1);
        REQUIRE(liveObjects == 1);
        REQUIRE(exchange.swapCount() == 2);
    }
    REQUIRE(liveObjects == 0);
}

TEST_CASE("SnapshotExchange destroys superseded objects the consumer never saw", "[core][snapshot]")
{
    liveObjects = 0;
    daw::SnapshotExchange<Tracked> exchange;
    exchange.publish(std::make_unique<Tracked>(1));
    exchange.publish(std::make_unique<Tracked>(2));
    exchange.publish(std::make_unique<Tracked>(3));
    REQUIRE(liveObjects == 1);
    REQUIRE(exchange.acquire()->value == 3);
    REQUIRE(exchange.swapCount() == 1);
}

TEST_CASE("SnapshotExchange never frees on the consumer and never leaks under contention", "[core][snapshot][threads]")
{
    liveObjects = 0;
    {
        daw::SnapshotExchange<Tracked> exchange;
        std::atomic<bool> done{false};
        std::atomic<bool> monotonic{true};

        std::thread consumer([&] {
            int last = 0;
            while (!done.load()) {
                if (const Tracked* t = exchange.acquire()) {
                    if (t->value < last)
                        monotonic = false;
                    last = t->value;
                }
            }
        });

        for (int i = 1; i <= 20000; ++i)
            exchange.publish(std::make_unique<Tracked>(i));
        done = true;
        consumer.join();

        REQUIRE(monotonic);
        REQUIRE(exchange.leakCount() == 0);
        exchange.collectGarbage();
    }
    REQUIRE(liveObjects == 0);
}
