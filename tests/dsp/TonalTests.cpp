// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

// Tonal balance and the master EQ: the shelf and bell sections have the
// gains they promise, the master EQ applies them, profiles put energy in the
// right bands and ignore quiet stretches, targets average references, and a
// mix filtered by a known EQ gets that EQ's inverse suggested.

#include <daw/dsp/MasterChain.h>
#include <daw/dsp/TonalBalance.h>

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

/// Pink-ish noise (white noise through a -3 dB/octave filter, Kellet's approximation).
Stereo pinkNoise(double seconds, unsigned seed, float level = 0.1f)
{
    Stereo s(static_cast<std::size_t>(seconds * kRate));
    std::mt19937 rng(seed);
    std::normal_distribution<float> n(0.0f, 1.0f);
    for (auto* channel : {&s.l, &s.r}) {
        double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
        for (float& v : *channel) {
            const double w = n(rng);
            b0 = 0.99886 * b0 + w * 0.0555179;
            b1 = 0.99332 * b1 + w * 0.0750759;
            b2 = 0.96900 * b2 + w * 0.1538520;
            b3 = 0.86650 * b3 + w * 0.3104856;
            b4 = 0.55000 * b4 + w * 0.5329522;
            b5 = -0.7616 * b5 - w * 0.0168980;
            v = static_cast<float>(level * 0.2 * (b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362));
            b6 = w * 0.115926;
        }
    }
    return s;
}

TonalProfile profileOf(const Stereo& s)
{
    TonalAnalyzer analyzer(kRate);
    analyzer.process(s.l.data(), s.r.data(), static_cast<int>(s.size()));
    return analyzer.result();
}

Stereo throughEq(Stereo s, const std::array<float, kMasterEqBandCount>& gains)
{
    MasterChain chain;
    chain.prepare(kRate);
    MasterParams p;
    p.eq = true;
    p.eqGainDb = gains;
    chain.setParams(p);
    chain.process(s.l.data(), s.r.data(), static_cast<int>(s.size()));
    // Drop the limiter's (idle) delay so the audio lines up again; it does not matter for spectra.
    return s;
}

std::size_t bandOf(double hz)
{
    return static_cast<std::size_t>(std::min_element(kTonalBandHz.begin(), kTonalBandHz.end(),
                                                     [&](double a, double b) { return std::abs(std::log2(a / hz)) < std::abs(std::log2(b / hz)); })
                                    - kTonalBandHz.begin());
}

} // namespace

TEST_CASE("Shelf and bell sections have the gains they promise", "[dsp][tonal]")
{
    const auto bell = BiquadCoefficients::peaking(1000.0, 0.8, 6.0, kRate);
    CHECK(bell.magnitudeDb(1000.0, kRate) == Approx(6.0).margin(0.01));
    CHECK(std::abs(bell.magnitudeDb(20.0, kRate)) < 0.1);
    CHECK(std::abs(bell.magnitudeDb(18000.0, kRate)) < 0.2);
    const auto low = BiquadCoefficients::lowShelf(100.0, 0.7071, -4.0, kRate);
    CHECK(low.magnitudeDb(10.0, kRate) == Approx(-4.0).margin(0.05));
    CHECK(low.magnitudeDb(100.0, kRate) == Approx(-2.0).margin(0.05)); // half the gain at the corner
    CHECK(std::abs(low.magnitudeDb(2000.0, kRate)) < 0.05);
    const auto high = BiquadCoefficients::highShelf(8000.0, 0.7071, 3.0, kRate);
    CHECK(high.magnitudeDb(20000.0, kRate) == Approx(3.0).margin(0.15));
    CHECK(high.magnitudeDb(8000.0, kRate) == Approx(1.5).margin(0.05));
    CHECK(std::abs(high.magnitudeDb(500.0, kRate)) < 0.05);
    // No overshoot: a shelf never goes past its own gain.
    for (double hz = 20.0; hz < 20000.0; hz *= 1.1)
        CHECK(low.magnitudeDb(hz, kRate) >= -4.0 - 1e-6);
}

TEST_CASE("The master EQ applies its gains and is transparent at 0 dB", "[dsp][tonal][master]")
{
    const Stereo noise = pinkNoise(4.0, 1);
    const TonalProfile flat = profileOf(noise);
    // Off, or on at 0 dB: the audio passes untouched (the limiter, off, only delays it).
    const TonalProfile zero = profileOf(throughEq(noise, {}));
    for (std::size_t b = 0; b < kTonalBands; ++b)
        CHECK(zero.levelDb[b] == Approx(flat.levelDb[b]).margin(0.05)); // the frames fall 5 ms later
    // +6 dB in the air band: the top bands rise against the midrange, the lows do not move.
    const TonalProfile bright = profileOf(throughEq(noise, {0.0f, 0.0f, 0.0f, 0.0f, 6.0f}));
    const auto relative = [&](const TonalProfile& p, double hz) { return p.levelDb[bandOf(hz)] - p.levelDb[bandOf(1000.0)]; };
    CHECK(relative(bright, 16000.0) - relative(flat, 16000.0) == Approx(6.0).margin(0.5));
    CHECK(std::abs(relative(bright, 100.0) - relative(flat, 100.0)) < 0.2);
}

TEST_CASE("A profile puts energy in the right band and ignores quiet stretches", "[dsp][tonal]")
{
    // A 1 kHz tone with a quiet 2 kHz tone 40 dB down, then a long stretch of
    // a 125 Hz tone 30 dB under the song's loud part: the gate leaves it out.
    Stereo s(static_cast<std::size_t>(6.0 * kRate));
    for (std::size_t i = 0; i < s.size(); ++i) {
        const double t = static_cast<double>(i) / kRate;
        const double v = t < 3.0 ? 0.5 * std::sin(2.0 * std::numbers::pi * 1000.0 * t) + 0.005 * std::sin(2.0 * std::numbers::pi * 2000.0 * t)
                                 : 0.016 * std::sin(2.0 * std::numbers::pi * 125.0 * t);
        s.l[i] = s.r[i] = static_cast<float>(v);
    }
    const TonalProfile p = profileOf(s);
    REQUIRE(p.valid());
    CHECK(p.levelDb[bandOf(1000.0)] > -0.1);                     // nearly everything
    CHECK(p.levelDb[bandOf(2000.0)] == Approx(-40.0).margin(1.0)); // the quiet tone, where it belongs
    CHECK(p.levelDb[bandOf(125.0)] < -45.0); // ungated it would be -30 dB; only the frame straddling the change counts
    CHECK(p.seconds == Approx(3.0).margin(0.2));
}

TEST_CASE("A target averages its references and records how much they disagree", "[dsp][tonal]")
{
    const TonalProfile a = profileOf(pinkNoise(3.0, 2));
    const TonalProfile b = profileOf(throughEq(pinkNoise(3.0, 3), {4.0f, 0.0f, 0.0f, 0.0f, 0.0f}));
    const TonalTarget target = makeTonalTarget("refs", {"a.wav", "b.wav"}, {a, b});
    REQUIRE(target.valid());
    const std::size_t low = bandOf(50.0), mid = bandOf(1000.0);
    CHECK(target.meanDb[low] == Approx(0.5 * (a.levelDb[low] + b.levelDb[low])).margin(1e-9));
    CHECK(target.spreadDb[low] > target.spreadDb[mid]); // they disagree about the lows
    CHECK(target.spreadDb[mid] >= TonalTarget::kMinSpreadDb);
    CHECK(makeTonalTarget("one", {"a.wav"}, {a}).spreadDb[mid] == TonalTarget::kSingleSpreadDb);
    CHECK_FALSE(makeTonalTarget("none", {}, {TonalProfile{}}).valid());
}

TEST_CASE("A mix filtered by a known EQ gets that EQ's inverse suggested", "[dsp][tonal]")
{
    // Three references: pink noise. The "mix": pink noise through a known master EQ.
    std::vector<TonalProfile> refs;
    for (unsigned seed = 10; seed < 13; ++seed)
        refs.push_back(profileOf(pinkNoise(4.0, seed)));
    const TonalTarget target = makeTonalTarget("pink", {"1", "2", "3"}, refs);
    const std::array<float, kMasterEqBandCount> colour{3.0f, -2.0f, 0.0f, 2.5f, -3.0f};
    const TonalProfile mix = profileOf(throughEq(pinkNoise(4.0, 20), colour));
    const TonalComparison cmp = compareTonal(mix, target);
    CHECK(cmp.rmsBeforeDb > 1.0);
    for (std::size_t j = 0; j < kMasterEqBandCount; ++j) {
        INFO("band " << j);
        CHECK(cmp.eqGainDb[j] == Approx(-colour[j]).margin(1.01));
    }
    CHECK(cmp.rmsAfterDb < 0.5 * cmp.rmsBeforeDb);

    // Applied on top, the measured deviation is what the comparison predicted.
    const TonalProfile fixed = profileOf(throughEq(throughEq(pinkNoise(4.0, 20), colour), cmp.eqGainDb));
    const TonalComparison again = compareTonal(fixed, target, cmp.eqGainDb);
    CHECK(again.rmsBeforeDb == Approx(cmp.rmsAfterDb).margin(0.3));
    // And with the EQ in place, the same EQ is suggested (no creeping further).
    for (std::size_t j = 0; j < kMasterEqBandCount; ++j)
        CHECK(again.eqGainDb[j] == Approx(cmp.eqGainDb[j]).margin(0.51));
}
