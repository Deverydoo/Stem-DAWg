// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/dsp/Multiband.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace daw {

namespace {

constexpr double kButterworthQ = std::numbers::sqrt2 / 2.0;

double timeCoeff(double ms, double rate) noexcept
{
    return std::exp(-1000.0 / (ms * rate));
}

} // namespace

void MultibandCompressor::Splitter::prepare(double sampleRate) noexcept
{
    const double a = std::min(kCrossoverHz[0], 0.45 * sampleRate);
    const double b = std::min(kCrossoverHz[1], 0.45 * sampleRate);
    for (Biquad& s : lowA_)
        s.setCoefficients(BiquadCoefficients::lowPass(a, kButterworthQ, sampleRate));
    for (Biquad& s : highA_)
        s.setCoefficients(BiquadCoefficients::highPass(a, kButterworthQ, sampleRate));
    for (Biquad& s : lowB_)
        s.setCoefficients(BiquadCoefficients::lowPass(b, kButterworthQ, sampleRate));
    for (Biquad& s : highB_)
        s.setCoefficients(BiquadCoefficients::highPass(b, kButterworthQ, sampleRate));
    allPassB_.setCoefficients(BiquadCoefficients::allPass(b, kButterworthQ, sampleRate));
    reset();
}

void MultibandCompressor::Splitter::reset() noexcept
{
    for (auto* chain : {&lowA_, &highA_, &lowB_, &highB_})
        for (Biquad& s : *chain)
            s.reset();
    allPassB_.reset();
}

std::array<double, kMultibandBands> MultibandCompressor::Splitter::split(double x) noexcept
{
    // Linkwitz-Riley 4th order = two Butterworth 2nd-order sections.
    const double low = allPassB_.process(lowA_[1].process(lowA_[0].process(x)));
    const double rest = highA_[1].process(highA_[0].process(x));
    const double mid = lowB_[1].process(lowB_[0].process(rest));
    const double high = highB_[1].process(highB_[0].process(rest));
    return {low, mid, high};
}

void MultibandCompressor::prepare(double sampleRate) noexcept
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    for (Splitter& s : splitters_)
        s.prepare(sampleRate_);
    detectorCoeff_ = timeCoeff(kDetectorMs, sampleRate_);
    for (std::size_t b = 0; b < kMultibandBands; ++b) {
        attackCoeff_[b] = timeCoeff(kAttackMs[b], sampleRate_);
        releaseCoeff_[b] = timeCoeff(kReleaseMs[b], sampleRate_);
    }
    glide_ = static_cast<float>(1.0 - timeCoeff(kGlideMs, sampleRate_));
    setParams(params_);
    reset();
}

void MultibandCompressor::setParams(const MultibandParams& params) noexcept
{
    const bool starting = params.enabled && !params_.enabled && mix_ == 0.0f;
    params_ = params;
    for (std::size_t b = 0; b < kMultibandBands; ++b) {
        float& t = params_.thresholdDb[b];
        t = std::isfinite(t) ? std::clamp(t, -60.0f, 0.0f) : -20.0f;
        float& r = params_.ratio[b];
        r = std::isfinite(r) ? std::clamp(r, 1.0f, static_cast<float>(kMaxRatio)) : 1.0f;
    }
    if (starting) // the bands start from silence, the detectors from nothing
        reset();
}

void MultibandCompressor::reset() noexcept
{
    for (Splitter& s : splitters_)
        s.reset();
    power_.fill(0.0);
    gainDb_.fill(0.0);
    minGainDb_.fill(0.0f);
    mix_ = params_.enabled ? 1.0f : 0.0f;
}

double MultibandCompressor::gainDb(double levelDb, double thresholdDb, double ratio) noexcept
{
    const double slope = 1.0 / std::max(1.0, ratio) - 1.0; // <= 0
    const double over = levelDb - thresholdDb;
    if (over <= -kKneeDb / 2.0)
        return 0.0;
    if (over < kKneeDb / 2.0) {
        const double x = over + kKneeDb / 2.0;
        return slope * x * x / (2.0 * kKneeDb);
    }
    return slope * over;
}

void MultibandCompressor::process(float* left, float* right, int numFrames) noexcept
{
    if (!active())
        return;
    const bool same = left == right;
    const float target = params_.enabled ? 1.0f : 0.0f;
    for (int i = 0; i < numFrames; ++i) {
        const double l = left[i];
        const double r = same ? l : static_cast<double>(right[i]);
        const auto bl = splitters_[0].split(l);
        const auto br = same ? bl : splitters_[1].split(r);
        double outL = 0.0, outR = 0.0;
        for (std::size_t b = 0; b < kMultibandBands; ++b) {
            const double p = 0.5 * (bl[b] * bl[b] + br[b] * br[b]);
            power_[b] = p + detectorCoeff_ * (power_[b] - p);
            const double level = 10.0 * std::log10(power_[b] + 1e-20);
            const double wanted = gainDb(level, params_.thresholdDb[b], params_.ratio[b]);
            const double coeff = wanted < gainDb_[b] ? attackCoeff_[b] : releaseCoeff_[b];
            gainDb_[b] = wanted + (gainDb_[b] - wanted) * coeff;
            minGainDb_[b] = std::min(minGainDb_[b], static_cast<float>(gainDb_[b]));
            const double g = gainDb_[b] > -1e-6 ? 1.0 : std::pow(10.0, gainDb_[b] / 20.0);
            outL += bl[b] * g;
            outR += br[b] * g;
        }
        mix_ += (target - mix_) * glide_;
        if (std::abs(mix_ - target) < 1e-5f)
            mix_ = target;
        left[i] = static_cast<float>(l + (outL - l) * mix_);
        if (!same)
            right[i] = static_cast<float>(r + (outR - r) * mix_);
    }
}

std::array<float, kMultibandBands> MultibandCompressor::takeMinGainDb() noexcept
{
    const auto lowest = minGainDb_;
    for (std::size_t b = 0; b < kMultibandBands; ++b)
        minGainDb_[b] = static_cast<float>(gainDb_[b]);
    return lowest;
}

} // namespace daw
