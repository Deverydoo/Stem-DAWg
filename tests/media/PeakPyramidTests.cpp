// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/media/PeakPyramid.h>

#include "TestSignals.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <random>

using namespace daw;
using namespace daw::test;

namespace {
PeakBucket bruteForce(const AudioAsset& asset, int channel, std::int64_t start, std::int64_t end)
{
    const auto& x = asset.channels[static_cast<std::size_t>(channel)];
    const auto first = x.begin() + start;
    const auto last = x.begin() + end;
    return {*std::min_element(first, last), *std::max_element(first, last)};
}
} // namespace

TEST_CASE("Peak pyramid level 0 matches brute force exactly", "[media][peaks]")
{
    const auto asset = makeNoise(100003, 48000.0, 2, 0.9f, 99);
    const auto pyramid = PeakPyramid::build(*asset);
    REQUIRE(pyramid.ok());
    const PeakLevel& base = pyramid->levels().front();
    REQUIRE(base.samplesPerBucket == 256);
    REQUIRE(base.numBuckets == (100003 + 255) / 256);
    for (int c = 0; c < 2; ++c)
        for (std::int64_t b = 0; b < base.numBuckets; ++b) {
            const std::int64_t start = b * 256;
            const std::int64_t end = std::min<std::int64_t>(start + 256, asset->numFrames());
            REQUIRE(base.channel(c)[b] == bruteForce(*asset, c, start, end));
        }
}

TEST_CASE("Peak pyramid coarser levels merge exactly", "[media][peaks]")
{
    const auto asset = makeNoise(1 << 20, 48000.0, 1, 1.0f, 3);
    const auto pyramid = PeakPyramid::build(*asset);
    REQUIRE(pyramid.ok());
    REQUIRE(pyramid->levels().size() > 3);
    REQUIRE(pyramid->levels().back().numBuckets <= 16);
    for (const PeakLevel& level : pyramid->levels())
        for (std::int64_t b = 0; b < level.numBuckets; b += 7) {
            const std::int64_t start = b * level.samplesPerBucket;
            const std::int64_t end = std::min(start + level.samplesPerBucket, asset->numFrames());
            REQUIRE(level.channel(0)[b] == bruteForce(*asset, 0, start, end));
        }
}

TEST_CASE("Peak queries are exact on boundaries and conservative elsewhere", "[media][peaks]")
{
    const auto asset = makeNoise(300000, 48000.0, 2, 0.7f, 11);
    const auto pyramid = PeakPyramid::build(*asset);
    REQUIRE(pyramid.ok());

    // Span 4096 selects the 4096-frame level; aligned ends make it exact.
    REQUIRE(pyramid->query(1, 3 * 4096, 4 * 4096) == bruteForce(*asset, 1, 3 * 4096, 4 * 4096));
    // Unaligned to that level but aligned to the base level: exact when the base level is forced.
    REQUIRE(pyramid->query(1, 1024, 1024 + 4096, pyramid->levels().front()) == bruteForce(*asset, 1, 1024, 1024 + 4096));

    std::mt19937 rng(5);
    std::uniform_int_distribution<std::int64_t> pos(0, asset->numFrames() - 2);
    for (int i = 0; i < 500; ++i) {
        std::int64_t a = pos(rng), b = pos(rng);
        if (a > b)
            std::swap(a, b);
        b = std::max(b, a + 1);
        const auto approx = pyramid->query(0, a, b);
        const auto exact = bruteForce(*asset, 0, a, b);
        REQUIRE(approx.min <= exact.min);
        REQUIRE(approx.max >= exact.max);
    }
}

TEST_CASE("Peak level selection follows the zoom level", "[media][peaks]")
{
    const auto pyramid = PeakPyramid::build(*makeSilence(1 << 20, 48000.0, 1));
    REQUIRE(pyramid.ok());
    REQUIRE(pyramid->levelForResolution(10.0).samplesPerBucket == 256);   // finer than base: base
    REQUIRE(pyramid->levelForResolution(300.0).samplesPerBucket == 256);
    REQUIRE(pyramid->levelForResolution(1024.0).samplesPerBucket == 1024);
    REQUIRE(pyramid->levelForResolution(5000.0).samplesPerBucket == 4096);
}

TEST_CASE("Peak pyramid rejects empty audio and handles out-of-range queries", "[media][peaks]")
{
    REQUIRE_FALSE(PeakPyramid::build(*makeSilence(0, 48000.0, 2)).ok());
    const auto pyramid = PeakPyramid::build(*makeRamp(1000, 48000.0));
    REQUIRE(pyramid.ok());
    REQUIRE(pyramid->query(5, 0, 100) == PeakBucket{});
    REQUIRE(pyramid->query(0, 2000, 3000) == PeakBucket{});
}
