// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/dsp/MasterChain.h>

#include <algorithm>
#include <cmath>

namespace daw {

// ---- TruePeakLimiter ------------------------------------------------------------

void TruePeakLimiter::prepare(double sampleRate)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    detector_.prepare(sampleRate_, 2);
    window_ = std::max(2, static_cast<int>(std::lround(kLookaheadMs * 0.001 * sampleRate_)));
    interp_ = detector_.delay();
    latency_ = window_ - 1 + interp_;
    for (auto& channel : audio_)
        channel.assign(static_cast<std::size_t>(latency_) + 2, 0.0f);
    // The deque holds the window (W + 1 values) plus the newest before the oldest leaves.
    minValue_.assign(static_cast<std::size_t>(window_) + 2, 1.0f);
    minIndex_.assign(static_cast<std::size_t>(window_) + 2, 0);
    average_.assign(static_cast<std::size_t>(window_), 1.0f);
    setParams(enabled_, 20.0f * std::log10(ceiling_), 100.0f);
    reset();
}

void TruePeakLimiter::setParams(bool enabled, float ceilingDb, float releaseMs) noexcept
{
    enabled_ = enabled;
    const float db = std::isfinite(ceilingDb) ? std::clamp(ceilingDb, static_cast<float>(kMinCeilingDb), static_cast<float>(kMaxCeilingDb))
                                              : -1.0f;
    ceiling_ = std::pow(10.0f, db / 20.0f);
    const double ms = std::isfinite(releaseMs) ? std::clamp(static_cast<double>(releaseMs), kMinReleaseMs, kMaxReleaseMs) : 100.0;
    releaseCoeff_ = static_cast<float>(std::exp(-1000.0 / (ms * sampleRate_)));
}

void TruePeakLimiter::reset() noexcept
{
    detector_.reset();
    for (auto& channel : audio_)
        std::fill(channel.begin(), channel.end(), 0.0f);
    audioPos_ = 0;
    minHead_ = minSize_ = 0;
    index_ = 0;
    envelope_ = 1.0f;
    std::fill(average_.begin(), average_.end(), 1.0f);
    averagePos_ = 0;
    averageSum_ = static_cast<double>(window_);
    minGain_ = 1.0f;
}

float TruePeakLimiter::delayed(int channel, int age) const noexcept
{
    const int size = static_cast<int>(audio_[0].size());
    int at = audioPos_ - age;
    if (at < 0)
        at += size;
    return audio_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(at)];
}

void TruePeakLimiter::process(float* left, float* right, int numFrames) noexcept
{
    const int size = static_cast<int>(audio_[0].size());
    const int span = window_ + 1; // the minimum's window
    const int capacity = static_cast<int>(minValue_.size());
    for (int i = 0; i < numFrames; ++i) {
        const float x[2] = {left[i], right[i]};

        // True peak of the stretch between the samples interp_ and interp_ - 1 ago.
        float peak = std::max(detector_.interpolatedPeak(0, x[0]), detector_.interpolatedPeak(1, x[1]));
        audioPos_ = audioPos_ + 1 == size ? 0 : audioPos_ + 1;
        audio_[0][static_cast<std::size_t>(audioPos_)] = x[0];
        audio_[1][static_cast<std::size_t>(audioPos_)] = x[1];
        for (int c = 0; c < 2; ++c)
            peak = std::max({peak, std::abs(delayed(c, interp_)), std::abs(delayed(c, std::max(0, interp_ - 1)))});
        const float needed = enabled_ && peak > ceiling_ ? ceiling_ / peak : 1.0f;

        // 1. Sliding minimum over the last W + 1 values (monotonic deque).
        while (minSize_ > 0) {
            const int back = (minHead_ + minSize_ - 1) % capacity;
            if (minValue_[static_cast<std::size_t>(back)] < needed)
                break;
            --minSize_;
        }
        const int slot = (minHead_ + minSize_) % capacity;
        minValue_[static_cast<std::size_t>(slot)] = needed;
        minIndex_[static_cast<std::size_t>(slot)] = index_;
        ++minSize_;
        if (minIndex_[static_cast<std::size_t>(minHead_)] <= index_ - span) {
            minHead_ = (minHead_ + 1) % capacity;
            --minSize_;
        }
        const float minimum = minValue_[static_cast<std::size_t>(minHead_)];
        ++index_;

        // 2. Down at once, up with the release.
        envelope_ = minimum < envelope_ ? minimum : minimum + (envelope_ - minimum) * releaseCoeff_;

        // 3. Average over W.
        averageSum_ += envelope_ - average_[static_cast<std::size_t>(averagePos_)];
        average_[static_cast<std::size_t>(averagePos_)] = envelope_;
        averagePos_ = averagePos_ + 1 == window_ ? 0 : averagePos_ + 1;
        if (averagePos_ == 0) { // re-sum now and then so rounding cannot drift
            double sum = 0.0;
            for (float v : average_)
                sum += v;
            averageSum_ = sum;
        }
        const float gain = std::min(1.0f, static_cast<float>(averageSum_ / window_));
        minGain_ = std::min(minGain_, gain);

        const float outLeft = delayed(0, latency_) * gain;
        const float outRight = delayed(1, latency_) * gain;
        left[i] = outLeft;
        right[i] = outRight;
    }
}

float TruePeakLimiter::takeMinGainDb() noexcept
{
    const float lowest = minGain_;
    minGain_ = 1.0f;
    return lowest >= 1.0f ? 0.0f : 20.0f * std::log10(std::max(lowest, 1e-6f));
}

// ---- MasterChain ------------------------------------------------------------------

BiquadCoefficients masterEqCoefficients(std::size_t band, double gainDb, double sampleRate) noexcept
{
    const MasterEqBand& b = kMasterEqBands[std::min(band, kMasterEqBandCount - 1)];
    const double hz = std::min(b.hz, 0.45 * sampleRate);
    switch (b.shape) {
    case EqShape::LowShelf: return BiquadCoefficients::lowShelf(hz, b.q, gainDb, sampleRate);
    case EqShape::HighShelf: return BiquadCoefficients::highShelf(hz, b.q, gainDb, sampleRate);
    case EqShape::Peak: break;
    }
    return BiquadCoefficients::peaking(hz, b.q, gainDb, sampleRate);
}

void MasterChain::prepare(double sampleRate)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    limiter_.prepare(sampleRate_);
    setParams(params_);
    reset();
}

void MasterChain::setParams(const MasterParams& params) noexcept
{
    params_ = params;
    limiter_.setParams(params.limiter, params.ceilingDbtp, params.releaseMs);
    // New coefficients keep the filters' memory, so moving a gain does not click.
    for (std::size_t b = 0; b < kMasterEqBandCount; ++b) {
        const float g = std::isfinite(params.eqGainDb[b])
                            ? std::clamp(params.eqGainDb[b], static_cast<float>(-kMasterEqMaxDb), static_cast<float>(kMasterEqMaxDb))
                            : 0.0f;
        const bool on = params.eq && g != 0.0f;
        if (on && !eqOn_[b])
            for (auto& channel : eq_)
                channel[b].reset();
        eqOn_[b] = on;
        const BiquadCoefficients c = masterEqCoefficients(b, g, sampleRate_);
        for (auto& channel : eq_)
            channel[b].setCoefficients(c);
    }
}

void MasterChain::reset() noexcept
{
    for (auto& channel : eq_)
        for (Biquad& section : channel)
            section.reset();
    limiter_.reset();
}

void MasterChain::process(float* left, float* right, int numFrames) noexcept
{
    float* channels[2] = {left, right};
    const int count = left == right ? 1 : 2;
    for (std::size_t b = 0; b < kMasterEqBandCount; ++b) {
        if (!eqOn_[b])
            continue;
        for (int c = 0; c < count; ++c) {
            Biquad& section = eq_[static_cast<std::size_t>(c)][b];
            float* x = channels[c];
            for (int i = 0; i < numFrames; ++i)
                x[i] = static_cast<float>(section.process(x[i]));
        }
    }
    limiter_.process(left, right, numFrames);
}

} // namespace daw
