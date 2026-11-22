// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include "AllocationGuard.h"
#include "EngineTestHelpers.h"
#include "TestSignals.h"

#include <daw/core/Logger.h>
#include <daw/core/RtThread.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <thread>

using namespace daw;
using namespace daw::test;

TEST_CASE("Allocation guard detects heap use on the guarded thread", "[engine][rt]")
{
    AllocationGuard guard;
    auto* p = new int(5);
    delete p;
    REQUIRE(guard.allocations() == 1);
    REQUIRE(guard.deallocations() == 1);
}

TEST_CASE("process() never touches the heap, including snapshot swaps and commands", "[engine][rt]")
{
    AudioEngine engine;
    engine.prepare({48000.0, 512, 2});
    engine.beginProcessing();

    const auto a = makeRamp(96000, 48000.0);
    const auto b = makeSine(220.0, 2.0, 48000.0);
    engine.publishSnapshot(singleClipSnapshot(a, 0, 0, 96000, 1));
    engine.play();

    std::vector<float> l(512), r(512);
    float* outs[2] = {l.data(), r.data()};
    engine.process(outs, 2, 512); // first adoption

    // Queue work for the audio thread: a new snapshot (retires the old one),
    // transport commands and RT events.
    engine.publishSnapshot(singleClipSnapshot(b, 100, 0, 90000, 2));
    engine.seek(4000);
    engine.stop();
    engine.play();

    std::size_t allocations = 0, deallocations = 0;
    {
        AllocationGuard guard;
        for (int i = 0; i < 200; ++i)
            engine.process(outs, 2, 512);
        allocations = guard.allocations();
        deallocations = guard.deallocations();
    }
    REQUIRE(allocations == 0);
    REQUIRE(deallocations == 0);
    REQUIRE(engine.diagnostics().snapshotSwaps == 2);

    engine.endProcessing();
    REQUIRE(engine.collectGarbage() == 1); // retired snapshot freed on this (control) thread
}

TEST_CASE("Engine runs concurrently with a control thread publishing snapshots", "[engine][rt][threads]")
{
    AudioEngine engine;
    engine.prepare({48000.0, 256, 2});
    engine.beginProcessing();
    const auto asset = makeRamp(48000, 48000.0);
    engine.publishSnapshot(singleClipSnapshot(asset, 0, 0, 48000, 1));
    engine.setStopAtEnd(false);
    engine.play();

    std::atomic<bool> running{true};
    std::thread audio([&] {
        std::vector<float> l(256), r(256);
        float* outs[2] = {l.data(), r.data()};
        while (running.load())
            engine.process(outs, 2, 256);
    });

    // Only start publishing once the audio thread is really running, and keep it
    // running until it has adopted the last snapshot. Without this the test can
    // finish before the OS ever schedules the audio thread (seen on CI runners)
    // and would then test nothing.
    while (engine.diagnostics().callbacks == 0)
        std::this_thread::yield();
    for (std::uint64_t v = 2; v < 2000; ++v) {
        engine.publishSnapshot(singleClipSnapshot(asset, static_cast<std::int64_t>(v % 100), 0, 48000, v));
        engine.collectGarbage();
        if (v % 50 == 0)
            engine.seek(static_cast<std::int64_t>(v));
    }
    const std::uint64_t afterPublishing = engine.diagnostics().callbacks;
    while (engine.diagnostics().callbacks < afterPublishing + 200)
        std::this_thread::yield();
    running = false;
    audio.join();
    engine.endProcessing();
    engine.collectGarbage();

    const auto d = engine.diagnostics();
    REQUIRE(d.snapshotLeaks == 0);
    REQUIRE(d.droppedCommands == 0);
    REQUIRE(d.snapshotSwaps >= 2); // the first snapshot and (at least) the final one were adopted
}

TEST_CASE("Mixer gain ramps and track re-matching never touch the heap", "[engine][rt][mixer]")
{
    AudioEngine engine;
    engine.prepare({48000.0, 256, 2});
    engine.beginProcessing();
    const auto asset = makeRamp(96000, 48000.0);

    auto build = [&](std::uint64_t version, float gain) {
        auto snapshot = std::make_unique<PlaybackSnapshot>();
        snapshot->version = version;
        snapshot->sampleRate = 48000.0;
        snapshot->keepAlive.push_back(asset);
        for (std::uint64_t key = 1; key <= 16; ++key) {
            TrackPlayback track;
            track.clips.push_back(ClipPlayback{asset.get(), 0, 0, 96000, 1.0f});
            track.gainLeft = gain;
            track.gainRight = 1.0f - gain;
            track.key = 17 - key; // reversed each time below to force key search
            snapshot->tracks.push_back(std::move(track));
        }
        if (version % 2 == 0)
            std::reverse(snapshot->tracks.begin(), snapshot->tracks.end());
        snapshot->endFrame = 96000;
        return snapshot;
    };

    engine.publishSnapshot(build(1, 1.0f));
    engine.play();
    std::vector<float> l(256), r(256);
    float* outs[2] = {l.data(), r.data()};
    engine.process(outs, 2, 256);

    std::size_t allocations = 0, deallocations = 0;
    for (std::uint64_t version = 2; version < 40; ++version) {
        engine.publishSnapshot(build(version, static_cast<float>(version % 5) / 4.0f)); // control thread
        AllocationGuard guard;
        for (int i = 0; i < 3; ++i)
            engine.process(outs, 2, 256);
        allocations += guard.allocations();
        deallocations += guard.deallocations();
    }
    REQUIRE(allocations == 0);
    REQUIRE(deallocations == 0);
    engine.endProcessing();
}
