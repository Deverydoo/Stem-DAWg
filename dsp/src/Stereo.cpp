// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/dsp/Stereo.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace daw {

// ---- StereoImager ---------------------------------------------------------------

void StereoImager::prepare(double sampleRate) noexcept
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    ramp_ = static_cast<float>(1.0 - std::exp(-1000.0 / (kRampMs * sampleRate_)));
    setParams(params_);
    reset();
}

void StereoImager::setParams(const StereoImagerParams& params) noexcept
{
    const auto finite = [](float v, float fallback) { return std::isfinite(v) ? v : fallback; };
    params_.monoBass = params.monoBass;
    params_.monoBelowHz = std::clamp(finite(params.monoBelowHz, 120.0f), static_cast<float>(kMinHz),
                                     static_cast<float>(std::min(kMaxHz, 0.45 * sampleRate_)));
    params_.width = std::clamp(finite(params.width, 1.0f), 0.0f, static_cast<float>(kMaxWidth));
    const BiquadCoefficients c = BiquadCoefficients::highPass(params_.monoBelowHz, std::numbers::sqrt2 / 2.0, sampleRate_);
    for (Biquad& section : highPass_)
        section.setCoefficients(c);
}

void StereoImager::reset() noexcept
{
    for (Biquad& section : highPass_)
        section.reset();
    mono_ = params_.monoBass ? 1.0f : 0.0f;
    width_ = params_.width;
}

bool StereoImager::active() const noexcept
{
    return params_.monoBass || params_.width != 1.0f || mono_ > 1e-4f || std::abs(width_ - 1.0f) > 1e-4f;
}

void StereoImager::process(float* left, float* right, int numFrames) noexcept
{
    const float monoTarget = params_.monoBass ? 1.0f : 0.0f;
    const float widthTarget = params_.width;
    for (int i = 0; i < numFrames; ++i) {
        mono_ += (monoTarget - mono_) * ramp_;
        width_ += (widthTarget - width_) * ramp_;
        const double mid = 0.5 * (static_cast<double>(left[i]) + right[i]);
        double side = 0.5 * (static_cast<double>(left[i]) - right[i]);
        // The high-pass always runs, so mono bass glides in without a transient.
        const double highSide = highPass_[1].process(highPass_[0].process(side));
        side = (side + mono_ * (highSide - side)) * width_;
        left[i] = static_cast<float>(mid + side);
        right[i] = static_cast<float>(mid - side);
    }
    if (std::abs(mono_ - monoTarget) < 1e-5f)
        mono_ = monoTarget;
    if (std::abs(width_ - widthTarget) < 1e-5f)
        width_ = widthTarget;
}

// ---- PhaseAligner ---------------------------------------------------------------

void PhaseAligner::prepare(double sampleRate, int fftSize, int hop)
{
    const double rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    const int size = std::max(16, fftSize);
    const int bins = size / 2 + 1;
    coeff_ = static_cast<float>(std::exp(-1000.0 * std::max(1, hop) / (kSmoothingMs * rate)));
    first_ = std::max(1, static_cast<int>(std::ceil(kLowestHz * size / rate)));
    settleFrames_ = std::max(1, static_cast<int>(std::lround(kSmoothingMs * 0.001 * rate / std::max(1, hop))));
    cross_.assign(static_cast<std::size_t>(bins), {});
    powerLeft_.assign(static_cast<std::size_t>(bins), 0.0f);
    powerRight_.assign(static_cast<std::size_t>(bins), 0.0f);
    phase_.assign(static_cast<std::size_t>(bins), 0.0f);
    rotation_.assign(static_cast<std::size_t>(bins), 0.0f);
    reset();
}

void PhaseAligner::reset() noexcept
{
    std::fill(cross_.begin(), cross_.end(), std::complex<float>{});
    std::fill(powerLeft_.begin(), powerLeft_.end(), 0.0f);
    std::fill(powerRight_.begin(), powerRight_.end(), 0.0f);
    std::fill(phase_.begin(), phase_.end(), 0.0f);
    std::fill(rotation_.begin(), rotation_.end(), 0.0f);
    frames_ = 0;
}

void PhaseAligner::analyse(const std::complex<float>* left, const std::complex<float>* right) noexcept
{
    constexpr float pi = std::numbers::pi_v<float>;
    const float limit = static_cast<float>(kMaxPhaseDeg * std::numbers::pi / 180.0);
    const float a = coeff_;
    const int last = bins() - 1; // the Nyquist bin is real: nothing to rotate
    frames_ = std::min(frames_ + 1, settleFrames_);
    const float settled = static_cast<float>(frames_) / static_cast<float>(settleFrames_);
    for (int k = first_; k < last; ++k) {
        const auto i = static_cast<std::size_t>(k);
        const std::complex<float> l = left[k], r = right[k];
        cross_[i] = a * cross_[i] + (1.0f - a) * l * std::conj(r);
        powerLeft_[i] = a * powerLeft_[i] + (1.0f - a) * std::norm(l);
        powerRight_[i] = a * powerRight_[i] + (1.0f - a) * std::norm(r);
        const float coherence = std::norm(cross_[i]) / (powerLeft_[i] * powerRight_[i] + 1e-30f);

        // Near +-pi the measured phase flips sign from frame to frame and from
        // bin to bin. Turning neighbouring bins opposite ways would smear the
        // frame, so near pi the positive way is the rule, and once a bin has
        // a direction it keeps it.
        float phase = std::arg(cross_[i]);
        const float previous = phase_[i];
        if (phase < -0.9f * pi && previous >= -0.5f * pi)
            phase += 2.0f * pi;
        else if (previous > 0.5f * pi && phase < -0.5f * pi)
            phase += 2.0f * pi;
        else if (previous < -0.5f * pi && phase > 0.5f * pi)
            phase -= 2.0f * pi;
        if (phase > 1.5f * pi)
            phase -= 2.0f * pi;
        else if (phase < -1.5f * pi)
            phase += 2.0f * pi;
        phase_[i] = phase;

        const float excess = std::max(std::abs(phase) - limit, 0.0f) * (phase < 0.0f ? -1.0f : 1.0f);
        const float weight = std::clamp((coherence - static_cast<float>(kCoherenceLow))
                                            / static_cast<float>(kCoherenceHigh - kCoherenceLow),
                                        0.0f, 1.0f);
        rotation_[i] = excess * weight * settled;
    }
}

void PhaseAligner::apply(std::complex<float>* left, std::complex<float>* right) const noexcept
{
    const int last = bins() - 1;
    for (int k = first_; k < last; ++k) {
        const float half = 0.5f * rotation_[static_cast<std::size_t>(k)];
        if (half == 0.0f)
            continue;
        const std::complex<float> turn = std::polar(1.0f, half);
        left[k] *= std::conj(turn);
        right[k] *= turn;
    }
}

} // namespace daw
