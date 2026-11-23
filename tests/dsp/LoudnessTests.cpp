// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

// Loudness conformance after EBU Tech 3341 (loudness metering) and Tech 3342
// (loudness range). The reference signals are generated here as 1 kHz stereo
// sines at the levels the EBU test set specifies; the tolerances are the EBU
// ones (+-0.1 LU for loudness, +-1 LU for range).

#include "AllocationGuard.h"

#include <daw/dsp/LoudnessMeter.h>
#include <daw/dsp/TruePeak.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <numbers>
#include <utility>
#include <vector>

using namespace daw;
using Catch::Approx;

namespace {

struct Segment {
    double seconds;
    double levelDbfs; // sine peak level, both channels
};

/// Phase-continuous 1 kHz stereo sine stepping through `segments`.
std::vector<std::vector<float>> sineProgramme(const std::vector<Segment>& segments, double rate, int channels = 2,
                                              double frequency = 1000.0)
{
    std::vector<float> mono;
    double phase = 0.0;
    for (const Segment& s : segments) {
        const double amplitude = std::pow(10.0, s.levelDbfs / 20.0);
        const auto frames = static_cast<std::int64_t>(std::llround(s.seconds * rate));
        for (std::int64_t i = 0; i < frames; ++i) {
            mono.push_back(static_cast<float>(amplitude * std::sin(phase)));
            phase += 2.0 * std::numbers::pi * frequency / rate;
        }
    }
    return std::vector<std::vector<float>>(static_cast<std::size_t>(channels), mono);
}

LoudnessReading measure(const std::vector<std::vector<float>>& audio, double rate, int block = 1024, bool truePeak = false)
{
    LoudnessMeter meter;
    meter.prepare(rate, static_cast<int>(audio.size()), truePeak);
    const auto frames = static_cast<int>(audio.front().size());
    std::vector<const float*> pointers(audio.size());
    for (int done = 0; done < frames;) {
        const int n = std::min(block, frames - done);
        for (std::size_t c = 0; c < audio.size(); ++c)
            pointers[c] = audio[c].data() + done;
        meter.process(pointers.data(), static_cast<int>(pointers.size()), n);
        done += n;
    }
    return meter.reading();
}

} // namespace

TEST_CASE("K-weighting at 48 kHz equals the BS.1770-4 coefficients", "[dsp][loudness]")
{
    const auto stages = kWeightingFilters(48000.0);
    CHECK(stages[0].b0 == Approx(1.53512485958697).margin(1e-7));
    CHECK(stages[0].b1 == Approx(-2.69169618940638).margin(1e-7));
    CHECK(stages[0].b2 == Approx(1.19839281085285).margin(1e-7));
    CHECK(stages[0].a1 == Approx(-1.69065929318241).margin(1e-7));
    CHECK(stages[0].a2 == Approx(0.73248077421585).margin(1e-7));
    CHECK(stages[1].b0 == 1.0);
    CHECK(stages[1].b1 == -2.0);
    CHECK(stages[1].b2 == 1.0);
    CHECK(stages[1].a1 == Approx(-1.99004745483398).margin(1e-7));
    CHECK(stages[1].a2 == Approx(0.99007225036621).margin(1e-7));
}

TEST_CASE("EBU 3341 tests 1 and 2: steady sines read their level", "[dsp][loudness]")
{
    for (const double rate : {44100.0, 48000.0}) {
        for (const double level : {-23.0, -33.0}) {
            const auto r = measure(sineProgramme({{20.0, level}}, rate), rate);
            CHECK(r.momentaryLufs == Approx(level).margin(0.1));
            CHECK(r.shortTermLufs == Approx(level).margin(0.1));
            CHECK(r.integratedLufs == Approx(level).margin(0.1));
            CHECK(r.loudnessRangeLu == Approx(0.0).margin(0.1));
            CHECK(r.seconds == Approx(20.0).margin(1e-6));
        }
    }
}

TEST_CASE("EBU 3341 tests 3 to 5: gating", "[dsp][loudness]")
{
    const double rate = 48000.0;
    // Test 3: the -36 dBFS parts fall below the relative gate.
    CHECK(measure(sineProgramme({{10, -36}, {60, -23}, {10, -36}}, rate), rate).integratedLufs == Approx(-23.0).margin(0.1));
    // Test 4: the -72 dBFS parts fall below the absolute gate as well.
    CHECK(measure(sineProgramme({{10, -72}, {10, -36}, {60, -23}, {10, -36}, {10, -72}}, rate), rate).integratedLufs
          == Approx(-23.0).margin(0.1));
    // Test 5: everything passes both gates; the average is -23.
    CHECK(measure(sineProgramme({{20, -26}, {20.1, -20}, {20, -26}}, rate), rate).integratedLufs == Approx(-23.0).margin(0.1));
}

TEST_CASE("EBU 3342: loudness range", "[dsp][loudness]")
{
    const double rate = 48000.0;
    CHECK(measure(sineProgramme({{20, -20}, {20, -30}}, rate), rate).loudnessRangeLu == Approx(10.0).margin(1.0));
    CHECK(measure(sineProgramme({{20, -20}, {20, -15}}, rate), rate).loudnessRangeLu == Approx(5.0).margin(1.0));
    CHECK(measure(sineProgramme({{20, -40}, {20, -20}}, rate), rate).loudnessRangeLu == Approx(20.0).margin(1.0));
    CHECK(measure(sineProgramme({{20, -50}, {20, -35}, {20, -20}, {20, -35}, {20, -50}}, rate), rate).loudnessRangeLu
          == Approx(15.0).margin(1.0));
}

TEST_CASE("A mono programme reads 3 dB below the same signal on both stereo channels", "[dsp][loudness]")
{
    const double rate = 48000.0;
    const auto stereo = measure(sineProgramme({{5, -20}}, rate, 2), rate);
    const auto mono = measure(sineProgramme({{5, -20}}, rate, 1), rate);
    CHECK(stereo.integratedLufs - mono.integratedLufs == Approx(10.0 * std::log10(2.0)).margin(0.01));
}

TEST_CASE("Loudness readings do not depend on the block size", "[dsp][loudness]")
{
    const double rate = 44100.0;
    const auto audio = sineProgramme({{4, -30}, {4, -18}, {4, -40}}, rate);
    const auto reference = measure(audio, rate, 1 << 20, true);
    for (const int block : {1, 17, 64, 441, 4410, 9000}) {
        const auto r = measure(audio, rate, block, true);
        CHECK(r.integratedLufs == Approx(reference.integratedLufs).margin(1e-9));
        CHECK(r.momentaryLufs == Approx(reference.momentaryLufs).margin(1e-9));
        CHECK(r.shortTermLufs == Approx(reference.shortTermLufs).margin(1e-9));
        CHECK(r.loudnessRangeLu == Approx(reference.loudnessRangeLu).margin(1e-9));
        CHECK(r.truePeakDbtp == Approx(reference.truePeakDbtp).margin(1e-6));
    }
}

TEST_CASE("Silence reads as the floor, and reset() starts a new measurement", "[dsp][loudness]")
{
    const double rate = 48000.0;
    LoudnessMeter meter;
    meter.prepare(rate, 2);
    const auto silent = sineProgramme({{2, -200}}, rate);
    const float* channels[2] = {silent[0].data(), silent[1].data()};
    meter.process(channels, 2, static_cast<int>(silent[0].size()));
    CHECK(meter.reading().integratedLufs == kLoudnessFloor);
    CHECK(meter.reading().truePeakDbtp <= -140.0);

    const auto tone = sineProgramme({{20, -10}}, rate); // long enough that the blocks straddling the start hardly count
    const float* toneChannels[2] = {tone[0].data(), tone[1].data()};
    meter.process(toneChannels, 2, static_cast<int>(tone[0].size()));
    CHECK(meter.reading().integratedLufs == Approx(-10.0).margin(0.1));
    meter.reset();
    CHECK(meter.reading().integratedLufs == kLoudnessFloor);
    CHECK(meter.reading().momentaryLufs == kLoudnessFloor);
    CHECK(meter.reading().seconds == 0.0);
}

TEST_CASE("True peak finds the inter-sample peak a sample meter misses", "[dsp][truepeak]")
{
    for (const double rate : {44100.0, 48000.0, 96000.0}) {
        // A sine at a quarter of the sample rate, 45 degrees off: every sample
        // lands at +-0.707 while the waveform between them reaches 1.0.
        const int frames = static_cast<int>(rate / 10);
        std::vector<float> wave(static_cast<std::size_t>(frames));
        for (int i = 0; i < frames; ++i)
            wave[static_cast<std::size_t>(i)] = static_cast<float>(std::sin(std::numbers::pi / 2.0 * i + std::numbers::pi / 4.0));
        const float* channels[1] = {wave.data()};
        LoudnessMeter meter;
        meter.prepare(rate, 1);
        meter.process(channels, 1, frames);
        if (rate < 96000.0) {
            CHECK(meter.reading().samplePeakDbfs == Approx(-3.01).margin(0.01));
            CHECK(meter.reading().truePeakDbtp == Approx(0.0).margin(0.2));
        }
    }

    // A slow sine: true peak equals the sample peak.
    const auto tone = sineProgramme({{1, -6}}, 48000.0, 1, 997.0);
    const float* channels[1] = {tone[0].data()};
    TruePeakDetector detector;
    detector.prepare(48000.0, 1);
    REQUIRE(detector.oversampling() == 4);
    CHECK(20.0 * std::log10(detector.process(channels, 1, static_cast<int>(tone[0].size()))) == Approx(-6.0).margin(0.05));
}

TEST_CASE("The loudness meter never touches the heap after prepare()", "[dsp][loudness][rt]")
{
    LoudnessMeter meter;
    meter.prepare(48000.0, 2);
    const auto audio = sineProgramme({{4, -20}}, 48000.0);
    std::size_t allocations = 0;
    {
        test::AllocationGuard guard;
        for (std::size_t done = 0; done + 512 <= audio[0].size(); done += 512) {
            const float* channels[2] = {audio[0].data() + done, audio[1].data() + done};
            meter.process(channels, 2, 512);
        }
        meter.reset();
        allocations = guard.allocations() + guard.deallocations();
    }
    REQUIRE(allocations == 0);
}
