// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

// The per-track clean-up chain: lookahead alignment, high-pass response, DC
// removal, the expander's curve and timing, block-size invariance and
// real-time safety.

#include <daw/dsp/CleanupChain.h>

#include "AllocationGuard.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <numbers>
#include <random>
#include <vector>

using namespace daw;
using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;

struct Stereo {
    std::vector<float> left, right;
    explicit Stereo(std::size_t frames = 0) : left(frames, 0.0f), right(frames, 0.0f) {}
    std::size_t size() const { return left.size(); }
};

Stereo sine(double hz, double seconds, double amplitude, double rate = kRate)
{
    Stereo s(static_cast<std::size_t>(seconds * rate));
    for (std::size_t i = 0; i < s.size(); ++i)
        s.left[i] = s.right[i] = static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / rate));
    return s;
}

/// Processes `input` through a freshly prepared chain in blocks of `block`.
Stereo run(const CleanupParams& params, Stereo input, int block = 512, double rate = kRate)
{
    CleanupChain chain;
    chain.prepare(rate);
    chain.setParams(params);
    for (std::size_t at = 0; at < input.size(); at += static_cast<std::size_t>(block)) {
        const int n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), input.size() - at));
        chain.process(input.left.data() + at, input.right.data() + at, n);
    }
    return input;
}

double rmsDb(const std::vector<float>& x, std::size_t from, std::size_t to)
{
    double sum = 0.0;
    for (std::size_t i = from; i < to; ++i)
        sum += static_cast<double>(x[i]) * x[i];
    return 10.0 * std::log10(sum / static_cast<double>(to - from) + 1e-30);
}

/// Steady-state gain of the high-pass at `hz`, in dB.
double highPassGainDb(double hz, double cutoff, int order, double rate = kRate)
{
    CleanupParams p;
    p.highPass = true;
    p.highPassHz = static_cast<float>(cutoff);
    p.highPassOrder = order;
    const double seconds = std::max(1.0, 40.0 / hz);
    const Stereo out = run(p, sine(hz, seconds, 0.5, rate), 512, rate);
    const std::size_t half = out.size() / 2;
    return rmsDb(out.left, half, out.size()) - 20.0 * std::log10(0.5 / std::sqrt(2.0));
}

} // namespace

TEST_CASE("The chain delays its output by exactly its latency: FFT frame plus lookahead", "[dsp][cleanup]")
{
    CleanupChain chain;
    chain.prepare(kRate);
    CHECK(chain.latency()
          == SpectralDenoiser::kFftSize + static_cast<int>(std::lround(kRate * CleanupChain::kLookaheadSeconds)));

    CleanupParams p;
    p.expander = true;
    p.ratio = 1.0f; // a 1:1 expander is transparent
    Stereo x(8192);
    x.left[100] = 0.5f;
    x.right[100] = -0.25f;
    const Stereo y = run(p, x, 64);
    const auto latency = static_cast<std::size_t>(chain.latency());
    for (std::size_t i = 0; i < y.size(); ++i) {
        CHECK(y.left[i] == (i == 100 + latency ? 0.5f : 0.0f));
        CHECK(y.right[i] == (i == 100 + latency ? -0.25f : 0.0f));
    }
}

TEST_CASE("High-pass slopes are Butterworth: -3 dB at the cutoff, 12/24/48 dB per octave below", "[dsp][cleanup]")
{
    for (const int order : {2, 4, 8}) {
        INFO("order " << order);
        CHECK(highPassGainDb(120.0, 120.0, order) == Approx(-3.01).margin(0.15));
        CHECK(highPassGainDb(2000.0, 120.0, order) == Approx(0.0).margin(0.05));
        // One octave below: |H| = 1 / sqrt(1 + 2^(2n)) for an nth-order Butterworth.
        const double expected = -10.0 * std::log10(1.0 + std::pow(2.0, 2.0 * order));
        CHECK(highPassGainDb(60.0, 120.0, order) == Approx(expected).margin(0.3));
    }
    // Extreme settings stay stable.
    CHECK(highPassGainDb(5000.0, 20.0, 8, 192000.0) == Approx(0.0).margin(0.05));
    CHECK(std::isfinite(highPassGainDb(2000.0, 1000.0, 8, 8000.0)));
}

TEST_CASE("DC removal takes out a constant offset and keeps the music", "[dsp][cleanup]")
{
    CleanupParams p;
    p.dcRemoval = true;
    Stereo x = sine(440.0, 2.0, 0.3);
    for (std::size_t i = 0; i < x.size(); ++i) {
        x.left[i] += 0.2f;
        x.right[i] -= 0.1f;
    }
    const Stereo y = run(p, x);
    double meanL = 0.0, meanR = 0.0;
    const std::size_t from = y.size() / 2;
    for (std::size_t i = from; i < y.size(); ++i) {
        meanL += y.left[i];
        meanR += y.right[i];
    }
    meanL /= static_cast<double>(y.size() - from);
    meanR /= static_cast<double>(y.size() - from);
    CHECK(std::abs(meanL) < 1e-3);
    CHECK(std::abs(meanR) < 1e-3);
    CHECK(rmsDb(y.left, from, y.size()) == Approx(20.0 * std::log10(0.3 / std::sqrt(2.0))).margin(0.05));
}

TEST_CASE("Expander curve: flat above the threshold, ratio below, limited by the range", "[dsp][cleanup]")
{
    const double t = -50.0, ratio = 4.0, range = 24.0;
    CHECK(CleanupChain::expanderGainDb(-20.0, t, ratio, range) == 0.0);
    CHECK(CleanupChain::expanderGainDb(t + CleanupChain::kKneeDb / 2.0, t, ratio, range) == 0.0);
    CHECK(CleanupChain::expanderGainDb(-56.0, t, ratio, range) == Approx(-18.0)); // 6 dB below: 3 x 6
    CHECK(CleanupChain::expanderGainDb(-80.0, t, ratio, range) == Approx(-24.0)); // capped
    CHECK(CleanupChain::expanderGainDb(-56.0, t, 1.0, range) == 0.0);             // ratio 1 does nothing
    // The soft knee is continuous at both ends and monotonic in between.
    const double halfKnee = CleanupChain::kKneeDb / 2.0;
    CHECK(CleanupChain::expanderGainDb(t - halfKnee + 1e-9, t, ratio, range)
          == Approx(CleanupChain::expanderGainDb(t - halfKnee - 1e-9, t, ratio, range)).margin(1e-6));
    double previous = -1000.0;
    for (double level = t - 5.0; level <= t + 5.0; level += 0.25) {
        const double g = CleanupChain::expanderGainDb(level, t, ratio, range);
        CHECK(g >= previous);
        previous = g;
    }
}

TEST_CASE("Expander pushes a noise bed down and opens in time for the note", "[dsp][cleanup]")
{
    // 1 s of noise at -50 dBFS RMS, then a -10 dBFS tone with the same noise.
    const std::size_t bed = static_cast<std::size_t>(kRate);
    const std::size_t total = bed * 2;
    Stereo x(total);
    std::mt19937 rng(5);
    std::normal_distribution<float> noise(0.0f, static_cast<float>(std::pow(10.0, -50.0 / 20.0)));
    const double toneAmp = std::pow(10.0, -10.0 / 20.0) * std::sqrt(2.0);
    for (std::size_t i = 0; i < total; ++i) {
        const float tone = i >= bed ? static_cast<float>(toneAmp * std::sin(2.0 * std::numbers::pi * 220.0 * static_cast<double>(i - bed) / kRate)) : 0.0f;
        x.left[i] = noise(rng) + tone;
        x.right[i] = noise(rng) + tone;
    }

    CleanupParams p;
    p.expander = true;
    p.thresholdDb = -38.0f; // 12 dB above the bed: 36 dB of expansion, capped by the range
    p.ratio = 4.0f;
    p.rangeDb = 24.0f;
    p.releaseMs = 50.0f;
    const Stereo y = run(p, x, 256);
    CleanupChain probe;
    probe.prepare(kRate);
    const auto latency = static_cast<std::size_t>(probe.latency());

    // The bed (after the expander settles) drops by the full range.
    const double bedIn = rmsDb(x.left, bed / 2, bed - latency);
    const double bedOut = rmsDb(y.left, bed / 2 + latency, bed);
    CHECK(bedIn - bedOut == Approx(24.0).margin(0.5));

    // The note comes through untouched from its first millisecond on (thanks to the lookahead).
    const std::size_t onset = bed + latency;
    const std::size_t ms = static_cast<std::size_t>(kRate / 1000.0);
    double worst = 0.0;
    for (std::size_t i = onset + ms / 4; i < onset + 20 * ms; ++i)
        worst = std::max(worst, std::abs(static_cast<double>(y.left[i]) - x.left[i - latency]));
    CHECK(worst < toneAmp * 0.03); // within 0.3 dB of the dry note
    CHECK(rmsDb(y.left, onset, onset + bed / 2) == Approx(rmsDb(x.left, bed, bed + bed / 2)).margin(0.01));
}

TEST_CASE("Clean-up output does not depend on the block size", "[dsp][cleanup]")
{
    CleanupParams p;
    p.dcRemoval = true;
    p.highPass = true;
    p.highPassHz = 90.0f;
    p.highPassOrder = 8;
    p.expander = true;
    p.thresholdDb = -30.0f;
    Stereo x(48000);
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    for (std::size_t i = 0; i < x.size(); ++i) {
        const float envelope = (i / 4800) % 2 == 0 ? 0.5f : 0.002f;
        x.left[i] = u(rng) * envelope;
        x.right[i] = u(rng) * envelope;
    }
    const Stereo a = run(p, x, 512);
    for (const int block : {1, 37, 64, 4096}) {
        const Stereo b = run(p, x, block);
        CHECK(a.left == b.left);
        CHECK(a.right == b.right);
    }
}

TEST_CASE("Changing settings keeps the chain stable; reset() clears it", "[dsp][cleanup]")
{
    CleanupChain chain;
    chain.prepare(kRate);
    CleanupParams p;
    p.highPass = true;
    Stereo x = sine(80.0, 1.0, 0.5);
    for (std::size_t at = 0; at < x.size(); at += 480) {
        p.highPassHz = 20.0f + static_cast<float>((at / 480) % 50) * 20.0f; // sweep 20 Hz .. 1 kHz
        p.highPassOrder = (at / 4800) % 2 == 0 ? 4 : 8;
        chain.setParams(p);
        chain.process(x.left.data() + at, x.right.data() + at, 480);
    }
    for (float v : x.left)
        REQUIRE(std::isfinite(v));
    for (float v : x.left)
        CHECK(std::abs(v) < 2.0f);

    chain.reset();
    Stereo silence(1024);
    chain.process(silence.left.data(), silence.right.data(), 1024);
    for (float v : silence.left)
        CHECK(v == 0.0f);
}

TEST_CASE("The clean-up chain never touches the heap after prepare()", "[dsp][cleanup][rt]")
{
    CleanupChain chain;
    chain.prepare(kRate);
    Stereo x = sine(100.0, 0.5, 0.5);
    CleanupParams p;
    p.dcRemoval = p.highPass = p.expander = true;
    {
        test::AllocationGuard guard;
        chain.setParams(p);
        chain.process(x.left.data(), x.right.data(), static_cast<int>(x.size()));
        p.highPassOrder = 8;
        chain.setParams(p);
        chain.reset();
        chain.process(x.left.data(), x.right.data(), static_cast<int>(x.size()));
        CHECK(guard.allocations() == 0);
        CHECK(guard.deallocations() == 0);
    }
}
