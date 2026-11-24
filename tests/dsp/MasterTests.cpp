// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

// The master bus: the true-peak limiter keeps every output's true peak at the
// ceiling (inter-sample peaks included), leaves what stays under it
// untouched, and recovers with its release.

#include <daw/dsp/MasterChain.h>
#include <daw/dsp/TruePeak.h>

#include "AllocationGuard.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <vector>

using namespace daw;
using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;

struct Stereo {
    std::vector<float> l, r;
    explicit Stereo(std::size_t frames = 0) : l(frames, 0.0f), r(frames, 0.0f) {}
    std::size_t size() const { return l.size(); }
};

MasterParams limiting(float ceilingDb = -1.0f, float releaseMs = 100.0f)
{
    return {.limiter = true, .ceilingDbtp = ceilingDb, .releaseMs = releaseMs};
}

/// Runs a fresh chain over x plus its latency, in blocks, and returns the output aligned with x.
Stereo run(const Stereo& x, const MasterParams& params, int block = 512)
{
    MasterChain chain;
    chain.prepare(kRate);
    chain.setParams(params);
    chain.reset();
    const auto latency = static_cast<std::size_t>(chain.latency());
    Stereo y(x.size() + latency);
    std::copy(x.l.begin(), x.l.end(), y.l.begin());
    std::copy(x.r.begin(), x.r.end(), y.r.begin());
    for (std::size_t at = 0; at < y.size(); at += static_cast<std::size_t>(block)) {
        const int n = static_cast<int>(std::min<std::size_t>(static_cast<std::size_t>(block), y.size() - at));
        chain.process(y.l.data() + at, y.r.data() + at, n);
    }
    Stereo aligned(x.size());
    std::copy(y.l.begin() + static_cast<std::ptrdiff_t>(latency), y.l.end(), aligned.l.begin());
    std::copy(y.r.begin() + static_cast<std::ptrdiff_t>(latency), y.r.end(), aligned.r.begin());
    return aligned;
}

double truePeakDb(const Stereo& x)
{
    TruePeakDetector meter;
    meter.prepare(kRate, 2);
    const float* channels[2] = {x.l.data(), x.r.data()};
    return 20.0 * std::log10(meter.process(channels, 2, static_cast<int>(x.size())));
}

Stereo sine(double hz, double amplitude, double seconds, double phase = 0.0)
{
    Stereo s(static_cast<std::size_t>(seconds * kRate));
    for (std::size_t i = 0; i < s.size(); ++i)
        s.l[i] = s.r[i] = static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / kRate + phase));
    return s;
}

/// The test signals that stress a true-peak limiter.
std::vector<std::pair<const char*, Stereo>> nastySignals()
{
    std::vector<std::pair<const char*, Stereo>> signals;
    std::mt19937 rng(1);
    std::normal_distribution<float> noise(0.0f, 1.0f);
    Stereo loudNoise(96000);
    for (std::size_t i = 0; i < loudNoise.size(); ++i) {
        loudNoise.l[i] = noise(rng);
        loudNoise.r[i] = noise(rng);
    }
    signals.emplace_back("noise 12 dB over", std::move(loudNoise));
    // A quarter of the sample rate at 45 degrees: every sample sits 3 dB under the true peak.
    signals.emplace_back("fs/4 at 45 degrees", sine(kRate / 4.0, 1.4, 1.0, std::numbers::pi / 4.0));
    Stereo square(96000);
    for (std::size_t i = 0; i < square.size(); ++i)
        square.l[i] = square.r[i] = (i / 240) % 2 == 0 ? 1.8f : -1.8f;
    signals.emplace_back("100 Hz square", std::move(square));
    Stereo hits(96000);
    for (std::size_t i = 0; i < hits.size(); ++i) {
        const double t = static_cast<double>(i % 12000) / kRate;
        hits.l[i] = static_cast<float>(3.0 * std::exp(-t / 0.03) * std::sin(2.0 * std::numbers::pi * 70.0 * t) + 0.3 * noise(rng) * std::exp(-t / 0.005));
        hits.r[i] = hits.l[i];
    }
    signals.emplace_back("drum hits", std::move(hits));
    Stereo clicks(96000);
    for (std::size_t i = 0; i < clicks.size(); i += 4801)
        clicks.l[i] = 2.0f, clicks.r[i + 1] = -2.0f;
    signals.emplace_back("single-sample clicks", std::move(clicks));
    return signals;
}

double thdPercent(const std::vector<float>& x, std::size_t from, std::size_t to, double hz)
{
    // Fundamental by Goertzel; distortion = what remains of the signal's power.
    const double w = 2.0 * std::numbers::pi * hz / kRate;
    double re = 0.0, im = 0.0, total = 0.0;
    for (std::size_t i = from; i < to; ++i) {
        re += x[i] * std::cos(w * static_cast<double>(i));
        im += x[i] * std::sin(w * static_cast<double>(i));
        total += static_cast<double>(x[i]) * x[i];
    }
    const double n = static_cast<double>(to - from);
    const double fundamental = 2.0 * (re * re + im * im) / (n * n); // power of the fundamental, A^2 / 2
    const double rest = std::max(0.0, total / n - fundamental);
    return 100.0 * std::sqrt(rest / fundamental);
}

} // namespace

TEST_CASE("The master chain at unity passes the audio exactly, delayed by its latency", "[dsp][master]")
{
    std::mt19937 rng(2);
    std::uniform_real_distribution<float> u(-1.5f, 1.5f);
    Stereo x(20000);
    for (std::size_t i = 0; i < x.size(); ++i) {
        x.l[i] = u(rng);
        x.r[i] = u(rng);
    }
    const Stereo y = run(x, MasterParams{}); // limiter off: overs pass
    CHECK(y.l == x.l);
    CHECK(y.r == x.r);
    MasterChain chain;
    chain.prepare(kRate);
    CHECK(chain.latency() == SoftClipper::latency() + static_cast<int>(std::lround(0.005 * kRate)) - 1 + 8); // clipper + 5 ms window + interpolator
}

TEST_CASE("The limiter keeps the true peak at the ceiling, inter-sample peaks included", "[dsp][master]")
{
    for (auto& [name, x] : nastySignals()) {
        INFO(name);
        for (const float ceiling : {-1.0f, -0.1f, -6.0f}) {
            INFO("ceiling " << ceiling);
            CHECK(truePeakDb(run(x, limiting(ceiling))) <= ceiling + 0.02);
        }
    }
}

TEST_CASE("Audio under the ceiling passes the limiter untouched", "[dsp][master]")
{
    const Stereo x = sine(1000.0, 0.5, 1.0); // -6 dBFS
    const Stereo y = run(x, limiting(-1.0f));
    CHECK(y.l == x.l);
}

TEST_CASE("A steady loud tone settles exactly at the ceiling, and the gain recovers after a burst", "[dsp][master]")
{
    const Stereo x = sine(1000.0, 2.0, 1.0); // +6 dBFS
    const Stereo y = run(x, limiting(-1.0f));
    float peak = 0.0f;
    for (std::size_t i = 24000; i < y.size(); ++i)
        peak = std::max(peak, std::abs(y.l[i]));
    CHECK(20.0 * std::log10(peak) == Approx(-1.0).margin(0.1));

    // A 100 ms burst, then a quiet tone: within 3 release times the gain is back within 0.5 dB.
    Stereo burst = sine(1000.0, 0.1, 1.5);
    for (std::size_t i = 4800; i < 9600; ++i)
        burst.l[i] = burst.r[i] = burst.l[i] * 40.0f;
    const Stereo out = run(burst, limiting(-1.0f, 100.0f));
    const std::size_t later = 9600 + static_cast<std::size_t>(0.3 * kRate);
    float quiet = 0.0f;
    for (std::size_t i = later; i < later + 4800; ++i)
        quiet = std::max(quiet, std::abs(out.l[i]));
    CHECK(20.0 * std::log10(quiet / 0.1) > -0.5);
}

TEST_CASE("The limiter does not depend on the block size", "[dsp][master]")
{
    auto signals = nastySignals();
    const Stereo& x = signals[3].second;
    const Stereo a = run(x, limiting(-1.0f), 64);
    const Stereo b = run(x, limiting(-1.0f), 1000);
    CHECK(a.l == b.l);
    CHECK(a.r == b.r);
}

TEST_CASE("Driven far into the ceiling, the limiter still never goes over", "[dsp][master]")
{
    // A 500 Hz tone 24 dB over a -1 dBTP ceiling, and the drum hits 24 dB hotter.
    CHECK(truePeakDb(run(sine(500.0, 14.0, 1.0), limiting(-1.0f))) <= -1.0 + 0.02);
    auto signals = nastySignals();
    Stereo hot = signals[3].second;
    for (std::size_t i = 0; i < hot.size(); ++i)
        hot.l[i] = hot.r[i] = hot.l[i] * 16.0f;
    CHECK(truePeakDb(run(hot, limiting(-1.0f))) <= -1.0 + 0.02);
}

TEST_CASE("The master chain never touches the heap", "[dsp][master][rt]")
{
    MasterChain chain;
    chain.prepare(kRate);
    Stereo x = sine(100.0, 2.0, 0.5);
    {
        test::AllocationGuard guard;
        chain.setParams(limiting(-1.0f));
        chain.process(x.l.data(), x.r.data(), 12000);
        chain.setParams(limiting(-0.3f, 300.0f));
        chain.reset();
        chain.process(x.l.data() + 12000, x.r.data() + 12000, 12000);
        CHECK(guard.allocations() == 0);
        CHECK(guard.deallocations() == 0);
    }
}
