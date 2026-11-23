// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

// Spectral noise reduction, hum removal and "listen to removed".

#include <daw/dsp/CleanupChain.h>
#include <daw/dsp/SpectralDenoiser.h>

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
constexpr int N = SpectralDenoiser::kFftSize;

struct Stereo {
    std::vector<float> l, r;
    explicit Stereo(std::size_t n = 0) : l(n, 0.0f), r(n, 0.0f) {}
    std::size_t size() const { return l.size(); }
};

Stereo noise(std::size_t frames, double rmsDb, unsigned seed)
{
    Stereo s(frames);
    std::mt19937 rng(seed);
    std::normal_distribution<float> g(0.0f, static_cast<float>(std::pow(10.0, rmsDb / 20.0)));
    for (std::size_t i = 0; i < frames; ++i) {
        s.l[i] = g(rng);
        s.r[i] = g(rng);
    }
    return s;
}

void addSine(Stereo& s, double hz, double amplitude, std::size_t from = 0)
{
    for (std::size_t i = from; i < s.size(); ++i) {
        const auto v = static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / kRate));
        s.l[i] += v;
        s.r[i] += v;
    }
}

/// Mean frame power per bin, as a profile is learned.
std::vector<float> learn(const Stereo& s)
{
    Fft fft(N);
    const auto window = SpectralDenoiser::window();
    std::vector<std::complex<float>> scratch(N);
    std::vector<float> power(SpectralDenoiser::kBins), sum(SpectralDenoiser::kBins, 0.0f);
    int frames = 0;
    for (std::size_t at = 0; at + N <= s.size(); at += N / 4, ++frames) {
        SpectralDenoiser::framePower(s.l.data() + at, s.r.data() + at, power.data(), scratch.data(), fft, window.data());
        for (int k = 0; k < SpectralDenoiser::kBins; ++k)
            sum[static_cast<std::size_t>(k)] += power[static_cast<std::size_t>(k)];
    }
    for (float& v : sum)
        v /= static_cast<float>(frames);
    return sum;
}

double rmsDb(const std::vector<float>& x, std::size_t from, std::size_t to)
{
    double e = 0.0;
    for (std::size_t i = from; i < to; ++i)
        e += static_cast<double>(x[i]) * x[i];
    return 10.0 * std::log10(e / static_cast<double>(to - from) + 1e-30);
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

} // namespace

TEST_CASE("The denoiser without a profile reconstructs its input, delayed by one frame", "[dsp][denoise]")
{
    SpectralDenoiser d;
    d.prepare();
    Stereo x = noise(20000, -10.0, 1);
    addSine(x, 440.0, 0.3);
    Stereo y = x;
    for (std::size_t at = 0; at < y.size(); at += 300)
        d.process(y.l.data() + at, y.r.data() + at, static_cast<int>(std::min<std::size_t>(300, y.size() - at)));
    double worst = 0.0;
    for (std::size_t i = N; i < y.size(); ++i) {
        worst = std::max(worst, static_cast<double>(std::abs(y.l[i] - x.l[i - N])));
        worst = std::max(worst, static_cast<double>(std::abs(y.r[i] - x.r[i - N])));
    }
    CHECK(worst < 1e-5);
    for (int i = 0; i < N - SpectralDenoiser::kHop; ++i) // nothing comes out before the first frame
        CHECK(std::abs(y.l[static_cast<std::size_t>(i)]) < 1e-6f); // FFT rounding only
}

TEST_CASE("A learned profile takes stationary noise down by the reduction amount", "[dsp][denoise]")
{
    const Stereo profileSource = noise(48000, -40.0, 2);
    const auto profile = learn(profileSource);

    SpectralDenoiser d;
    d.prepare();
    d.setParams({12.0f, 0.0f});
    d.setNoiseProfile(profile.data());
    Stereo y = noise(96000, -40.0, 3); // different noise, same statistics
    const double before = rmsDb(y.l, 24000, 96000);
    d.process(y.l.data(), y.r.data(), static_cast<int>(y.size()));
    const double after = rmsDb(y.l, 24000 + N, 96000);
    CHECK(before - after == Approx(12.0).margin(1.0));
}

TEST_CASE("Music above the noise passes the denoiser almost untouched", "[dsp][denoise]")
{
    const auto profile = learn(noise(48000, -50.0, 4));
    SpectralDenoiser d;
    d.prepare();
    d.setParams({20.0f, 0.0f});
    d.setNoiseProfile(profile.data());

    Stereo x = noise(96000, -50.0, 5);
    addSine(x, 1000.0, 0.3); // about -13 dBFS RMS, 37 dB above the noise
    Stereo y = x;
    d.process(y.l.data(), y.r.data(), static_cast<int>(y.size()));

    const double toneIn = toneAmplitude(x.l, 24000, 72000, 1000.0);
    const double toneOut = toneAmplitude(y.l, 24000 + N, 72000 + N, 1000.0);
    CHECK(20.0 * std::log10(toneOut / toneIn) == Approx(0.0).margin(0.3));

    // Away from the tone the noise drops by nearly the full 20 dB (5-15 kHz band
    // of the averaged spectra).
    auto band = [](const Stereo& s, std::size_t from, std::size_t to) {
        Stereo part(to - from);
        std::copy(s.l.begin() + static_cast<std::ptrdiff_t>(from), s.l.begin() + static_cast<std::ptrdiff_t>(to), part.l.begin());
        std::copy(s.r.begin() + static_cast<std::ptrdiff_t>(from), s.r.begin() + static_cast<std::ptrdiff_t>(to), part.r.begin());
        const auto power = learn(part);
        double sum = 0.0;
        for (int k = static_cast<int>(5000.0 * N / kRate); k < static_cast<int>(15000.0 * N / kRate); ++k)
            sum += power[static_cast<std::size_t>(k)];
        return 10.0 * std::log10(sum);
    };
    CHECK(band(x, 24000, 72000) - band(y, 24000 + N, 72000 + N) > 17.0);
}

TEST_CASE("Hum notches remove mains hum and its harmonics, nothing else", "[dsp][hum]")
{
    for (const double mains : {50.0, 60.0}) {
        INFO("mains " << mains);
        HumFilter hum;
        hum.prepare(kRate);
        hum.setParams(mains, 4);
        Stereo x(96000);
        addSine(x, mains, 0.1);
        addSine(x, 2 * mains, 0.05);
        addSine(x, 3 * mains, 0.05);
        addSine(x, 1000.0, 0.2);
        Stereo y = x;
        hum.process(y.l.data(), y.r.data(), static_cast<int>(y.size()));
        const std::size_t from = 48000, to = 96000; // after the notches settle
        for (int h = 1; h <= 3; ++h)
            CHECK(20.0 * std::log10(toneAmplitude(y.l, from, to, h * mains) / toneAmplitude(x.l, from, to, h * mains)) < -40.0);
        CHECK(20.0 * std::log10(toneAmplitude(y.l, from, to, 1000.0) / toneAmplitude(x.l, from, to, 1000.0))
              == Approx(0.0).margin(0.05));
    }
    HumFilter none;
    none.prepare(kRate);
    none.setParams(50.0, 0);
    Stereo x(1000);
    addSine(x, 50.0, 0.5);
    Stereo y = x;
    none.process(y.l.data(), y.r.data(), 1000);
    CHECK(y.l == x.l);
}

TEST_CASE("Listen to removed plays exactly what the chain takes away", "[dsp][cleanup][delta]")
{
    Stereo x = noise(48000, -20.0, 6);
    addSine(x, 60.0, 0.2);

    CleanupParams p;
    p.hum = true;
    p.humHz = 60.0f;
    p.humHarmonics = 1;
    CleanupChain wet, removed;
    wet.prepare(kRate);
    removed.prepare(kRate);
    wet.setParams(p);
    p.listenRemoved = true;
    removed.setParams(p);
    Stereo a = x, b = x;
    wet.process(a.l.data(), a.r.data(), static_cast<int>(a.size()));
    removed.process(b.l.data(), b.r.data(), static_cast<int>(b.size()));

    const auto latency = static_cast<std::size_t>(wet.latency());
    double worst = 0.0;
    for (std::size_t i = latency; i < x.size(); ++i)
        worst = std::max(worst, static_cast<double>(std::abs(b.l[i] - (x.l[i - latency] - a.l[i]))));
    CHECK(worst < 1e-6);
    // What was removed is essentially the hum.
    const std::size_t from = 24000, to = 48000;
    CHECK(toneAmplitude(b.l, from, to, 60.0) == Approx(0.2).margin(0.01));
    CHECK(rmsDb(b.l, from, to) == Approx(20.0 * std::log10(0.2 / std::sqrt(2.0))).margin(0.3)); // nothing but the hum
}

TEST_CASE("The full chain with noise reduction and hum removal never touches the heap", "[dsp][denoise][rt]")
{
    const auto profile = learn(noise(16384, -40.0, 7));
    CleanupChain chain;
    chain.prepare(kRate);
    Stereo x = noise(24000, -30.0, 8);
    CleanupParams p;
    p.dcRemoval = p.highPass = p.hum = p.denoise = p.expander = true;
    p.noiseProfile = profile.data();
    {
        test::AllocationGuard guard;
        chain.setParams(p);
        chain.process(x.l.data(), x.r.data(), 12000);
        p.listenRemoved = true;
        p.humHarmonics = 8;
        chain.setParams(p);
        chain.reset();
        chain.process(x.l.data() + 12000, x.r.data() + 12000, 12000);
        CHECK(guard.allocations() == 0);
        CHECK(guard.deallocations() == 0);
    }
    for (float v : x.l)
        REQUIRE(std::isfinite(v));
}
