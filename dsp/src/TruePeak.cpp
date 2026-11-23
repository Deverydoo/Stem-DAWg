// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/dsp/TruePeak.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace daw {

namespace {

/// Zeroth-order modified Bessel function of the first kind (Kaiser window).
double besselI0(double x)
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 50; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1e-12 * sum)
            break;
    }
    return sum;
}

} // namespace

void TruePeakDetector::prepare(double sampleRate, int numChannels)
{
    factor_ = sampleRate < 96000.0 ? 4 : (sampleRate < 192000.0 ? 2 : 1);
    channels_ = std::max(0, numChannels);
    taps_ = factor_ == 1 ? 1 : 16;

    // Windowed-sinc low-pass at the original Nyquist frequency, split into
    // `factor_` branches. Each branch is normalised to unity DC gain so a
    // constant signal reads exactly its own value.
    const int length = factor_ * taps_;
    const double centre = (length - 1) / 2.0;
    const double beta = 8.0;
    std::vector<double> prototype(static_cast<std::size_t>(length), 1.0);
    if (factor_ > 1)
        for (int n = 0; n < length; ++n) {
            const double t = (n - centre) / factor_;
            const double sinc = t == 0.0 ? 1.0 : std::sin(std::numbers::pi * t) / (std::numbers::pi * t);
            const double r = (n - centre) / (centre + 1.0);
            prototype[static_cast<std::size_t>(n)] = sinc * besselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / besselI0(beta);
        }

    phases_.assign(static_cast<std::size_t>(length), 0.0f);
    for (int p = 0; p < factor_; ++p) {
        double sum = 0.0;
        for (int k = 0; k < taps_; ++k)
            sum += prototype[static_cast<std::size_t>(p + k * factor_)];
        for (int k = 0; k < taps_; ++k)
            phases_[static_cast<std::size_t>(p * taps_ + k)] =
                static_cast<float>(prototype[static_cast<std::size_t>(p + k * factor_)] / sum);
    }

    history_.assign(static_cast<std::size_t>(channels_ * 2 * taps_), 0.0f);
    position_.assign(static_cast<std::size_t>(channels_), 0);
}

void TruePeakDetector::reset() noexcept
{
    std::fill(history_.begin(), history_.end(), 0.0f);
    std::fill(position_.begin(), position_.end(), 0);
}

float TruePeakDetector::process(const float* const* channels, int numChannels, int numFrames) noexcept
{
    float peak = 0.0f;
    const int count = std::min(numChannels, channels_);
    for (int c = 0; c < count; ++c) {
        const float* in = channels[c];
        if (in == nullptr)
            continue;
        float* ring = history_.data() + static_cast<std::size_t>(c) * 2 * static_cast<std::size_t>(taps_);
        int& pos = position_[static_cast<std::size_t>(c)];
        for (int i = 0; i < numFrames; ++i) {
            const float x = in[i];
            peak = std::max(peak, std::abs(x));
            if (factor_ == 1)
                continue;
            // Newest sample at ring[pos], older ones follow: ring[pos .. pos + taps_).
            pos = pos == 0 ? taps_ - 1 : pos - 1;
            ring[pos] = x;
            ring[pos + taps_] = x;
            const float* recent = ring + pos;
            for (int p = 0; p < factor_; ++p) {
                const float* h = phases_.data() + static_cast<std::size_t>(p * taps_);
                float y = 0.0f;
                for (int k = 0; k < taps_; ++k)
                    y += h[k] * recent[k];
                peak = std::max(peak, std::abs(y));
            }
        }
    }
    return peak;
}

} // namespace daw
