// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/core/Files.h>
#include <daw/media/PeakCache.h>
#include <daw/media/WavWriter.h>

#include "TempDir.h"
#include "TestSignals.h"

#include <catch2/catch_test_macros.hpp>

using namespace daw;
using namespace daw::test;

TEST_CASE("Peak cache round-trips bit-exactly", "[media][peakcache]")
{
    TempDir dir("peakcache");
    const auto pyramid = PeakPyramid::build(*makeNoise(123457, 44100.0, 2, 0.5f));
    REQUIRE(pyramid.ok());
    const auto file = dir / "a.peaks";
    REQUIRE(writePeakCache(file, *pyramid, 0xABCDEF).ok());
    const auto loaded = readPeakCache(file, 0xABCDEF);
    REQUIRE(loaded.ok());
    REQUIRE(*loaded == *pyramid);
}

TEST_CASE("Peak cache rejects stale, corrupt, truncated and missing files", "[media][peakcache]")
{
    TempDir dir("peakcache-bad");
    const auto pyramid = PeakPyramid::build(*makeNoise(50000, 48000.0, 1, 0.5f));
    REQUIRE(pyramid.ok());
    const std::string bytes = encodePeakCache(*pyramid, 42);

    REQUIRE(decodePeakCache(bytes, 43).error().code == ErrorCode::Stale);

    std::string flipped = bytes;
    flipped[flipped.size() / 2] ^= 0x01;
    REQUIRE(decodePeakCache(flipped, 42).error().code == ErrorCode::Corrupt);

    REQUIRE(decodePeakCache(bytes.substr(0, bytes.size() - 100), 42).error().code == ErrorCode::Corrupt);
    REQUIRE(decodePeakCache("", 42).error().code == ErrorCode::Corrupt);
    REQUIRE(readPeakCache(dir / "none.peaks", 42).error().code == ErrorCode::NotFound);
}

TEST_CASE("Source key changes when the media file changes", "[media][peakcache]")
{
    TempDir dir("sourcekey");
    const auto wav = dir / "a.wav";
    REQUIRE(writeWavFile(wav, *makeRamp(1000, 48000.0)).ok());
    const auto first = computeSourceKey(wav);
    REQUIRE(first.ok());
    REQUIRE(*computeSourceKey(wav) == *first); // stable

    REQUIRE(writeWavFile(wav, *makeRamp(2000, 48000.0)).ok()); // different size
    REQUIRE(*computeSourceKey(wav) != *first);
    REQUIRE(computeSourceKey(dir / "missing.wav").error().code == ErrorCode::NotFound);
}
