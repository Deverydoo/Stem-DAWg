// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/dsp/ResonanceSuppressor.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace daw {

namespace {

// 1 inside [low, high], raised-cosine fades over the given octaves outside.
float rangeWeight(double hz, double low, double high, double lowTaper, double highTaper) noexcept
{
    if (hz <= 0.0)
        return 0.0f;
    if (hz < low) {
        const double octaves = std::log2(low / hz);
        return octaves >= lowTaper ? 0.0f : static_cast<float>(0.5 + 0.5 * std::cos(std::numbers::pi * octaves / lowTaper));
    }
    if (hz > high) {
        const double octaves = std::log2(hz / high);
        return octaves >= highTaper ? 0.0f : static_cast<float>(0.5 + 0.5 * std::cos(std::numbers::pi * octaves / highTaper));
    }
    return 1.0f;
}

} // namespace

void ResonanceSuppressor::prepare(double sampleRate, int fftSize, int hop)
{
    const double rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    const int size = std::max(16, fftSize);
    const int bins = size / 2 + 1;
    binHz_ = rate / size;
    gap_ = std::max(1, static_cast<int>(std::lround(kGapHz / binHz_)));
    const double frameMs = 1000.0 * std::max(1, hop) / rate;
    attackCoeff_ = static_cast<float>(std::exp(-frameMs / kAttackMs));
    releaseCoeff_ = static_cast<float>(std::exp(-frameMs / kReleaseMs));

    low_.assign(static_cast<std::size_t>(bins), 0);
    high_.assign(static_cast<std::size_t>(bins), 0);
    harshWeight_.assign(static_cast<std::size_t>(bins), 0.0f);
    shimmerWeight_.assign(static_cast<std::size_t>(bins), 0.0f);
    maxCutDb_.assign(static_cast<std::size_t>(bins), 0.0f);
    excessDb_.assign(static_cast<std::size_t>(bins), 0.0f);
    cut_.assign(static_cast<std::size_t>(bins), 0.0f);
    cumulative_.assign(static_cast<std::size_t>(bins) + 1, 0.0);

    const double down = std::exp2(-kNeighbourOctaves);
    const double up = std::exp2(kNeighbourOctaves);
    first_ = bins;
    last_ = 0;
    for (int k = 1; k < bins; ++k) {
        const auto i = static_cast<std::size_t>(k);
        low_[i] = std::max(0, static_cast<int>(std::floor(k * down)));
        high_[i] = std::min(bins - 1, static_cast<int>(std::ceil(k * up)));
        // A bin is only judged with a full neighbourhood on both sides.
        if (k - gap_ - low_[i] < gap_ || high_[i] - k - gap_ < gap_ || k * up > bins - 1)
            continue;
        const double hz = k * binHz_;
        harshWeight_[i] = rangeWeight(hz, kHarshLowHz, kHarshHighHz, kLowTaperOctaves, kHarshTaperOctaves);
        shimmerWeight_[i] = rangeWeight(hz, kShimmerLowHz, kShimmerHighHz, kLowTaperOctaves, kShimmerTaperOctaves);
        if (harshWeight_[i] > 0.0f || shimmerWeight_[i] > 0.0f) {
            first_ = std::min(first_, k);
            last_ = k + 1;
        }
    }
    if (last_ <= first_)
        first_ = last_ = 0;
    setParams(params_);
    reset();
}

void ResonanceSuppressor::setParams(const ResonanceParams& params) noexcept
{
    const auto limit = [](float db) { return std::isfinite(db) ? std::clamp(db, 0.0f, static_cast<float>(kMaxDb)) : 0.0f; };
    params_ = {limit(params.harshDb), limit(params.shimmerDb)};
    for (std::size_t i = 0; i < maxCutDb_.size(); ++i)
        maxCutDb_[i] = std::max(params_.harshDb * harshWeight_[i], params_.shimmerDb * shimmerWeight_[i]);
}

void ResonanceSuppressor::reset() noexcept
{
    std::fill(excessDb_.begin(), excessDb_.end(), 0.0f);
    frames_ = 0;
}

void ResonanceSuppressor::analyse(const float* power) noexcept
{
    if (last_ <= first_)
        return;
    // Prefix sums up to the right edge of the last judged bin's neighbourhood.
    const int end = high_[static_cast<std::size_t>(last_ - 1)] + 1;
    double sum = 0.0;
    cumulative_[0] = 0.0;
    for (int k = 0; k < end; ++k) {
        sum += power[k];
        cumulative_[static_cast<std::size_t>(k) + 1] = sum;
    }
    const auto mean = [&](int from, int to) { // bins [from, to)
        return (cumulative_[static_cast<std::size_t>(to)] - cumulative_[static_cast<std::size_t>(from)]) / (to - from);
    };

    // A fresh detector averages over few frames, growing to the full time constants.
    const float warm = static_cast<float>(frames_ + kWarmupFrames) / static_cast<float>(frames_ + kWarmupFrames + 1);
    const float attack = std::min(attackCoeff_, warm);
    const float release = std::min(releaseCoeff_, warm);
    if (frames_ < 1 << 20)
        ++frames_;

    constexpr double kTiny = 1e-24;
    for (int k = first_; k < last_; ++k) {
        const auto i = static_cast<std::size_t>(k);
        const double left = mean(low_[i], k - gap_);
        const double right = mean(k + gap_ + 1, high_[i] + 1);
        const float excess = static_cast<float>(10.0 * std::log10((power[k] + kTiny) / (std::max(left, right) + kTiny)));
        float& smoothed = excessDb_[i];
        smoothed = excess + (smoothed - excess) * (excess > smoothed ? attack : release);
    }
}

float ResonanceSuppressor::cutDb(int bin) const noexcept
{
    const auto i = static_cast<std::size_t>(bin);
    return std::clamp(excessDb_[i] - static_cast<float>(kThresholdDb), 0.0f, maxCutDb_[i]);
}

void ResonanceSuppressor::process(const float* power, float* gain) noexcept
{
    analyse(power);
    if (!active())
        return;
    for (int k = first_; k < last_; ++k)
        cut_[static_cast<std::size_t>(k)] = cutDb(k);
    for (int k = first_; k < last_; ++k) {
        float cut = 0.0f;
        for (int j = std::max(first_, k - kSpreadBins); j <= std::min(last_ - 1, k + kSpreadBins); ++j)
            cut = std::max(cut, cut_[static_cast<std::size_t>(j)]);
        cut = std::min(cut, maxCutDb_[static_cast<std::size_t>(k)]); // the range's taper still applies
        if (cut > 0.0f)
            gain[k] *= std::pow(10.0f, -cut / 20.0f);
    }
}

} // namespace daw
