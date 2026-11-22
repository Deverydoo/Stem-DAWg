// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/engine/Transport.h>

#include <catch2/catch_test_macros.hpp>

using namespace daw;

TEST_CASE("Transport starts stopped at zero", "[engine][transport]")
{
    Transport t;
    REQUIRE(t.playhead() == 0);
    REQUIRE_FALSE(t.playing());
    REQUIRE(t.stopAtEnd());
}

TEST_CASE("Transport advances only while playing", "[engine][transport]")
{
    Transport t;
    t.advance(512);
    REQUIRE(t.playhead() == 0);
    t.apply({TransportCommandType::Play});
    t.advance(512);
    t.advance(100);
    REQUIRE(t.playhead() == 612);
    t.apply({TransportCommandType::Stop});
    t.advance(512);
    REQUIRE(t.playhead() == 612); // stop pauses in place
}

TEST_CASE("Transport seek clamps negative positions", "[engine][transport]")
{
    Transport t;
    t.apply({TransportCommandType::Seek, 48000});
    REQUIRE(t.playhead() == 48000);
    t.apply({TransportCommandType::Seek, -5});
    REQUIRE(t.playhead() == 0);
}

TEST_CASE("Transport position is exact over a 24 hour session", "[engine][transport]")
{
    Transport t;
    t.apply({TransportCommandType::Play});
    constexpr std::int64_t block = 441;
    const std::int64_t blocks = 24LL * 3600 * 192000 / block; // 24 h at 192 kHz
    for (std::int64_t i = 0; i < blocks; ++i)
        t.advance(block);
    REQUIRE(t.playhead() == blocks * block);
}

TEST_CASE("Transport stopAt pauses at an exact frame", "[engine][transport]")
{
    Transport t;
    t.apply({TransportCommandType::Play});
    t.advance(1000);
    t.stopAt(800);
    REQUIRE_FALSE(t.playing());
    REQUIRE(t.playhead() == 800);
}
