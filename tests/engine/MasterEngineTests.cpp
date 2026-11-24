// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

// The master bus in the engine (EngineConfig::masterChain): the limiter holds
// the output under its ceiling, delays it by exactly masterLatency() frames,
// forgets the audio before a seek, stays off unless asked for, and never
// allocates.

#include "AllocationGuard.h"
#include "EngineTestHelpers.h"
#include "TestSignals.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

using namespace daw;
using namespace daw::test;

namespace {

constexpr double kRate = 48000.0;

std::unique_ptr<PlaybackSnapshot> loudSnapshot(const AudioAssetPtr& asset, bool limiter)
{
    auto snapshot = singleClipSnapshot(asset, 0, 0, asset->numFrames());
    snapshot->master = {.limiter = limiter, .ceilingDbtp = -1.0f, .releaseMs = 100.0f};
    return snapshot;
}

float peakOf(const StereoCapture& c, std::size_t from = 0)
{
    float peak = 0.0f;
    for (std::size_t i = from; i < c.left.size(); ++i)
        peak = std::max({peak, std::abs(c.left[i]), std::abs(c.right[i])});
    return peak;
}

} // namespace

TEST_CASE("The engine's master bus limits the mix and delays it by its latency", "[engine][master]")
{
    const auto asset = makeSine(200.0, 1.0, kRate, 2, 2.0f); // +6 dBFS
    AudioEngine engine;
    engine.prepare({.sampleRate = kRate, .maxBlockSize = 256, .masterChain = true});
    engine.publishSnapshot(loudSnapshot(asset, true));
    engine.seek(0);
    engine.play();
    const int latency = engine.masterLatency();
    REQUIRE(latency > 0);
    const StereoCapture out = renderBlocks(engine, 24000, 256);
    for (int i = 0; i < latency; ++i)
        REQUIRE(out.left[static_cast<std::size_t>(i)] == 0.0f); // the limiter's empty delay
    CHECK(std::abs(out.left[static_cast<std::size_t>(latency) + 3]) > 0.0f);
    CHECK(peakOf(out) <= std::pow(10.0f, -1.0f / 20.0f) * 1.003f); // -1 dBTP ceiling (sample peaks)
    CHECK(engine.takeMasterReductionDb() < -6.0f);

    // After a seek into silence nothing from before the seek comes out.
    engine.seek(asset->numFrames() + 48000);
    const StereoCapture after = renderBlocks(engine, 512, 256);
    CHECK(peakOf(after) == 0.0f);
}

TEST_CASE("Without masterChain the engine plays the mix as before", "[engine][master]")
{
    const auto asset = makeSine(200.0, 0.5, kRate, 2, 2.0f);
    AudioEngine engine;
    engine.prepare({.sampleRate = kRate, .maxBlockSize = 256});
    engine.publishSnapshot(loudSnapshot(asset, true)); // a limiter in the snapshot is ignored
    engine.seek(0);
    engine.play();
    CHECK(engine.masterLatency() == 0);
    const StereoCapture out = renderBlocks(engine, 12000, 256);
    CHECK(peakOf(out) > 1.9f);
    CHECK(out.left[100] == asset->channels[0][100]);
}

TEST_CASE("The engine's master bus never allocates while playing", "[engine][master][rt]")
{
    const auto asset = makeSine(200.0, 1.0, kRate, 2, 2.0f);
    AudioEngine engine;
    engine.prepare({.sampleRate = kRate, .maxBlockSize = 256, .masterChain = true});
    engine.publishSnapshot(loudSnapshot(asset, true));
    engine.seek(0);
    engine.play();
    renderBlocks(engine, 1024, 256); // adopt the snapshot first
    std::vector<float> l(256), r(256);
    float* outs[2] = {l.data(), r.data()};
    {
        AllocationGuard guard;
        for (int b = 0; b < 40; ++b)
            engine.process(outs, 2, 256);
        engine.seek(1000);
        for (int b = 0; b < 10; ++b)
            engine.process(outs, 2, 256);
        CHECK(guard.allocations() == 0);
    }
}
