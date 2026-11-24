// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

// The master bus's soft clipper and multiband compressor: the clipper is a
// plain delay when off, transparent below its knee, never far past its clip
// level, keeps its harmonics from folding back thanks to oversampling, and
// counts what it touches; the multiband's bands add back up flat, compress
// by the static curve, and leave the other bands alone.

#include <daw/dsp/Clipper.h>
#include <daw/dsp/Fft.h>
#include <daw/dsp/MasterChain.h>
#include <daw/dsp/Multiband.h>

#include "AllocationGuard.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <random>
#include <vector>

using namespace daw;
using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;

std::vector<float> sine(double hz, double amplitude, std::size_t frames)
{
    std::vector<float> x(frames);
    for (std::size_t i = 0; i < frames; ++i)
        x[i] = static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / kRate));
    return x;
}

/// Amplitude of `hz` in x[from, to) (Goertzel, normalised for a sine).
double toneAmplitude(const std::vector<float>& x, std::size_t from, std::size_t to, double hz)
{
    const double w = 2.0 * std::numbers::pi * hz / kRate;
    double re = 0.0, im = 0.0;
    for (std::size_t i = from; i < to; ++i) {
        re += x[i] * std::cos(w * static_cast<double>(i));
        im += x[i] * std::sin(w * static_cast<double>(i));
    }
    return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(to - from);
}

double db(double ratio)
{
    return 20.0 * std::log10(ratio);
}

} // namespace

TEST_CASE("The clipper off is a plain delay of its latency", "[dsp][clipper]")
{
    std::mt19937 rng(1);
    std::uniform_real_distribution<float> u(-2.0f, 2.0f);
    std::vector<float> l(4000), r(4000);
    for (std::size_t i = 0; i < l.size(); ++i) {
        l[i] = u(rng);
        r[i] = u(rng);
    }
    SoftClipper clipper;
    clipper.prepare(kRate);
    clipper.setParams(false, -2.0f);
    std::vector<float> yl = l, yr = r;
    clipper.process(yl.data(), yr.data(), static_cast<int>(yl.size()));
    const auto latency = static_cast<std::size_t>(SoftClipper::latency());
    for (std::size_t i = latency; i < l.size(); ++i) {
        REQUIRE(yl[i] == l[i - latency]);
        REQUIRE(yr[i] == r[i - latency]);
    }
}

TEST_CASE("Under its knee the clipper passes the audio, delayed and unchanged", "[dsp][clipper]")
{
    const auto x = sine(1000.0, 0.5, 48000); // -6 dBFS against a -2 dBFS clip level
    SoftClipper clipper;
    clipper.prepare(kRate);
    clipper.setParams(true, -2.0f);
    std::vector<float> l = x, r = x;
    clipper.process(l.data(), r.data(), static_cast<int>(l.size()));
    const auto latency = static_cast<std::size_t>(SoftClipper::latency());
    double worst = 0.0;
    for (std::size_t i = 4800; i < x.size(); ++i)
        worst = std::max(worst, static_cast<double>(std::abs(l[i] - x[i - latency])));
    CHECK(worst < 1e-3); // the oversampling filters' passband ripple only
    CHECK(clipper.takeClippedFraction() == 0.0);
}

TEST_CASE("The clipper holds the level, and oversampling keeps its harmonics from folding back", "[dsp][clipper]")
{
    // A 9 kHz tone 6 dB over the clip level. Its third harmonic (27 kHz) is
    // above the Nyquist frequency; without oversampling it would fold back to
    // 21 kHz at about -20 dB.
    const auto x = sine(9000.0, 2.0 * std::pow(10.0, -2.0 / 20.0), 48000);
    SoftClipper clipper;
    clipper.prepare(kRate);
    clipper.setParams(true, -2.0f);
    std::vector<float> l = x, r = x;
    clipper.process(l.data(), r.data(), static_cast<int>(l.size()));
    float peak = 0.0f;
    for (std::size_t i = 4800; i < l.size(); ++i)
        peak = std::max(peak, std::abs(l[i]));
    // Clipped and filtered back into the audio band, a high tone is mostly its
    // fundamental at about 4/pi of the clip level (+2.1 dB): the limiter after the clipper catches that.
    CHECK(db(peak) < -2.0 + 2.2);
    const double fundamental = toneAmplitude(l, 4800, 48000, 9000.0);
    CHECK(db(toneAmplitude(l, 4800, 48000, 21000.0) / fundamental) < -55.0);
    CHECK(clipper.takeClippedFraction() > 0.3);
}

TEST_CASE("The share the clipper touches is predicted from the signal", "[dsp][clipper]")
{
    std::mt19937 rng(2);
    std::normal_distribution<float> n(0.0f, 0.2f);
    std::vector<float> l(96000), r(96000);
    for (std::size_t i = 0; i < l.size(); ++i) {
        l[i] = n(rng);
        r[i] = n(rng);
    }
    // The knee where 2 % of the oversampled samples lie above it.
    const double kneeDb = SoftClipper::levelExceededBy(l.data(), r.data(), static_cast<std::int64_t>(l.size()), 0.02);
    SoftClipper clipper;
    clipper.prepare(kRate);
    clipper.setParams(true, static_cast<float>(kneeDb + SoftClipper::kKneeDb));
    clipper.process(l.data(), r.data(), static_cast<int>(l.size()));
    CHECK(clipper.takeClippedFraction() == Approx(0.02).margin(0.002));
}

TEST_CASE("The master chain with the clipper still never passes the ceiling", "[dsp][clipper][master]")
{
    std::mt19937 rng(3);
    std::normal_distribution<float> n(0.0f, 1.0f);
    std::vector<float> l(96000), r(96000);
    for (std::size_t i = 0; i < l.size(); ++i) {
        const double t = static_cast<double>(i % 12000) / kRate;
        l[i] = static_cast<float>(3.0 * std::exp(-t / 0.03) * std::sin(2.0 * std::numbers::pi * 60.0 * t) + 0.5 * n(rng));
        r[i] = l[i];
    }
    MasterChain chain;
    chain.prepare(kRate);
    MasterParams p;
    p.limiter = true;
    p.clipper = true;
    p.ceilingDbtp = -1.0f;
    chain.setParams(p);
    chain.reset();
    chain.process(l.data(), r.data(), static_cast<int>(l.size()));
    TruePeakDetector meter;
    meter.prepare(kRate, 2);
    const float* channels[2] = {l.data(), r.data()};
    CHECK(db(meter.process(channels, 2, static_cast<int>(l.size()))) <= -1.0 + 0.02);
    CHECK(chain.takeClippedFraction() > 0.0);
}

TEST_CASE("The multiband bands add back up flat", "[dsp][multiband]")
{
    // Ratio 1: nothing compresses, so the output is the input through the
    // crossovers: an all-pass, flat in magnitude.
    MultibandCompressor mb;
    mb.prepare(kRate);
    MultibandParams p;
    p.enabled = true;
    p.ratio = {1.0f, 1.0f, 1.0f};
    mb.setParams(p);
    mb.reset();
    for (const double hz : {40.0, 120.0, 500.0, 2500.0, 8000.0}) {
        auto l = sine(hz, 0.3, 48000), r = l;
        mb.reset();
        mb.process(l.data(), r.data(), static_cast<int>(l.size()));
        INFO(hz << " Hz");
        CHECK(db(toneAmplitude(l, 24000, 48000, hz) / 0.3) == Approx(0.0).margin(0.05));
    }
}

TEST_CASE("Each multiband band compresses by the static curve and leaves the others alone", "[dsp][multiband]")
{
    CHECK(MultibandCompressor::gainDb(-30.0, -20.0, 2.0) == 0.0);
    CHECK(MultibandCompressor::gainDb(-10.0, -20.0, 2.0) == Approx(-5.0));
    CHECK(MultibandCompressor::gainDb(-20.0, -20.0, 4.0) == Approx(-0.75 * 3.0 / 4.0)); // half-way into the 6 dB knee

    // A loud 1 kHz tone (mid band, RMS -13 dBFS) and a quiet 60 Hz tone (low band).
    auto loud = sine(1000.0, 0.316, 96000), quiet = sine(60.0, 0.05, 96000);
    std::vector<float> l(96000);
    for (std::size_t i = 0; i < l.size(); ++i)
        l[i] = loud[i] + quiet[i];
    std::vector<float> r = l;
    MultibandCompressor mb;
    mb.prepare(kRate);
    MultibandParams p;
    p.enabled = true;
    p.thresholdDb = {-20.0f, -23.0f, -20.0f};
    p.ratio = {2.0f, 2.0f, 2.0f};
    mb.setParams(p);
    mb.reset();
    mb.process(l.data(), r.data(), static_cast<int>(l.size()));
    // The mid band is 10 dB over its threshold: down 5 dB. The low band (-29 dBFS) stays.
    CHECK(db(toneAmplitude(l, 48000, 96000, 1000.0) / 0.316) == Approx(-5.0).margin(0.4));
    // At 60 Hz the 120 Hz crossover still sends -25 dB of the tone into the mid band, so
    // compressing that band by 5 dB takes about 0.2 dB off it.
    CHECK(db(toneAmplitude(l, 48000, 96000, 60.0) / 0.05) == Approx(0.0).margin(0.3));
    const auto reduction = mb.takeMinGainDb();
    CHECK(reduction[1] < -4.0f);
    CHECK(reduction[0] == Approx(0.0f).margin(0.01));
}

TEST_CASE("The clipper and the multiband never touch the heap", "[dsp][clipper][multiband][rt]")
{
    MasterChain chain;
    chain.prepare(kRate);
    auto l = sine(100.0, 2.0, 24000), r = l;
    MasterParams p;
    p.limiter = p.clipper = true;
    p.multiband.enabled = true;
    {
        test::AllocationGuard guard;
        chain.setParams(p);
        chain.process(l.data(), r.data(), 12000);
        p.clipper = false;
        p.multiband.ratio = {3.0f, 3.0f, 3.0f};
        chain.setParams(p);
        chain.reset();
        chain.process(l.data() + 12000, r.data() + 12000, 12000);
        CHECK(guard.allocations() == 0);
        CHECK(guard.deallocations() == 0);
    }
}
