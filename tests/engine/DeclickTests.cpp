// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

// Declick: with EngineConfig::declick the live engine fades out before a stop
// or seek takes effect and fades in after play or seek, so the listener never
// hears a click. Offline renders keep it off (covered by the exactness tests).

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
constexpr std::int64_t kFrames = 48000;
const int kFade = static_cast<int>(std::lround(kRate * AudioEngine::kDeclickSeconds)); // 240

/// A constant 0.5 on both channels: any hard start or stop is a full 0.5 step.
AudioAssetPtr constant()
{
    auto asset = AudioAsset::create(2, kFrames, kRate);
    for (auto& channel : asset->channels)
        std::fill(channel.begin(), channel.end(), 0.5f);
    return asset;
}

void start(AudioEngine& engine, const AudioAssetPtr& asset)
{
    EngineConfig config;
    config.sampleRate = kRate;
    config.maxBlockSize = 512;
    config.declick = true;
    engine.prepare(config);
    engine.beginProcessing();
    engine.publishSnapshot(singleClipSnapshot(asset, 0, 0, kFrames));
    engine.setStopAtEnd(false);
}

float largestStep(const std::vector<float>& x, float previous = 0.0f)
{
    float worst = 0.0f;
    for (float v : x) {
        worst = std::max(worst, std::abs(v - previous));
        previous = v;
    }
    return worst;
}

} // namespace

TEST_CASE("Declick: play fades in and stop fades out, keeping the stop position", "[engine][declick]")
{
    AudioEngine engine;
    start(engine, constant());
    engine.play();
    const auto opening = renderBlocks(engine, 2048, 64);
    CHECK(opening.left[0] < 0.01f);
    CHECK(opening.left[static_cast<std::size_t>(kFade)] == 0.5f);
    CHECK(largestStep(opening.left) <= 0.5f / static_cast<float>(kFade) * 1.01f);

    engine.stop();
    const auto closing = renderBlocks(engine, 1024, 64);
    CHECK(largestStep(closing.left, 0.5f) <= 0.5f / static_cast<float>(kFade) * 1.01f);
    CHECK(closing.left[static_cast<std::size_t>(kFade)] == 0.0f);
    CHECK(closing.left.back() == 0.0f);
    const auto state = engine.transportState();
    CHECK_FALSE(state.playing);
    CHECK(state.playhead == 2048); // where Stop was pressed, not where the fade ended
    engine.endProcessing();
}

TEST_CASE("Declick: a seek fades out, jumps, and fades in at the exact new position", "[engine][declick]")
{
    const auto asset = makeRamp(kFrames, kRate);
    AudioEngine engine;
    start(engine, asset);
    engine.play();
    renderBlocks(engine, 2048, 64);
    engine.seek(20000);
    const auto out = renderBlocks(engine, 4096, 64);

    // Fade-out completes inside the 4th 64-frame block; the seek lands after it.
    const auto fadeBlocks = static_cast<std::size_t>((kFade + 63) / 64 * 64);
    for (std::size_t i = static_cast<std::size_t>(kFade); i < fadeBlocks; ++i)
        CHECK(out.left[i] == 0.0f);
    CHECK(std::abs(out.left[fadeBlocks]) < 0.01f); // fading in from silence
    for (std::size_t i = fadeBlocks + static_cast<std::size_t>(kFade); i < out.left.size(); ++i) {
        const auto source = static_cast<std::size_t>(20000) + (i - fadeBlocks);
        if (out.left[i] != asset->channels[0][source] || out.right[i] != asset->channels[1][source])
            FAIL("mismatch at frame " << i);
    }
    CHECK(engine.transportState().playhead == 20000 + static_cast<std::int64_t>(4096 - fadeBlocks));
    engine.endProcessing();
}

TEST_CASE("Declick: play during the fade-out cancels the stop", "[engine][declick]")
{
    AudioEngine engine;
    start(engine, constant());
    engine.play();
    renderBlocks(engine, 1024, 64);
    engine.stop();
    renderBlocks(engine, 64, 64); // part of the fade-out
    engine.play();
    const auto out = renderBlocks(engine, 1024, 64);
    CHECK(engine.transportState().playing);
    CHECK(out.left.back() == 0.5f);
    CHECK(largestStep(out.left, out.left.front()) <= 0.5f / static_cast<float>(kFade) * 1.01f);
    engine.endProcessing();
}

TEST_CASE("Declick: a stop still fading out when the device stops is applied", "[engine][declick]")
{
    AudioEngine engine;
    start(engine, constant());
    engine.play();
    renderBlocks(engine, 1000, 100);
    engine.seek(5000);
    engine.stop();
    engine.endProcessing(); // no further process() calls
    const auto state = engine.transportState();
    CHECK_FALSE(state.playing);
    CHECK(state.playhead == 5000);
}

TEST_CASE("Declick never touches the heap", "[engine][declick][rt]")
{
    AudioEngine engine;
    start(engine, constant());
    std::vector<float> l(64), r(64);
    float* outs[2] = {l.data(), r.data()};
    engine.play();
    engine.process(outs, 2, 64);
    AllocationGuard guard;
    for (int round = 0; round < 10; ++round) {
        engine.seek(round * 1000);
        for (int i = 0; i < 8; ++i)
            engine.process(outs, 2, 64);
        engine.stop();
        for (int i = 0; i < 8; ++i)
            engine.process(outs, 2, 64);
        engine.play();
        for (int i = 0; i < 8; ++i)
            engine.process(outs, 2, 64);
    }
    CHECK(guard.allocations() == 0);
    CHECK(guard.deallocations() == 0);
    engine.endProcessing();
}
