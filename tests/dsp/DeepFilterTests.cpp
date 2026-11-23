// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

// Any-size FFT and the DeepFilterNet3 signal processing (everything but the network).

#include "AllocationGuard.h"

#include <daw/dsp/DeepFilterDsp.h>
#include <daw/dsp/Fft.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <complex>
#include <numbers>
#include <random>
#include <vector>

using namespace daw;
namespace df = daw::deepfilter;
using Catch::Approx;

TEST_CASE("ChirpFft matches a direct DFT at any size and inverts exactly", "[dsp][fft]")
{
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> uniform(-1.0f, 1.0f);
    for (const int n : {1, 2, 3, 5, 12, 64, 100, 960}) {
        INFO("size " << n);
        std::vector<std::complex<float>> x(static_cast<std::size_t>(n));
        for (auto& v : x)
            v = {uniform(rng), uniform(rng)};
        std::vector<std::complex<double>> expected(static_cast<std::size_t>(n));
        for (int k = 0; k < n; ++k)
            for (int i = 0; i < n; ++i)
                expected[static_cast<std::size_t>(k)] += std::complex<double>(x[static_cast<std::size_t>(i)])
                                                         * std::polar(1.0, -2.0 * std::numbers::pi * k * i / n);
        ChirpFft fft(n);
        auto y = x;
        fft.forward(y.data());
        double worst = 0.0;
        for (int k = 0; k < n; ++k)
            worst = std::max(worst, std::abs(std::complex<double>(y[static_cast<std::size_t>(k)]) - expected[static_cast<std::size_t>(k)]));
        CHECK(worst < 1e-3 * std::sqrt(static_cast<double>(n)));
        fft.inverse(y.data());
        for (int i = 0; i < n; ++i)
            CHECK(std::abs(y[static_cast<std::size_t>(i)] - x[static_cast<std::size_t>(i)]) < 1e-4f);
    }

    // Prepared transforms do not allocate.
    ChirpFft fft(960);
    std::vector<std::complex<float>> data(960, {0.5f, 0.0f});
    test::AllocationGuard guard;
    fft.forward(data.data());
    fft.inverse(data.data());
    CHECK(guard.allocations() == 0);
}

TEST_CASE("DeepFilterNet ERB bands cover the spectrum like libDF's", "[dsp][deepfilter]")
{
    const auto& bands = df::erbBandWidths();
    int total = 0;
    for (int width : bands) {
        CHECK(width >= 2);
        total += width;
    }
    CHECK(total == df::kBins);
    // libDF's erb_fb(48000, 960, 32, 2), computed independently.
    const std::array<int, df::kErbBands> expected{2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 5, 5, 7,
                                                  7, 8, 10, 12, 13, 15, 18, 20, 24, 28, 31, 37, 42, 50, 56, 67};
    CHECK(bands == expected);

    // The Vorbis window satisfies Princen-Bradley: w[i]^2 + w[i + hop]^2 == 1.
    const auto& w = df::window();
    for (int i = 0; i < df::kHop; ++i)
        CHECK(w[static_cast<std::size_t>(i)] * w[static_cast<std::size_t>(i)]
                  + w[static_cast<std::size_t>(i + df::kHop)] * w[static_cast<std::size_t>(i + df::kHop)]
              == Approx(1.0f).margin(1e-5));
    CHECK(df::Features::alpha() == Approx(0.99f));
}

TEST_CASE("DeepFilterNet analysis and synthesis reconstruct the input one hop late", "[dsp][deepfilter]")
{
    std::mt19937 rng(9);
    std::uniform_real_distribution<float> uniform(-0.5f, 0.5f);
    const int hops = 20;
    std::vector<float> input(static_cast<std::size_t>(hops * df::kHop));
    for (auto& v : input)
        v = uniform(rng);
    df::Analysis analysis;
    df::Synthesis synthesis;
    df::Spectrum spectrum;
    std::vector<float> output(input.size());
    test::AllocationGuard guard;
    for (int t = 0; t < hops; ++t) {
        analysis.process(input.data() + t * df::kHop, spectrum);
        synthesis.process(spectrum, output.data() + t * df::kHop);
    }
    CHECK(guard.allocations() == 0);
    float worst = 0.0f;
    for (std::size_t i = df::kHop; i < output.size(); ++i)
        worst = std::max(worst, std::abs(output[i] - input[i - df::kHop]));
    CHECK(worst < 1e-5f);

    // The spectrum is scaled as libDF scales it: a full-scale DC frame gives
    // sum(window) * 2 * hop / N^2 in bin 0.
    df::Analysis dc;
    std::vector<float> ones(df::kHop, 1.0f);
    dc.process(ones.data(), spectrum);
    dc.process(ones.data(), spectrum);
    double windowSum = 0.0;
    for (float v : df::window())
        windowSum += v;
    CHECK(spectrum[0].real() == Approx(windowSum * 2.0 * df::kHop / (static_cast<double>(df::kFftSize) * df::kFftSize)).epsilon(1e-4));
}

TEST_CASE("DeepFilterNet features and output stages", "[dsp][deepfilter]")
{
    // A steady tone: the mean-normalised ERB features settle towards zero and
    // the unit-normalised spectrum keeps the tone's bin the largest.
    df::Analysis analysis;
    df::Features features;
    df::Spectrum spectrum;
    std::vector<float> hop(df::kHop), erb(df::kErbBands), spec(2 * df::kDfBins);
    const int toneBin = 20; // 1 kHz
    for (int t = 0; t < 1000; ++t) {
        for (int i = 0; i < df::kHop; ++i)
            hop[static_cast<std::size_t>(i)] =
                0.3f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 1000.0 * (t * df::kHop + i) / df::kSampleRate));
        analysis.process(hop.data(), spectrum);
        features.compute(spectrum, erb.data(), spec.data());
    }
    for (float v : erb)
        CHECK(std::abs(v) < 0.05f);
    const auto magnitude = [&](int k) { return std::hypot(spec[static_cast<std::size_t>(k)], spec[static_cast<std::size_t>(df::kDfBins + k)]); };
    for (int k = 0; k < df::kDfBins; ++k)
        if (std::abs(k - toneBin) > 2)
            CHECK(magnitude(k) < magnitude(toneBin));

    // Gains scale whole bands.
    df::Spectrum s;
    s.fill({1.0f, 1.0f});
    std::vector<float> gains(df::kErbBands, 1.0f);
    gains[0] = 0.5f;
    gains.back() = 0.0f;
    df::applyErbGains(s, gains.data());
    CHECK(s[0] == std::complex<float>(0.5f, 0.5f));
    CHECK(s[1] == std::complex<float>(0.5f, 0.5f));
    CHECK(s[2] == std::complex<float>(1.0f, 1.0f));
    CHECK(s[df::kBins - 1] == std::complex<float>(0.0f, 0.0f));

    // The deep filter with a single unit tap on the middle frame is identity
    // for the low bins and leaves the rest alone.
    std::array<df::Spectrum, df::kDfOrder> frames;
    for (int i = 0; i < df::kDfOrder; ++i)
        frames[static_cast<std::size_t>(i)].fill({static_cast<float>(i), 1.0f});
    std::array<const df::Spectrum*, df::kDfOrder> taps{};
    for (int i = 0; i < df::kDfOrder; ++i)
        taps[static_cast<std::size_t>(i)] = &frames[static_cast<std::size_t>(i)];
    std::vector<float> coefs(df::kDfBins * df::kDfOrder * 2, 0.0f);
    for (int f = 0; f < df::kDfBins; ++f)
        coefs[static_cast<std::size_t>((f * df::kDfOrder + 2) * 2)] = 1.0f; // tap 2 (the current frame), real 1
    df::Spectrum out;
    out.fill({9.0f, 9.0f});
    df::applyDeepFilter(taps.data(), coefs.data(), out);
    CHECK(out[0] == std::complex<float>(2.0f, 1.0f));
    CHECK(out[df::kDfBins - 1] == std::complex<float>(2.0f, 1.0f));
    CHECK(out[df::kDfBins] == std::complex<float>(9.0f, 9.0f));
}
