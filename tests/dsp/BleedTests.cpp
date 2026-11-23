// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

// Cross-stem bleed suppression (mask sharpening).

#include <daw/dsp/BleedSuppressor.h>

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
constexpr int N = BleedSuppressor::kFftSize;
constexpr int kHop = BleedSuppressor::kHop;

using Signal = std::vector<float>;

Signal noise(std::size_t frames, float rms, unsigned seed)
{
    Signal s(frames);
    std::mt19937 rng(seed);
    std::normal_distribution<float> g(0.0f, rms);
    for (float& v : s)
        v = g(rng);
    return s;
}

Signal sine(std::size_t frames, double hz, double amplitude)
{
    Signal s(frames);
    for (std::size_t i = 0; i < frames; ++i)
        s[i] = static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / kRate));
    return s;
}

Signal add(const Signal& a, const Signal& b)
{
    Signal s(a.size());
    for (std::size_t i = 0; i < a.size(); ++i)
        s[i] = a[i] + b[i];
    return s;
}

/// Runs mono stems through the suppressor in blocks of `hops`; returns every
/// stem's output (same length as the input, delayed by latency()).
std::vector<Signal> run(const std::vector<Signal>& stems, const BleedParams& params, int hops = BleedSuppressor::kMaxHops)
{
    BleedSuppressor suppressor(static_cast<int>(stems.size()));
    suppressor.setParams(params);
    const std::size_t frames = stems.front().size();
    std::vector<Signal> out(stems.size(), Signal(frames, 0.0f));
    const std::size_t block = static_cast<std::size_t>(hops) * kHop;
    std::vector<const float*> in(stems.size());
    std::vector<float*> outs(stems.size());
    for (std::size_t start = 0; start + block <= frames; start += block) {
        for (std::size_t n = 0; n < stems.size(); ++n) {
            in[n] = stems[n].data() + start;
            outs[n] = out[n].data() + start;
        }
        suppressor.process(in.data(), nullptr, outs.data(), nullptr, hops);
    }
    return out;
}

/// Amplitude of `hz` in x[from, to) by correlation with a sine and a cosine.
double toneAmplitude(const Signal& x, double hz, std::size_t from, std::size_t to)
{
    double s = 0.0, c = 0.0;
    for (std::size_t i = from; i < to; ++i) {
        const double phase = 2.0 * std::numbers::pi * hz * static_cast<double>(i) / kRate;
        s += x[i] * std::sin(phase);
        c += x[i] * std::cos(phase);
    }
    return 2.0 * std::sqrt(s * s + c * c) / static_cast<double>(to - from);
}

double db(double ratio)
{
    return 20.0 * std::log10(ratio);
}

} // namespace

TEST_CASE("Exponent 1 reconstructs every stem exactly, delayed by the latency", "[dsp][bleed]")
{
    const std::size_t frames = 64 * static_cast<std::size_t>(kHop);
    const std::vector<Signal> stems{noise(frames, 0.2f, 1), noise(frames, 0.05f, 2), noise(frames, 0.1f, 3)};
    BleedParams params;
    params.exponent = 1.0f;
    const auto out = run(stems, params);
    for (std::size_t n = 0; n < stems.size(); ++n) {
        double error = 0.0;
        for (std::size_t i = N; i < frames; ++i)
            error = std::max(error, static_cast<double>(std::abs(out[n][i] - stems[n][i - N])));
        CHECK(error < 1e-5);
    }
    STATIC_REQUIRE(BleedSuppressor::latency() == N);
}

TEST_CASE("A single stem, or stems that split a bin evenly, are left alone", "[dsp][bleed]")
{
    const std::size_t frames = 32 * static_cast<std::size_t>(kHop);
    const Signal x = noise(frames, 0.1f, 4);
    BleedParams strong;
    strong.exponent = 3.0f;
    strong.reductionDb = 24.0f;
    for (const auto& stems : {std::vector<Signal>{x}, std::vector<Signal>{x, x}}) {
        const auto out = run(stems, strong);
        double error = 0.0;
        for (std::size_t i = N; i < frames; ++i)
            error = std::max(error, static_cast<double>(std::abs(out[0][i] - x[i - N])));
        CHECK(error < 1e-5);
    }
}

TEST_CASE("Bleed is turned down by the reduction limit, a stem's own sound is kept", "[dsp][bleed]")
{
    // Stem A plays 1 kHz. Stem B plays its own 3 kHz and a -30 dB copy of A's tone (bleed).
    const std::size_t frames = 96 * static_cast<std::size_t>(kHop);
    const Signal toneA = sine(frames, 1000.0, 0.5);
    const Signal a = toneA;
    const Signal b = add(sine(frames, 3000.0, 0.2), sine(frames, 1000.0, 0.5 * std::pow(10.0, -30.0 / 20.0)));
    const std::size_t from = 4 * N, to = frames;

    for (const auto& [exponent, reduction] : {std::pair{2.0f, 18.0f}, std::pair{3.0f, 24.0f}, std::pair{1.5f, 12.0f}}) {
        CAPTURE(exponent, reduction);
        BleedParams params;
        params.exponent = exponent;
        params.reductionDb = reduction;
        const auto out = run({a, b}, params);
        // Delay-compensated comparison windows.
        const double bleedBefore = toneAmplitude(b, 1000.0, from - N, to - N);
        const double bleedAfter = toneAmplitude(out[1], 1000.0, from, to);
        CHECK(db(bleedAfter / bleedBefore) == Approx(-reduction).margin(1.0));
        CHECK(db(toneAmplitude(out[1], 3000.0, from, to) / toneAmplitude(b, 3000.0, from - N, to - N)) == Approx(0.0).margin(0.1));
        CHECK(db(toneAmplitude(out[0], 1000.0, from, to) / toneAmplitude(a, 1000.0, from - N, to - N)) == Approx(0.0).margin(0.05));
    }
}

TEST_CASE("Bleed suppression does not depend on the block size", "[dsp][bleed]")
{
    const std::size_t frames = 64 * static_cast<std::size_t>(kHop);
    const std::vector<Signal> stems{add(noise(frames, 0.2f, 5), sine(frames, 440.0, 0.3)), noise(frames, 0.02f, 6)};
    const BleedParams params;
    const auto whole = run(stems, params, BleedSuppressor::kMaxHops);
    const auto single = run(stems, params, 1);
    const auto odd = run(stems, params, 8);
    for (std::size_t n = 0; n < stems.size(); ++n)
        for (std::size_t i = 0; i < frames; ++i) {
            REQUIRE(single[n][i] == Approx(whole[n][i]).margin(1e-6));
            REQUIRE(odd[n][i] == Approx(whole[n][i]).margin(1e-6));
        }
}

TEST_CASE("Bleed suppression keeps stereo channels apart and allocates nothing", "[dsp][bleed]")
{
    const std::size_t frames = 32 * static_cast<std::size_t>(kHop);
    const Signal left = sine(frames, 500.0, 0.4);
    const Signal right = sine(frames, 2000.0, 0.4);
    const Signal other = noise(frames, 0.001f, 7);
    BleedSuppressor suppressor(2);
    Signal outL(frames), outR(frames), outOther(frames);
    {
        test::AllocationGuard guard;
        for (std::size_t start = 0; start < frames; start += 4 * kHop) {
            suppressor.analyse(0, left.data() + start, right.data() + start, 4);
            suppressor.analyse(1, other.data() + start, nullptr, 4);
            suppressor.computeGains();
            suppressor.synthesise(0, outL.data() + start, outR.data() + start);
            suppressor.synthesise(1, outOther.data() + start, nullptr);
        }
    }
    // 200 ms: whole cycles of both tones, so the measurement itself does not leak.
    const std::size_t from = 2 * N, to = from + 9600;
    CHECK(db(toneAmplitude(outL, 500.0, from, to) / 0.4) == Approx(0.0).margin(0.1));
    CHECK(toneAmplitude(outL, 2000.0, from, to) < 1e-4);
    CHECK(db(toneAmplitude(outR, 2000.0, from, to) / 0.4) == Approx(0.0).margin(0.1));
    CHECK(toneAmplitude(outR, 500.0, from, to) < 1e-4);
    // The quiet reference stem holds a tiny share of the tones' bins: floored there.
    BleedParams params;
    CHECK(suppressor.lastGain(1, 500 * N / 48000) == Approx(std::pow(10.0f, -params.reductionDb / 20.0f)));
    CHECK(suppressor.lastGain(0, 500 * N / 48000) == Approx(1.0f));
}
