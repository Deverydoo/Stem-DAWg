// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include "EngineTestHelpers.h"
#include "TestSignals.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>

using namespace daw;
using namespace daw::test;

namespace {
constexpr double kRate = 48000.0;

AudioEngine& prepared(AudioEngine& engine)
{
    engine.prepare({kRate, 4096, 2});
    return engine;
}
} // namespace

TEST_CASE("Engine renders silence with no snapshot or when stopped", "[engine]")
{
    AudioEngine engine;
    prepared(engine);
    auto out = renderBlocks(engine, 2048, 512);
    for (float s : out.left)
        REQUIRE(s == 0.0f);

    engine.publishSnapshot(singleClipSnapshot(makeRamp(4800, kRate), 0, 0, 4800));
    out = renderBlocks(engine, 2048, 512); // not playing
    for (float s : out.left)
        REQUIRE(s == 0.0f);
    REQUIRE(engine.transportState().playhead == 0);
}

TEST_CASE("Engine output is sample-exact against the source clip", "[engine]")
{
    AudioEngine engine;
    prepared(engine);
    const auto asset = makeRamp(30000, kRate);
    constexpr std::int64_t timelineStart = 1000, sourceStart = 500, length = 20000;
    engine.publishSnapshot(singleClipSnapshot(asset, timelineStart, sourceStart, length));
    engine.setStopAtEnd(false);
    engine.play();

    const auto out = renderBlocks(engine, 24000, 480);
    for (std::int64_t i = 0; i < 24000; ++i) {
        const bool inside = i >= timelineStart && i < timelineStart + length;
        const float expectedL = inside ? asset->channels[0][static_cast<std::size_t>(i - timelineStart + sourceStart)] : 0.0f;
        const float expectedR = inside ? asset->channels[1][static_cast<std::size_t>(i - timelineStart + sourceStart)] : 0.0f;
        if (out.left[static_cast<std::size_t>(i)] != expectedL || out.right[static_cast<std::size_t>(i)] != expectedR)
            FAIL("mismatch at frame " << i);
    }
    REQUIRE(engine.transportState().playhead == 24000);
}

TEST_CASE("Engine output is independent of device block size", "[engine][regression]")
{
    const auto asset = makeRamp(30000, kRate);
    auto renderWith = [&](int blockSize) {
        AudioEngine engine;
        prepared(engine);
        engine.publishSnapshot(singleClipSnapshot(asset, 1000, 500, 20000));
        engine.play();
        return renderBlocks(engine, 25000, blockSize);
    };

    const auto reference = renderWith(512);
    const int blockSize = GENERATE(1, 7, 64, 480, 513, 4096);
    const auto other = renderWith(blockSize);
    REQUIRE(other.left == reference.left);
    REQUIRE(other.right == reference.right);
}

TEST_CASE("Engine seek repositions playback exactly", "[engine]")
{
    AudioEngine engine;
    prepared(engine);
    const auto asset = makeRamp(30000, kRate);
    engine.publishSnapshot(singleClipSnapshot(asset, 0, 0, 30000));
    engine.play();
    renderBlocks(engine, 1024, 256);
    engine.seek(20000);
    const auto out = renderBlocks(engine, 512, 512);
    for (std::int64_t i = 0; i < 512; ++i)
        REQUIRE(out.left[static_cast<std::size_t>(i)] == asset->channels[0][static_cast<std::size_t>(20000 + i)]);
    REQUIRE(engine.transportState().playhead == 20512);
}

TEST_CASE("Engine stops at end of content at the exact frame", "[engine]")
{
    AudioEngine engine;
    prepared(engine);
    engine.publishSnapshot(singleClipSnapshot(makeRamp(10000, kRate), 0, 0, 10000));
    engine.play();
    renderBlocks(engine, 12288, 512);
    const auto state = engine.transportState();
    REQUIRE_FALSE(state.playing);
    REQUIRE(state.playhead == 10000);

    bool sawStopEvent = false;
    RtEvent event;
    while (engine.popEvent(event))
        sawStopEvent = sawStopEvent || event.code == RtEventCode::TransportStoppedAtEnd;
    REQUIRE(sawStopEvent);
}

TEST_CASE("Engine maps mono assets to both channels and applies gain", "[engine]")
{
    AudioEngine engine;
    prepared(engine);
    const auto asset = makeRamp(4800, kRate, 1);
    engine.publishSnapshot(singleClipSnapshot(asset, 0, 0, 4800, 1, 0.5f));
    engine.play();
    const auto out = renderBlocks(engine, 4800, 512);
    for (std::int64_t i = 0; i < 4800; ++i) {
        const float expected = asset->channels[0][static_cast<std::size_t>(i)] * 0.5f;
        REQUIRE(out.left[static_cast<std::size_t>(i)] == expected);
        REQUIRE(out.right[static_cast<std::size_t>(i)] == expected);
    }
}

TEST_CASE("Engine renders silence and reports a sample-rate mismatch", "[engine]")
{
    AudioEngine engine;
    prepared(engine);
    auto asset = makeRamp(4800, 44100.0);
    engine.publishSnapshot(singleClipSnapshot(asset, 0, 0, 4800));
    engine.play();
    const auto out = renderBlocks(engine, 2048, 512);
    for (float s : out.left)
        REQUIRE(s == 0.0f);
    REQUIRE(engine.diagnostics().rateMismatchBlocks == 4);

    int mismatchEvents = 0;
    RtEvent event;
    while (engine.popEvent(event))
        mismatchEvents += event.code == RtEventCode::SampleRateMismatch ? 1 : 0;
    REQUIRE(mismatchEvents == 1); // reported once per snapshot, not per block
}

TEST_CASE("Engine swaps snapshots mid-playback without losing position", "[engine]")
{
    AudioEngine engine;
    prepared(engine);
    const auto a = makeRamp(48000, kRate);
    const auto b = makeSine(440.0, 1.0, kRate);
    engine.publishSnapshot(singleClipSnapshot(a, 0, 0, 48000, 1));
    engine.play();
    renderBlocks(engine, 4096, 512);
    engine.publishSnapshot(singleClipSnapshot(b, 0, 0, 48000, 2));
    const auto out = renderBlocks(engine, 512, 512);
    for (std::int64_t i = 0; i < 512; ++i)
        REQUIRE(out.left[static_cast<std::size_t>(i)] == b->channels[0][static_cast<std::size_t>(4096 + i)]);
    REQUIRE(engine.collectGarbage() == 1);
    REQUIRE(engine.diagnostics().snapshotSwaps == 2);
}

TEST_CASE("Engine queues commands while processing and applies them at the next block", "[engine]")
{
    AudioEngine engine;
    prepared(engine);
    engine.publishSnapshot(singleClipSnapshot(makeRamp(48000, kRate), 0, 0, 48000));
    engine.beginProcessing();
    engine.seek(1234);
    engine.play();
    REQUIRE(engine.transportState().playhead == 0); // not applied yet
    renderBlocks(engine, 100, 100);
    REQUIRE(engine.transportState() == TransportState{1334, true});

    engine.endProcessing();
    engine.stop(); // applied immediately when not processing
    REQUIRE_FALSE(engine.transportState().playing);
}

TEST_CASE("Engine meters report and reset peaks", "[engine]")
{
    AudioEngine engine;
    prepared(engine);
    engine.publishSnapshot(singleClipSnapshot(makeSine(1000.0, 0.5, kRate, 2, 0.25f), 0, 0, 24000));
    engine.play();
    renderBlocks(engine, 4800, 480);
    const auto peaks = engine.takeMeterPeaks();
    REQUIRE(peaks.left > 0.24f);
    REQUIRE(peaks.left <= 0.25f);
    const auto reset = engine.takeMeterPeaks();
    REQUIRE(reset.left == 0.0f);
}

// ---- Mixer: per-channel track gains and declicked gain changes -------------

namespace {

std::shared_ptr<AudioAsset> makeDc(std::int64_t frames, float value = 1.0f)
{
    auto asset = AudioAsset::create(2, frames, kRate);
    for (auto& channel : asset->channels)
        std::fill(channel.begin(), channel.end(), value);
    return asset;
}

std::unique_ptr<PlaybackSnapshot> keyedSnapshot(const AudioAssetPtr& asset, std::uint64_t version, float left,
                                                float right, std::uint64_t key = 42)
{
    auto snapshot = singleClipSnapshot(asset, 0, 0, asset->numFrames(), version);
    snapshot->tracks[0].gainLeft = left;
    snapshot->tracks[0].gainRight = right;
    snapshot->tracks[0].key = key;
    return snapshot;
}

} // namespace

TEST_CASE("Engine applies per-channel track gains", "[engine][mixer]")
{
    AudioEngine engine;
    prepared(engine);
    const auto asset = makeRamp(4800, kRate);
    engine.publishSnapshot(keyedSnapshot(asset, 1, 1.0f, 0.0f)); // hard left
    engine.play();
    const auto out = renderBlocks(engine, 4800, 512);
    for (std::size_t i = 0; i < 4800; ++i) {
        REQUIRE(out.left[i] == asset->channels[0][i]);
        REQUIRE(out.right[i] == 0.0f);
    }
}

TEST_CASE("Gain changes during playback ramp over 10 ms instead of stepping", "[engine][mixer][regression]")
{
    AudioEngine engine;
    prepared(engine);
    const auto asset = makeDc(48000);
    engine.publishSnapshot(keyedSnapshot(asset, 1, 1.0f, 1.0f));
    engine.play();
    renderBlocks(engine, 1024, 256);

    engine.publishSnapshot(keyedSnapshot(asset, 2, 0.0f, 0.5f)); // e.g. mute left, fader down right
    const auto out = renderBlocks(engine, 2048, 256);
    constexpr std::size_t ramp = 480; // 10 ms at 48 kHz

    // Strictly moving towards the target, in small steps, and exact afterwards.
    for (std::size_t i = 0; i < ramp; ++i) {
        const float prevL = i == 0 ? 1.0f : out.left[i - 1];
        const float prevR = i == 0 ? 1.0f : out.right[i - 1];
        REQUIRE(out.left[i] < prevL);
        REQUIRE(out.right[i] < prevR);
        REQUIRE(prevL - out.left[i] < 0.0025f);
        REQUIRE(prevR - out.right[i] < 0.0013f);
    }
    REQUIRE(std::abs(out.left[ramp - 1]) < 1e-6f);
    REQUIRE(std::abs(out.right[ramp - 1] - 0.5f) < 1e-6f);
    for (std::size_t i = ramp; i < out.left.size(); ++i) {
        REQUIRE(out.left[i] == 0.0f);
        REQUIRE(out.right[i] == 0.5f);
    }
}

TEST_CASE("Gain ramps are independent of device block size", "[engine][mixer]")
{
    auto renderWith = [](int blockSize) {
        AudioEngine engine;
        engine.prepare({kRate, 4096, 2});
        const auto asset = makeDc(48000);
        engine.publishSnapshot(keyedSnapshot(asset, 1, 1.0f, 1.0f));
        engine.play();
        renderBlocks(engine, 4096, 4096);
        engine.publishSnapshot(keyedSnapshot(asset, 2, 0.25f, 0.75f));
        return renderBlocks(engine, 2048, blockSize);
    };
    const auto reference = renderWith(2048);
    for (int blockSize : {1, 64, 100, 480, 512}) {
        const auto out = renderWith(blockSize);
        for (std::size_t i = 0; i < reference.left.size(); ++i) {
            REQUIRE(out.left[i] == reference.left[i]);
            REQUIRE(out.right[i] == reference.right[i]);
        }
    }
}

TEST_CASE("Gain changes while stopped apply without a ramp", "[engine][mixer]")
{
    AudioEngine engine;
    prepared(engine);
    const auto asset = makeDc(48000);
    engine.publishSnapshot(keyedSnapshot(asset, 1, 1.0f, 1.0f));
    renderBlocks(engine, 512, 512); // adopted while stopped
    engine.publishSnapshot(keyedSnapshot(asset, 2, 0.5f, 0.5f));
    renderBlocks(engine, 512, 512);
    engine.play();
    const auto out = renderBlocks(engine, 512, 512);
    for (float s : out.left)
        REQUIRE(s == 0.5f);
}

TEST_CASE("A new track starts at its target gain; unrelated tracks keep ramping state", "[engine][mixer]")
{
    AudioEngine engine;
    prepared(engine);
    const auto asset = makeDc(48000);
    engine.publishSnapshot(keyedSnapshot(asset, 1, 1.0f, 1.0f, 7));
    engine.play();
    renderBlocks(engine, 512, 512);

    // Different key = different track: no ramp from the old track's gain.
    engine.publishSnapshot(keyedSnapshot(asset, 2, 0.25f, 0.25f, 8));
    const auto out = renderBlocks(engine, 512, 512);
    for (float s : out.left)
        REQUIRE(s == 0.25f);
}

TEST_CASE("The engine meters the loudness of what it plays; a seek restarts the measurement", "[engine][loudness]")
{
    AudioEngine engine;
    engine.prepare({48000.0, 512, 2});
    // 1 kHz stereo sine at -23 dBFS reads -23 LUFS (EBU Tech 3341, test 1).
    const auto tone = makeSine(1000.0, 6.0, 48000.0, 2, static_cast<float>(std::pow(10.0, -23.0 / 20.0)));
    engine.publishSnapshot(singleClipSnapshot(tone, 0, 0, tone->numFrames(), 1));

    renderBlocks(engine, 512 * 100, 512); // stopped: nothing is measured
    REQUIRE(engine.loudness().integratedLufs == kLoudnessFloor);

    engine.play();
    renderBlocks(engine, 512 * 500, 512); // 5.3 s
    const auto playing = engine.loudness();
    REQUIRE(std::abs(playing.integratedLufs + 23.0) < 0.1);
    REQUIRE(std::abs(playing.momentaryLufs + 23.0) < 0.1);
    REQUIRE(std::abs(playing.truePeakDbtp + 23.0) < 0.2);
    REQUIRE(playing.seconds > 5.0);

    engine.stop(); // pause keeps the measurement
    renderBlocks(engine, 512 * 10, 512);
    REQUIRE(engine.loudness().integratedLufs == playing.integratedLufs);

    engine.seek(0);
    renderBlocks(engine, 512, 512);
    REQUIRE(engine.loudness().integratedLufs == kLoudnessFloor);
    REQUIRE(engine.loudness().seconds == 0.0);

    engine.play();
    renderBlocks(engine, 512 * 200, 512);
    REQUIRE(engine.loudness().integratedLufs > -24.0);
    engine.resetLoudness();
    REQUIRE(engine.loudness().integratedLufs == kLoudnessFloor);
}
