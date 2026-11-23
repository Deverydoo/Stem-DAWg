// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/dsp/DeepFilterDsp.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace daw::deepfilter {

namespace {

constexpr int kMinNbErbFreqs = 2;
constexpr float kNormTau = 1.0f; // seconds
// Initial normalisation states (libDF's MEAN_NORM_INIT and UNIT_NORM_INIT),
// spread linearly across the bands.
constexpr float kMeanInitLow = -60.0f, kMeanInitHigh = -90.0f;
constexpr float kUnitInitLow = 0.001f, kUnitInitHigh = 0.0001f;
// The forward transform's scale (libDF's wnorm): 1 / (N^2 / (2 * hop)).
constexpr float kForwardScale = 2.0f * kHop / (static_cast<float>(kFftSize) * kFftSize);

float freqToErb(float hz)
{
    return 9.265f * std::log1p(hz / (24.7f * 9.265f));
}

float erbToFreq(float erb)
{
    return 24.7f * 9.265f * (std::exp(erb / 9.265f) - 1.0f);
}

std::array<int, kErbBands> makeErbBands()
{
    std::array<int, kErbBands> bands{};
    const float binHz = static_cast<float>(kSampleRate) / kFftSize;
    const float low = freqToErb(0.0f);
    const float high = freqToErb(kSampleRate / 2.0f);
    const float step = (high - low) / kErbBands;
    int previous = 0, over = 0;
    for (int i = 1; i <= kErbBands; ++i) {
        const int bin = static_cast<int>(std::lround(erbToFreq(low + static_cast<float>(i) * step) / binHz));
        int width = bin - previous - over;
        if (width < kMinNbErbFreqs) {
            over = kMinNbErbFreqs - width;
            width = kMinNbErbFreqs;
        } else {
            over = 0;
        }
        bands[static_cast<std::size_t>(i - 1)] = width;
        previous = bin;
    }
    bands[kErbBands - 1] += 1; // the Nyquist bin
    int total = 0;
    for (int b : bands)
        total += b;
    bands[kErbBands - 1] -= total - kBins;
    return bands;
}

std::array<float, kFftSize> makeWindow()
{
    std::array<float, kFftSize> w{};
    for (int i = 0; i < kFftSize; ++i) {
        const double s = std::sin(std::numbers::pi * (i + 0.5) / kFftSize);
        w[static_cast<std::size_t>(i)] = static_cast<float>(std::sin(0.5 * std::numbers::pi * s * s));
    }
    return w;
}

} // namespace

const std::array<int, kErbBands>& erbBandWidths()
{
    static const std::array<int, kErbBands> bands = makeErbBands();
    return bands;
}

const std::array<float, kFftSize>& window()
{
    static const std::array<float, kFftSize> w = makeWindow();
    return w;
}

// ---------------------------------------------------------------- Analysis

Analysis::Analysis() : fft_(kFftSize)
{
    (void)window();
    (void)erbBandWidths();
}

void Analysis::reset() noexcept
{
    memory_.fill(0.0f);
}

void Analysis::process(const float* hop, Spectrum& out) noexcept
{
    const auto& w = window();
    for (int i = 0; i < kHop; ++i) {
        buffer_[static_cast<std::size_t>(i)] = {memory_[static_cast<std::size_t>(i)] * w[static_cast<std::size_t>(i)], 0.0f};
        buffer_[static_cast<std::size_t>(i + kHop)] = {hop[i] * w[static_cast<std::size_t>(i + kHop)], 0.0f};
    }
    std::copy(hop, hop + kHop, memory_.begin());
    fft_.forward(buffer_.data());
    for (int k = 0; k < kBins; ++k)
        out[static_cast<std::size_t>(k)] = buffer_[static_cast<std::size_t>(k)] * kForwardScale;
}

// ---------------------------------------------------------------- Synthesis

Synthesis::Synthesis() : fft_(kFftSize)
{
    (void)window();
}

void Synthesis::reset() noexcept
{
    memory_.fill(0.0f);
}

void Synthesis::process(const Spectrum& in, float* hop) noexcept
{
    // Hermitian extension, then an unscaled inverse transform (libDF's irfft):
    // the forward scale and the squared window then sum to unity.
    for (int k = 0; k < kBins; ++k)
        buffer_[static_cast<std::size_t>(k)] = in[static_cast<std::size_t>(k)];
    buffer_[0].imag(0.0f);
    buffer_[kFftSize / 2].imag(0.0f);
    for (int k = 1; k < kFftSize / 2; ++k)
        buffer_[static_cast<std::size_t>(kFftSize - k)] = std::conj(in[static_cast<std::size_t>(k)]);
    fft_.inverse(buffer_.data());
    const auto& w = window();
    const auto n = static_cast<float>(kFftSize);
    for (int i = 0; i < kHop; ++i) {
        hop[i] = buffer_[static_cast<std::size_t>(i)].real() * n * w[static_cast<std::size_t>(i)] + memory_[static_cast<std::size_t>(i)];
        memory_[static_cast<std::size_t>(i)] =
            buffer_[static_cast<std::size_t>(i + kHop)].real() * n * w[static_cast<std::size_t>(i + kHop)];
    }
}

// ---------------------------------------------------------------- Features

float Features::alpha() noexcept
{
    // libDF's calc_norm_alpha: exp(-hop / (rate * tau)), rounded to the first
    // precision (3, 4, ... decimals) that stays below one: 0.99 here.
    const double a = std::exp(-static_cast<double>(kHop) / (kSampleRate * kNormTau));
    for (int precision = 3;; ++precision) {
        const double scale = std::pow(10.0, precision);
        const double rounded = std::round(a * scale) / scale;
        if (rounded < 1.0)
            return static_cast<float>(rounded);
    }
}

void Features::reset() noexcept
{
    for (int i = 0; i < kErbBands; ++i)
        meanState_[static_cast<std::size_t>(i)] = kMeanInitLow + (kMeanInitHigh - kMeanInitLow) * static_cast<float>(i) / (kErbBands - 1);
    for (int i = 0; i < kDfBins; ++i)
        unitState_[static_cast<std::size_t>(i)] = kUnitInitLow + (kUnitInitHigh - kUnitInitLow) * static_cast<float>(i) / (kDfBins - 1);
}

void Features::compute(const Spectrum& spectrum, float* erb, float* spec) noexcept
{
    const float a = alpha();
    const auto& bands = erbBandWidths();
    int bin = 0;
    for (int b = 0; b < kErbBands; ++b) {
        const int width = bands[static_cast<std::size_t>(b)];
        float power = 0.0f;
        for (int j = 0; j < width; ++j)
            power += std::norm(spectrum[static_cast<std::size_t>(bin + j)]);
        bin += width;
        const float db = 10.0f * std::log10(power / static_cast<float>(width) + 1e-10f);
        float& state = meanState_[static_cast<std::size_t>(b)];
        state = db * (1.0f - a) + state * a;
        erb[b] = (db - state) / 40.0f;
    }
    for (int k = 0; k < kDfBins; ++k) {
        const std::complex<float> x = spectrum[static_cast<std::size_t>(k)];
        float& state = unitState_[static_cast<std::size_t>(k)];
        state = std::abs(x) * (1.0f - a) + state * a;
        const float norm = 1.0f / std::sqrt(state);
        spec[k] = x.real() * norm;
        spec[kDfBins + k] = x.imag() * norm;
    }
}

// ---------------------------------------------------------------- Output stages

void applyErbGains(Spectrum& spectrum, const float* gains) noexcept
{
    const auto& bands = erbBandWidths();
    int bin = 0;
    for (int b = 0; b < kErbBands; ++b) {
        const int width = bands[static_cast<std::size_t>(b)];
        for (int j = 0; j < width; ++j)
            spectrum[static_cast<std::size_t>(bin + j)] *= gains[b];
        bin += width;
    }
}

void applyDeepFilter(const Spectrum* const* frames, const float* coefs, Spectrum& out) noexcept
{
    for (int f = 0; f < kDfBins; ++f) {
        std::complex<float> sum{};
        for (int i = 0; i < kDfOrder; ++i) {
            const float* c = coefs + (f * kDfOrder + i) * 2;
            sum += (*frames[i])[static_cast<std::size_t>(f)] * std::complex<float>(c[0], c[1]);
        }
        out[static_cast<std::size_t>(f)] = sum;
    }
}

} // namespace daw::deepfilter
