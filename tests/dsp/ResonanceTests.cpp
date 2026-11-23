// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

// Harshness and shimmer control (ResonanceSuppressor inside the clean-up
// chain's spectral stage): turns down narrow peaks that persist above the
// spectrum around them, only in its ranges and only by the set depth; leaves
// noise, moving notes and spectral slopes alone; stays real-time safe.

#include <daw/dsp/CleanupChain.h>
#include <daw/dsp/ResonanceSuppressor.h>

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
constexpr int kBlock = 512;
constexpr double kNoiseRms = 0.05;

struct Stereo {
    std::vector<float> l, r;
    explicit Stereo(std::size_t frames = 0) : l(frames, 0.0f), r(frames, 0.0f) {}
    std::size_t size() const { return l.size(); }
};

Stereo whiteNoise(std::size_t frames, unsigned seed, double rms = kNoiseRms)
{
    Stereo s(frames);
    std::mt19937 rng(seed);
    std::normal_distribution<float> n(0.0f, static_cast<float>(rms));
    for (std::size_t i = 0; i < frames; ++i) {
        s.l[i] = n(rng);
        s.r[i] = n(rng);
    }
    return s;
}

/// Amplitude of a sine that stands `db` above white noise of kNoiseRms in one
/// bin of the chain's 2048-point square-root-Hann frames.
double toneAboveNoise(double db)
{
    const double n = SpectralDenoiser::kFftSize;
    // Tone: (A/2 * 2N/pi)^2; noise: rms^2 * N/2.
    return kNoiseRms * std::sqrt(std::pow(10.0, db / 10.0) * std::numbers::pi * std::numbers::pi / (2.0 * n));
}

void addSine(Stereo& s, double hz, double amplitude, std::size_t from = 0, std::size_t to = 0)
{
    to = to == 0 ? s.size() : to;
    for (std::size_t i = from; i < to; ++i) {
        const auto v = static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / kRate));
        s.l[i] += v;
        s.r[i] += v;
    }
}

CleanupParams resonance(float harshDb, float shimmerDb)
{
    CleanupParams p;
    p.resonance = true;
    p.resonanceHarshDb = harshDb;
    p.resonanceShimmerDb = shimmerDb;
    return p;
}

/// Runs the chain over x (plus its latency of silence) and returns the output
/// aligned with x.
Stereo run(const Stereo& x, const CleanupParams& params)
{
    CleanupChain chain;
    chain.prepare(kRate);
    chain.setParams(params);
    const auto latency = static_cast<std::size_t>(chain.latency());
    Stereo y(x.size() + latency);
    std::copy(x.l.begin(), x.l.end(), y.l.begin());
    std::copy(x.r.begin(), x.r.end(), y.r.begin());
    for (std::size_t at = 0; at < y.size(); at += kBlock) {
        const int n = static_cast<int>(std::min<std::size_t>(kBlock, y.size() - at));
        chain.process(y.l.data() + at, y.r.data() + at, n);
    }
    Stereo aligned(x.size());
    std::copy(y.l.begin() + static_cast<std::ptrdiff_t>(latency), y.l.end(), aligned.l.begin());
    std::copy(y.r.begin() + static_cast<std::ptrdiff_t>(latency), y.r.end(), aligned.r.begin());
    return aligned;
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

/// Mean power per bin of x[from, to) in the chain's frames (both channels).
std::vector<double> spectrum(const Stereo& x, std::size_t from, std::size_t to)
{
    constexpr int n = SpectralDenoiser::kFftSize;
    const Fft fft(n);
    const auto window = SpectralDenoiser::window();
    std::vector<std::complex<float>> scratch(n);
    std::vector<float> power(SpectralDenoiser::kBins);
    std::vector<double> sum(SpectralDenoiser::kBins, 0.0);
    int frames = 0;
    for (std::size_t at = from; at + n <= to; at += n / 2, ++frames) {
        SpectralDenoiser::framePower(x.l.data() + at, x.r.data() + at, power.data(), scratch.data(), fft, window.data());
        for (int k = 0; k < SpectralDenoiser::kBins; ++k)
            sum[static_cast<std::size_t>(k)] += power[static_cast<std::size_t>(k)];
    }
    for (double& v : sum)
        v /= std::max(1, frames);
    return sum;
}

/// Level change in dB between two spectra over [lowHz, highHz), leaving out
/// `skipHz` +- 100 Hz when given.
double bandChangeDb(const std::vector<double>& before, const std::vector<double>& after, double lowHz, double highHz,
                    double skipHz = 0.0)
{
    const double binHz = kRate / SpectralDenoiser::kFftSize;
    double a = 0.0, b = 0.0;
    for (int k = static_cast<int>(lowHz / binHz); k < static_cast<int>(highHz / binHz); ++k) {
        if (skipHz > 0.0 && std::abs(k * binHz - skipHz) < 100.0)
            continue;
        a += before[static_cast<std::size_t>(k)];
        b += after[static_cast<std::size_t>(k)];
    }
    return 10.0 * std::log10(b / a);
}

double gainDb(double before, double after)
{
    return 20.0 * std::log10(after / before);
}

} // namespace

TEST_CASE("Resonance suppression leaves steady noise alone", "[dsp][resonance]")
{
    const Stereo x = whiteNoise(static_cast<std::size_t>(kRate * 6), 1);
    const Stereo y = run(x, resonance(12.0f, 12.0f));
    const std::size_t from = static_cast<std::size_t>(kRate * 2);
    const auto before = spectrum(x, from, x.size());
    const auto after = spectrum(y, from, y.size());
    CHECK(bandChangeDb(before, after, 2000.0, 6000.0) == Approx(0.0).margin(0.1));
    CHECK(bandChangeDb(before, after, 8000.0, 17500.0) == Approx(0.0).margin(0.1));
}

TEST_CASE("A steady tone standing above its surroundings is turned down by the depth", "[dsp][resonance]")
{
    const std::size_t frames = static_cast<std::size_t>(kRate * 6);
    const std::size_t from = static_cast<std::size_t>(kRate * 3); // settled
    for (const double hz : {3700.0, 16000.0}) {
        Stereo x = whiteNoise(frames, 2);
        addSine(x, hz, toneAboveNoise(14.0));
        const double in = toneAmplitude(x.l, from, frames, hz);
        const bool harsh = hz < 7000.0;

        // In its range: down by the 6 dB depth (the peak stands 14 dB above
        // the noise, more than threshold + depth). The noise around it stays.
        const Stereo y = run(x, harsh ? resonance(6.0f, 0.0f) : resonance(0.0f, 6.0f));
        CHECK(gainDb(in, toneAmplitude(y.l, from, frames, hz)) == Approx(-6.0).margin(1.0));
        const double low = harsh ? 2000.0 : 8000.0, high = harsh ? 6000.0 : 17500.0;
        CHECK(bandChangeDb(spectrum(x, from, frames), spectrum(y, from, frames), low, high, hz) == Approx(0.0).margin(0.15));

        // The other range's control does nothing to it.
        const Stereo other = run(x, harsh ? resonance(0.0f, 12.0f) : resonance(12.0f, 0.0f));
        CHECK(gainDb(in, toneAmplitude(other.l, from, frames, hz)) == Approx(0.0).margin(0.2));
    }
}

TEST_CASE("A peak is brought down towards its surroundings, never below", "[dsp][resonance]")
{
    const std::size_t frames = static_cast<std::size_t>(kRate * 6);
    const std::size_t from = static_cast<std::size_t>(kRate * 3);
    Stereo x = whiteNoise(frames, 3);
    addSine(x, 4000.0, toneAboveNoise(6.0)); // 6 dB proud: 4 dB above the threshold
    const double in = toneAmplitude(x.l, from, frames, 4000.0);
    const double out = toneAmplitude(run(x, resonance(12.0f, 0.0f)).l, from, frames, 4000.0);
    CHECK(gainDb(in, out) < -2.0);
    CHECK(gainDb(in, out) > -5.0); // not the full 12 dB depth

    // A peak under the threshold is left alone.
    Stereo faint = whiteNoise(frames, 4);
    addSine(faint, 4000.0, toneAboveNoise(0.5));
    const double faintIn = toneAmplitude(faint.l, from, frames, 4000.0);
    CHECK(gainDb(faintIn, toneAmplitude(run(faint, resonance(12.0f, 0.0f)).l, from, frames, 4000.0))
          == Approx(0.0).margin(0.5));
}

TEST_CASE("Peaks outside both ranges are never touched; between them the tapers apply", "[dsp][resonance]")
{
    const std::size_t frames = static_cast<std::size_t>(kRate * 6);
    const std::size_t from = static_cast<std::size_t>(kRate * 3);
    Stereo x = whiteNoise(frames, 5);
    for (const double hz : {800.0, 1300.0, 7000.0, 21500.0})
        addSine(x, hz, toneAboveNoise(20.0));
    const Stereo y = run(x, resonance(12.0f, 12.0f));
    const auto change = [&](double hz) {
        return gainDb(toneAmplitude(x.l, from, frames, hz), toneAmplitude(y.l, from, frames, hz));
    };
    CHECK(change(800.0) == Approx(0.0).margin(0.2));
    CHECK(change(1300.0) == Approx(0.0).margin(0.2)); // below the harshness range's half-octave taper
    CHECK(change(21500.0) == Approx(0.0).margin(0.2)); // above the shimmer range's quarter-octave taper
    // 7 kHz lies in both tapers; the shimmer range's weighs more there:
    // 0.5 + 0.5 cos(pi log2(8000 / 7000) / 0.5) of the depth.
    const double weight = 0.5 + 0.5 * std::cos(std::numbers::pi * std::log2(8000.0 / 7000.0) / 0.5);
    CHECK(change(7000.0) == Approx(-12.0 * weight).margin(1.0));
}

TEST_CASE("A melody of short notes is not mistaken for resonances", "[dsp][resonance]")
{
    // Sawtooth-like notes (12 partials) changing every 250 ms over two octaves:
    // each partial comes and goes, so none persists.
    const std::size_t frames = static_cast<std::size_t>(kRate * 20);
    Stereo x = whiteNoise(frames, 6, 0.002);
    std::mt19937 rng(11);
    std::uniform_int_distribution<int> semitone(0, 23);
    const std::size_t note = static_cast<std::size_t>(kRate * 0.25);
    for (std::size_t start = 0; start < frames; start += note) {
        const double f0 = 220.0 * std::pow(2.0, semitone(rng) / 12.0);
        for (int h = 1; h <= 30; ++h)
            if (f0 * h < 17000.0)
                addSine(x, f0 * h, 0.1 / h, start, std::min(frames, start + note));
    }
    const Stereo y = run(x, resonance(6.0f, 6.0f));
    const auto before = spectrum(x, static_cast<std::size_t>(kRate * 2), frames);
    const auto after = spectrum(y, static_cast<std::size_t>(kRate * 2), frames);
    CHECK(bandChangeDb(before, after, 2000.0, 6000.0) > -0.5);
    CHECK(bandChangeDb(before, after, 8000.0, 17000.0) > -0.5);
}

TEST_CASE("A spectral cliff is not a peak", "[dsp][resonance]")
{
    // Noise that stops abruptly at 12 kHz, like a generator's low-pass: the
    // bins just below the cliff stand above the (empty) right side only.
    const std::size_t frames = static_cast<std::size_t>(kRate * 6);
    Stereo x = whiteNoise(frames, 7);
    {
        // 12th-order Butterworth low-pass at 12 kHz.
        std::vector<Biquad> sections(6);
        for (int k = 0; k < 6; ++k) {
            const double theta = std::numbers::pi * (2.0 * k + 1.0) / 24.0;
            sections[static_cast<std::size_t>(k)].setCoefficients(
                BiquadCoefficients::lowPass(12000.0, 1.0 / (2.0 * std::cos(theta)), kRate));
        }
        for (auto* channel : {&x.l, &x.r}) {
            for (auto& s : sections)
                s.reset();
            for (float& v : *channel)
                for (auto& s : sections)
                    v = static_cast<float>(s.process(v));
        }
    }
    const Stereo y = run(x, resonance(12.0f, 12.0f));
    const std::size_t from = static_cast<std::size_t>(kRate * 2);
    CHECK(bandChangeDb(spectrum(x, from, frames), spectrum(y, from, frames), 9000.0, 12500.0) == Approx(0.0).margin(0.2));
}

TEST_CASE("The resonance detector keeps its memory across reset; prepare clears it", "[dsp][resonance]")
{
    ResonanceSuppressor s;
    s.prepare(kRate, SpectralDenoiser::kFftSize, SpectralDenoiser::kHop);
    s.setParams({6.0f, 6.0f});
    std::vector<float> power(static_cast<std::size_t>(s.bins()), 1.0f);
    const int bin = static_cast<int>(std::lround(4000.0 / s.binHz()));
    power[static_cast<std::size_t>(bin)] = 100.0f; // 20 dB proud
    for (int i = 0; i < 400; ++i)
        s.analyse(power.data());
    CHECK(s.excessDb(bin) > 15.0f);
    CHECK(s.cutDb(bin) == Approx(6.0f));
    CHECK(s.cutDb(bin - 20) == 0.0f);
    s.reset();
    CHECK(s.cutDb(bin) == 0.0f);
    s.prepare(kRate, SpectralDenoiser::kFftSize, SpectralDenoiser::kHop);
    CHECK(s.cutDb(bin) == 0.0f);
}

TEST_CASE("Resonance depths are limited and the chain restarts only when the spectral stage starts or stops",
          "[dsp][resonance]")
{
    ResonanceSuppressor s;
    s.prepare(kRate, SpectralDenoiser::kFftSize, SpectralDenoiser::kHop);
    s.setParams({40.0f, std::nanf("")});
    CHECK(s.params().harshDb == static_cast<float>(ResonanceSuppressor::kMaxDb));
    CHECK(s.params().shimmerDb == 0.0f);
    s.setParams({-3.0f, 0.0f});
    CHECK_FALSE(s.active());

    CleanupParams off;
    CleanupParams on = resonance(6.0f, 6.0f);
    CHECK(CleanupChain::needsRestart(off, on));
    CHECK_FALSE(CleanupChain::needsRestart(on, resonance(3.0f, 0.0f)));
    off.denoise = on.denoise = true; // the spectral stage already runs
    CHECK_FALSE(CleanupChain::needsRestart(off, on));
    CHECK(on.active());
}

TEST_CASE("The chain with resonance suppression never touches the heap", "[dsp][resonance][rt]")
{
    CleanupChain chain;
    chain.prepare(kRate);
    Stereo x = whiteNoise(48000, 8);
    addSine(x, 3000.0, 0.05);
    CleanupParams p = resonance(6.0f, 6.0f);
    p.expander = p.transient = true;
    {
        test::AllocationGuard guard;
        chain.setParams(p);
        chain.process(x.l.data(), x.r.data(), 24000);
        p.resonanceHarshDb = 12.0f;
        p.resonanceShimmerDb = 0.0f;
        chain.setParams(p);
        chain.reset();
        chain.process(x.l.data() + 24000, x.r.data() + 24000, 24000);
        CHECK(guard.allocations() == 0);
        CHECK(guard.deallocations() == 0);
    }
    for (float v : x.l)
        REQUIRE(std::isfinite(v));
}
