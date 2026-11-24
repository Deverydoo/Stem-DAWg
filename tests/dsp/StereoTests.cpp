// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

// Stereo repair inside the clean-up chain: mono bass makes the low end mono
// and leaves the rest; width scales the side signal; changes glide; phase
// alignment brings coherent channels into step without changing their
// levels, and leaves mono, unrelated channels and the low end alone.

#include <daw/dsp/CleanupChain.h>
#include <daw/dsp/Stereo.h>

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
constexpr int kBlock = 480;

struct Stereo {
    std::vector<float> l, r;
    explicit Stereo(std::size_t frames = 0) : l(frames, 0.0f), r(frames, 0.0f) {}
    std::size_t size() const { return l.size(); }
};

/// Sums of sines with random phases between `lowHz` and `highHz`: the right
/// channel carries the same partials turned by `degrees` (0 = mono, 90 = in
/// quadrature, 180 = polarity inverted). With `independent` the right channel
/// gets its own random phases instead (unrelated channels).
Stereo partials(double seconds, double lowHz, double highHz, double degrees, unsigned seed, bool independent = false)
{
    const auto frames = static_cast<std::size_t>(seconds * kRate);
    Stereo s(frames);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> phase(0.0, 2.0 * std::numbers::pi);
    constexpr int count = 96;
    const double turn = degrees * std::numbers::pi / 180.0;
    for (int p = 0; p < count; ++p) {
        const double hz = lowHz * std::pow(highHz / lowHz, (p + 0.5) / count);
        const double a = 0.3 / std::sqrt(static_cast<double>(count));
        std::complex<double> left = std::polar(a, phase(rng));
        std::complex<double> right = independent ? std::polar(a, phase(rng)) : left * std::polar(1.0, -turn);
        const std::complex<double> step = std::polar(1.0, 2.0 * std::numbers::pi * hz / kRate);
        for (std::size_t i = 0; i < frames; ++i) {
            s.l[i] += static_cast<float>(left.real());
            s.r[i] += static_cast<float>(right.real());
            left *= step;
            right *= step;
        }
    }
    return s;
}

CleanupParams stereo(bool monoBass, float width = 1.0f, bool align = false, float hz = 120.0f)
{
    CleanupParams p;
    p.stereo = true;
    p.stereoMonoBass = monoBass;
    p.stereoMonoBelowHz = hz;
    p.stereoWidth = width;
    p.stereoPhaseAlign = align;
    return p;
}

/// Runs the chain over x (plus its latency of silence) and returns the output aligned with x.
Stereo run(const Stereo& x, const CleanupParams& params, int block = kBlock)
{
    CleanupChain chain;
    chain.prepare(kRate);
    chain.setParams(params);
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

struct BandPowers {
    double left = 0.0, right = 0.0, mid = 0.0, side = 0.0;
    double monoLossDb() const { return 10.0 * std::log10(mid / (0.5 * (left + right))); }
    double sideToMidDb() const { return 10.0 * std::log10(side / mid); }
};

/// Powers in [lowHz, highHz) of x[from, to), from 4096-point Hann frames.
BandPowers band(const Stereo& x, std::size_t from, std::size_t to, double lowHz, double highHz)
{
    constexpr int n = 4096;
    const Fft fft(n);
    std::vector<std::complex<float>> z(n);
    BandPowers p;
    const int k0 = static_cast<int>(std::ceil(lowHz * n / kRate));
    const int k1 = static_cast<int>(highHz * n / kRate);
    for (std::size_t at = from; at + n <= to; at += n / 2) {
        for (int i = 0; i < n; ++i) {
            const float w = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / n));
            z[static_cast<std::size_t>(i)] = {x.l[at + static_cast<std::size_t>(i)] * w, x.r[at + static_cast<std::size_t>(i)] * w};
        }
        fft.forward(z);
        for (int k = k0; k < k1; ++k) {
            const std::complex<float> a = z[static_cast<std::size_t>(k)];
            const std::complex<float> b = std::conj(z[static_cast<std::size_t>(n - k)]);
            const std::complex<float> l = 0.5f * (a + b);
            const std::complex<float> r = std::complex<float>(0.0f, -0.5f) * (a - b);
            p.left += std::norm(l);
            p.right += std::norm(r);
            p.mid += 0.25 * std::norm(l + r);
            p.side += 0.25 * std::norm(l - r);
        }
    }
    return p;
}

double db(double ratio)
{
    return 10.0 * std::log10(ratio);
}

} // namespace

TEST_CASE("The stereo stage at neutral settings changes nothing", "[dsp][stereo]")
{
    const Stereo x = partials(1.0, 40.0, 16000.0, 0.0, 1, true);
    const Stereo dry = run(x, CleanupParams{.dcRemoval = true});
    CleanupParams neutral = stereo(false);
    neutral.dcRemoval = true;
    const Stereo y = run(x, neutral);
    REQUIRE(y.l == dry.l);
    REQUIRE(y.r == dry.r);
}

TEST_CASE("Mono bass makes the low end mono and leaves the mid and the rest of the image", "[dsp][stereo]")
{
    const Stereo x = partials(4.0, 30.0, 8000.0, 0.0, 2, true); // unrelated channels: side as strong as mid
    const Stereo y = run(x, stereo(true));
    const std::size_t from = 4800, to = x.size();
    CHECK(band(x, from, to, 30.0, 60.0).sideToMidDb() == Approx(0.0).margin(1.5));
    CHECK(band(y, from, to, 30.0, 60.0).sideToMidDb() < -20.0);
    CHECK(band(y, from, to, 500.0, 8000.0).sideToMidDb() == Approx(band(x, from, to, 500.0, 8000.0).sideToMidDb()).margin(0.1));
    // The mid signal passes untouched.
    double worst = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i)
        worst = std::max(worst, std::abs(0.5 * (y.l[i] + y.r[i]) - 0.5 * (x.l[i] + x.r[i])));
    CHECK(worst < 1e-6);
}

TEST_CASE("Width scales the side signal only", "[dsp][stereo]")
{
    const Stereo x = partials(2.0, 200.0, 8000.0, 0.0, 3, true);
    const std::size_t from = 4800, to = x.size();
    const BandPowers in = band(x, from, to, 200.0, 8000.0);
    const Stereo mono = run(x, stereo(false, 0.0f));
    double worst = 0.0;
    for (std::size_t i = 24000; i < mono.size(); ++i) // after the glide from the neutral start
        worst = std::max(worst, static_cast<double>(std::abs(mono.l[i] - mono.r[i])));
    CHECK(worst < 1e-6);
    const BandPowers wide = band(run(x, stereo(false, 2.0f)), from, to, 200.0, 8000.0);
    CHECK(db(wide.side / in.side) == Approx(6.02).margin(0.05));
    CHECK(db(wide.mid / in.mid) == Approx(0.0).margin(0.01));
}

TEST_CASE("Stereo changes glide instead of jumping", "[dsp][stereo]")
{
    // A 200 Hz pure side signal; the width drops from 100 % to 0 % mid-way.
    const auto frames = static_cast<std::size_t>(kRate);
    Stereo x(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        const auto v = static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 200.0 * static_cast<double>(i) / kRate));
        x.l[i] = v;
        x.r[i] = -v;
    }
    CleanupChain chain;
    chain.prepare(kRate);
    chain.setParams(stereo(false));
    Stereo y = x;
    chain.process(y.l.data(), y.r.data(), 24000);
    chain.setParams(stereo(false, 0.0f));
    chain.process(y.l.data() + 24000, y.r.data() + 24000, 24000);
    // The sine itself moves at most 2 pi 200 / 48000 * 0.5 = 0.013 per sample.
    float step = 0.0f;
    for (std::size_t i = 1; i < frames; ++i)
        step = std::max(step, std::abs(y.l[i] - y.l[i - 1]));
    CHECK(step < 0.02f);
    CHECK(std::abs(y.l.back()) < 1e-3f); // and it does arrive at mono
}

TEST_CASE("Phase alignment brings coherent channels into step and keeps their levels", "[dsp][stereo]")
{
    const std::size_t from = 24000; // after the statistics settle
    for (const double degrees : {90.0, 180.0}) {
        const Stereo x = partials(4.0, 150.0, 15000.0, degrees, 4);
        const Stereo y = run(x, stereo(false, 1.0f, true));
        const BandPowers in = band(x, from, x.size(), 200.0, 15000.0);
        const BandPowers out = band(y, from, y.size(), 200.0, 15000.0);
        // Turned to the 45 degrees left alone: a mono sum loses 10 log10((1 + cos 45) / 2) = -0.69 dB.
        CHECK(out.monoLossDb() > -1.0);
        CHECK(db(out.left / in.left) == Approx(0.0).margin(0.3));
        CHECK(db(out.right / in.right) == Approx(0.0).margin(0.3));
    }
}

TEST_CASE("Phase alignment leaves mono, unrelated channels and the low end alone", "[dsp][stereo]")
{
    const std::size_t from = 24000;
    // Mono: passes exactly as without alignment (the spectral stage alone).
    const Stereo mono = partials(2.0, 150.0, 15000.0, 0.0, 5);
    CleanupParams spectralOnly;
    spectralOnly.resonance = true; // runs the spectral stage with zero depths: a plain frame round trip
    const Stereo plain = run(mono, spectralOnly);
    const Stereo aligned = run(mono, stereo(false, 1.0f, true));
    double worst = 0.0;
    for (std::size_t i = 0; i < mono.size(); ++i)
        worst = std::max(worst, static_cast<double>(std::abs(aligned.l[i] - plain.l[i])) + std::abs(aligned.r[i] - plain.r[i]));
    CHECK(worst < 1e-4);

    // Unrelated channels (independent noise) have no phase to correct: they
    // stay at -3 dB in mono. (Steady sines would not do: a sine in both
    // channels is coherent whatever its phases.)
    Stereo unrelated(static_cast<std::size_t>(4.0 * kRate));
    {
        std::mt19937 rng(6);
        std::normal_distribution<float> noise(0.0f, 0.05f);
        for (std::size_t i = 0; i < unrelated.size(); ++i) {
            unrelated.l[i] = noise(rng);
            unrelated.r[i] = noise(rng);
        }
    }
    const BandPowers u = band(run(unrelated, stereo(false, 1.0f, true)), from, unrelated.size(), 200.0, 15000.0);
    CHECK(u.monoLossDb() == Approx(band(unrelated, from, unrelated.size(), 200.0, 15000.0).monoLossDb()).margin(0.3));

    // Below 120 Hz it is mono bass's job: an inverted 40-100 Hz stays inverted.
    const Stereo low = partials(4.0, 40.0, 100.0, 180.0, 7);
    CHECK(band(run(low, stereo(false, 1.0f, true)), from, low.size(), 40.0, 100.0).monoLossDb() < -20.0);
}

TEST_CASE("Phase alignment needs the spectral stage, which restarts the chain", "[dsp][stereo]")
{
    const CleanupParams off = stereo(true);
    const CleanupParams on = stereo(true, 1.0f, true);
    CHECK_FALSE(off.spectral());
    CHECK(on.spectral());
    CHECK(CleanupChain::needsRestart(off, on));
    CHECK_FALSE(CleanupChain::needsRestart(stereo(false), off)); // mono bass and width need no restart
    CleanupParams switchedOff = on;
    switchedOff.stereo = false;
    CHECK_FALSE(switchedOff.spectral());
}

TEST_CASE("The chain with stereo repair never touches the heap", "[dsp][stereo][rt]")
{
    CleanupChain chain;
    chain.prepare(kRate);
    Stereo x = partials(1.0, 40.0, 15000.0, 90.0, 8);
    CleanupParams p = stereo(true, 0.8f, true);
    {
        test::AllocationGuard guard;
        chain.setParams(p);
        chain.process(x.l.data(), x.r.data(), 24000);
        p.stereoMonoBelowHz = 200.0f;
        p.stereoWidth = 1.5f;
        chain.setParams(p);
        chain.reset();
        chain.process(x.l.data() + 24000, x.r.data() + 24000, 24000);
        CHECK(guard.allocations() == 0);
        CHECK(guard.deallocations() == 0);
    }
    for (float v : x.l)
        REQUIRE(std::isfinite(v));
}
