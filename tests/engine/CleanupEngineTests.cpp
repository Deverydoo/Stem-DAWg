// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

// Clean-up chains inside the engine: the lookahead is hidden (processed
// tracks stay sample-aligned with the timeline), output is exactly the chain
// run offline, whatever the block size, across seeks and settings changes.

#include "AllocationGuard.h"
#include "EngineTestHelpers.h"
#include "TestSignals.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

using namespace daw;
using namespace daw::test;

namespace {

constexpr double kRate = 48000.0;
constexpr std::int64_t kFrames = 30000;

/// Low drone + high tone, so a high-pass audibly changes it.
AudioAssetPtr makeMusic(int channels = 2)
{
    auto asset = AudioAsset::create(channels, kFrames, kRate);
    for (int c = 0; c < channels; ++c)
        for (std::int64_t i = 0; i < kFrames; ++i) {
            const double t = static_cast<double>(i) / kRate;
            asset->channels[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)] =
                static_cast<float>(0.4 * std::sin(2 * std::numbers::pi * 45.0 * t) + (c == 0 ? 0.2 : 0.15) * std::sin(2 * std::numbers::pi * 1500.0 * t));
        }
    return asset;
}

CleanupParams highPass(float hz = 200.0f)
{
    CleanupParams p;
    p.dcRemoval = true;
    p.highPass = true;
    p.highPassHz = hz;
    p.highPassOrder = 4;
    p.expander = true;
    p.thresholdDb = -30.0f;
    return p;
}

std::unique_ptr<PlaybackSnapshot> cleanSnapshot(const AudioAssetPtr& asset, const CleanupParams& params,
                                                std::uint64_t version = 1)
{
    auto snapshot = singleClipSnapshot(asset, 0, 0, kFrames, version);
    snapshot->tracks[0].key = 42;
    snapshot->tracks[0].cleanup = params;
    return snapshot;
}

/// The chain run offline over the asset from `start`, with the engine's
/// alignment applied: result[j] is what the engine should play at start + j.
/// `change` switches to `next` once the chain has consumed `changeAt` input frames.
StereoCapture offline(const AudioAssetPtr& asset, const CleanupParams& params, std::int64_t start, std::int64_t frames,
                      std::int64_t changeAt = -1, const CleanupParams& next = {})
{
    CleanupChain chain;
    chain.prepare(kRate);
    chain.setParams(params);
    const int latency = chain.latency();
    const std::int64_t total = frames + latency;
    std::vector<float> l(static_cast<std::size_t>(total), 0.0f), r(static_cast<std::size_t>(total), 0.0f);
    for (std::int64_t i = 0; i < total; ++i) {
        const std::int64_t at = start + i;
        if (at < asset->numFrames()) {
            const int channels = asset->numChannels();
            l[static_cast<std::size_t>(i)] = asset->channels[0][static_cast<std::size_t>(at)];
            r[static_cast<std::size_t>(i)] = asset->channels[channels > 1 ? 1 : 0][static_cast<std::size_t>(at)];
        }
    }
    if (changeAt < 0) {
        chain.process(l.data(), r.data(), static_cast<int>(total));
    } else {
        chain.process(l.data(), r.data(), static_cast<int>(changeAt));
        chain.setParams(next);
        chain.process(l.data() + changeAt, r.data() + changeAt, static_cast<int>(total - changeAt));
    }
    StereoCapture out;
    out.left.assign(l.begin() + latency, l.end());
    out.right.assign(r.begin() + latency, r.end());
    return out;
}

} // namespace

TEST_CASE("A transparent clean-up chain plays sample-aligned with the dry clip", "[engine][cleanup]")
{
    const auto asset = makeRamp(kFrames, kRate);
    CleanupParams transparent;
    transparent.expander = true;
    transparent.ratio = 1.0f;

    AudioEngine engine;
    engine.prepare({kRate, 512, 2});
    engine.publishSnapshot(cleanSnapshot(asset, transparent));
    engine.setStopAtEnd(false);
    engine.play();
    const auto out = renderBlocks(engine, kFrames, 480);
    REQUIRE(out.left == asset->channels[0]);
    REQUIRE(out.right == asset->channels[1]);
}

TEST_CASE("Engine clean-up equals the chain run offline, for any block size", "[engine][cleanup]")
{
    const auto asset = makeMusic();
    const auto expected = offline(asset, highPass(), 0, 25000);
    const int blockSize = GENERATE(1, 64, 480, 512, 4096);
    AudioEngine engine;
    engine.prepare({kRate, 512, 2}); // 4096-frame blocks exceed maxBlockSize: rendered in pieces
    engine.publishSnapshot(cleanSnapshot(asset, highPass()));
    engine.play();
    const auto out = renderBlocks(engine, 25000, blockSize);
    REQUIRE(out.left == expected.left);
    REQUIRE(out.right == expected.right);
}

TEST_CASE("Engine clean-up restarts cleanly after a seek", "[engine][cleanup]")
{
    const auto asset = makeMusic();
    AudioEngine engine;
    engine.prepare({kRate, 512, 2});
    engine.publishSnapshot(cleanSnapshot(asset, highPass()));
    engine.play();
    renderBlocks(engine, 10000, 512);
    engine.seek(5000);
    const auto out = renderBlocks(engine, 8000, 512);
    const auto expected = offline(asset, highPass(), 5000, 8000);
    REQUIRE(out.left == expected.left);
    REQUIRE(out.right == expected.right);
}

TEST_CASE("Changing clean-up settings mid-play keeps the chain running", "[engine][cleanup]")
{
    const auto asset = makeMusic();
    AudioEngine engine;
    engine.prepare({kRate, 512, 2});
    engine.publishSnapshot(cleanSnapshot(asset, highPass(200.0f)));
    engine.play();
    auto first = renderBlocks(engine, 5120, 512);
    engine.publishSnapshot(cleanSnapshot(asset, highPass(400.0f), 2));
    auto second = renderBlocks(engine, 5120, 512);
    first.left.insert(first.left.end(), second.left.begin(), second.left.end());
    first.right.insert(first.right.end(), second.right.begin(), second.right.end());

    // The new settings take effect at the block boundary, with the filter memory kept.
    CleanupChain probe;
    probe.prepare(kRate);
    const auto expected = offline(asset, highPass(200.0f), 0, 10240, 5120 + probe.latency(), highPass(400.0f));
    REQUIRE(first.left == expected.left);
    REQUIRE(first.right == expected.right);
}

TEST_CASE("Mono media feeds both channels of the clean-up chain", "[engine][cleanup]")
{
    const auto asset = makeMusic(1);
    AudioEngine engine;
    engine.prepare({kRate, 512, 2});
    engine.publishSnapshot(cleanSnapshot(asset, highPass()));
    engine.play();
    const auto out = renderBlocks(engine, 12000, 512);
    const auto expected = offline(asset, highPass(), 0, 12000);
    REQUIRE(out.left == expected.left);
    REQUIRE(out.right == out.left);
}

TEST_CASE("Clean-up slots follow tracks across snapshots without touching the heap", "[engine][cleanup][rt]")
{
    AudioEngine engine;
    engine.prepare({kRate, 256, 2});
    engine.beginProcessing();
    const auto asset = makeMusic();

    auto build = [&](std::uint64_t version) {
        auto snapshot = std::make_unique<PlaybackSnapshot>();
        snapshot->version = version;
        snapshot->sampleRate = kRate;
        snapshot->keepAlive.push_back(asset);
        for (std::uint64_t key = 1; key <= 12; ++key) {
            TrackPlayback track;
            track.clips.push_back(ClipPlayback{asset.get(), 0, 0, kFrames, 1.0f});
            track.gainLeft = track.gainRight = (key + version) % 4 == 0 ? 0.0f : 0.5f; // some silent: chains go stale
            track.key = key;
            if ((key + version) % 3 != 0) // chains switch on and off between snapshots
                track.cleanup = highPass(100.0f + 10.0f * static_cast<float>(version % 7));
            snapshot->tracks.push_back(std::move(track));
        }
        if (version % 2 == 0)
            std::reverse(snapshot->tracks.begin(), snapshot->tracks.end());
        snapshot->endFrame = kFrames;
        return snapshot;
    };

    engine.publishSnapshot(build(1));
    engine.play();
    std::vector<float> l(256), r(256);
    float* outs[2] = {l.data(), r.data()};
    engine.process(outs, 2, 256);

    std::size_t allocations = 0, deallocations = 0;
    for (std::uint64_t version = 2; version < 40; ++version) {
        engine.publishSnapshot(build(version)); // control thread
        if (version % 5 == 0)
            engine.seek(static_cast<std::int64_t>(version * 300));
        AllocationGuard guard;
        for (int i = 0; i < 3; ++i)
            engine.process(outs, 2, 256);
        allocations += guard.allocations();
        deallocations += guard.deallocations();
        for (float v : l)
            REQUIRE(std::isfinite(v));
    }
    REQUIRE(allocations == 0);
    REQUIRE(deallocations == 0);
    engine.endProcessing();
}
