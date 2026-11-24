// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/dsp/Clipper.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <numeric>

namespace daw {

namespace {

constexpr double kCutoff = 0.46 / SoftClipper::kFactor; // cycles per oversampled sample: 92 % of the input's Nyquist
constexpr double kKaiserBeta = 7.0;
constexpr int kHighRing = SoftClipper::kTaps + SoftClipper::kFactor; // the newest step's samples plus the filter span

double besselI0(double x)
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 60; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1e-14 * sum)
            break;
    }
    return sum;
}

} // namespace

void SoftClipper::prepare(double sampleRate)
{
    const double centre = (kTaps - 1) / 2.0;
    taps_.assign(kTaps, 0.0f);
    std::vector<double> h(kTaps);
    for (int n = 0; n < kTaps; ++n) {
        const double t = n - centre;
        const double sinc = t == 0.0 ? 2.0 * kCutoff : std::sin(2.0 * std::numbers::pi * kCutoff * t) / (std::numbers::pi * t);
        const double r = t / centre;
        h[static_cast<std::size_t>(n)] = sinc * besselI0(kKaiserBeta * std::sqrt(std::max(0.0, 1.0 - r * r))) / besselI0(kKaiserBeta);
    }
    const double sum = std::accumulate(h.begin(), h.end(), 0.0);
    for (int n = 0; n < kTaps; ++n)
        taps_[static_cast<std::size_t>(n)] = static_cast<float>(h[static_cast<std::size_t>(n)] / sum);

    // Upsampler branches: u[4n + p] = 4 * sum_k h[p + 4k] x[n - k].
    branchTaps_ = (kTaps + kFactor - 1) / kFactor;
    for (int p = 0; p < kFactor; ++p) {
        auto& branch = phases_[static_cast<std::size_t>(p)];
        branch.assign(static_cast<std::size_t>(branchTaps_), 0.0f);
        for (int k = 0; p + k * kFactor < kTaps; ++k)
            branch[static_cast<std::size_t>(k)] = static_cast<float>(kFactor) * taps_[static_cast<std::size_t>(p + k * kFactor)];
    }
    for (auto& ring : input_)
        ring.assign(static_cast<std::size_t>(2 * branchTaps_), 0.0f);
    for (auto& ring : high_)
        ring.assign(static_cast<std::size_t>(2 * kHighRing), 0.0f);
    for (auto& line : delay_)
        line.assign(static_cast<std::size_t>(latency()), 0.0f);
    glide_ = static_cast<float>(1.0 - std::exp(-1000.0 / (kGlideMs * (sampleRate > 0.0 ? sampleRate : 48000.0))));
    reset();
}

void SoftClipper::setParams(bool enabled, float clipDbfs) noexcept
{
    if (enabled && !enabled_ && mix_ == 0.0f)
        for (auto& ring : high_) // the oversampled path starts clean
            std::fill(ring.begin(), ring.end(), 0.0f);
    enabled_ = enabled;
    const float db = std::isfinite(clipDbfs) ? std::clamp(clipDbfs, -24.0f, 0.0f) : -2.0f;
    clip_ = std::pow(10.0f, db / 20.0f);
}

void SoftClipper::reset() noexcept
{
    for (auto* rings : {&input_, &high_, &delay_})
        for (auto& ring : *rings)
            std::fill(ring.begin(), ring.end(), 0.0f);
    inputPos_ = highPos_ = delayPos_ = 0;
    mix_ = enabled_ ? 1.0f : 0.0f;
    touched_ = counted_ = 0;
}

float SoftClipper::shape(float x, float clip) noexcept
{
    const float knee = clip * static_cast<float>(std::pow(10.0, -kKneeDb / 20.0));
    const float a = std::abs(x);
    if (a <= knee)
        return x;
    const float y = knee + (clip - knee) * std::tanh((a - knee) / (clip - knee));
    return std::copysign(y, x);
}

void SoftClipper::process(float* left, float* right, int numFrames) noexcept
{
    float* io[2] = {left, right};
    const int channels = left == right ? 1 : 2;
    const float knee = clip_ * static_cast<float>(std::pow(10.0, -kKneeDb / 20.0));
    const float target = enabled_ ? 1.0f : 0.0f;
    const bool oversample = enabled_ || mix_ > 0.0f;
    const int length = latency();
    for (int i = 0; i < numFrames; ++i) {
        float out[2] = {0.0f, 0.0f};
        for (int c = 0; c < channels; ++c) {
            const float x = io[c][i];
            // The bypass path: a plain delay of latency() frames.
            float& slot = delay_[static_cast<std::size_t>(c)][static_cast<std::size_t>(delayPos_)];
            const float dry = slot;
            slot = x;
            if (!oversample) {
                out[c] = dry;
                continue;
            }
            // Newest first, mirrored so every read is contiguous.
            auto& in = input_[static_cast<std::size_t>(c)];
            const int ip = inputPos_ == 0 ? branchTaps_ - 1 : inputPos_ - 1;
            in[static_cast<std::size_t>(ip)] = x;
            in[static_cast<std::size_t>(ip + branchTaps_)] = x;
            const float* recent = in.data() + ip;
            auto& high = high_[static_cast<std::size_t>(c)];
            int hp = highPos_;
            for (int p = 0; p < kFactor; ++p) {
                const float* h = phases_[static_cast<std::size_t>(p)].data();
                float u = 0.0f;
                for (int k = 0; k < branchTaps_; ++k)
                    u += h[k] * recent[k];
                if (std::abs(u) > knee)
                    ++touched_;
                ++counted_;
                hp = hp == 0 ? kHighRing - 1 : hp - 1;
                const float v = shape(u, clip_);
                high[static_cast<std::size_t>(hp)] = v;
                high[static_cast<std::size_t>(hp + kHighRing)] = v;
            }
            // Decimate at this step's first oversampled sample (kFactor - 1
            // newer ones follow it), so the delay is a whole number of frames.
            const float* history = high.data() + hp + (kFactor - 1);
            float z = 0.0f;
            for (int j = 0; j < kTaps; ++j)
                z += taps_[static_cast<std::size_t>(j)] * history[j];
            out[c] = dry + (z - dry) * mix_;
        }
        if (oversample) {
            inputPos_ = inputPos_ == 0 ? branchTaps_ - 1 : inputPos_ - 1;
            highPos_ = (highPos_ - kFactor + kHighRing) % kHighRing;
            mix_ += (target - mix_) * glide_;
            if (std::abs(mix_ - target) < 1e-5f)
                mix_ = target;
        }
        delayPos_ = delayPos_ + 1 == length ? 0 : delayPos_ + 1;
        io[0][i] = out[0];
        if (channels == 2)
            io[1][i] = out[1];
    }
}

double SoftClipper::levelExceededBy(const float* left, const float* right, std::int64_t frames, double share)
{
    // A histogram of the oversampled magnitudes in 0.05 dB steps from -60 dBFS up.
    constexpr double low = -60.0, step = 0.05;
    constexpr int bins = 1800; // up to +30 dBFS
    std::vector<std::uint64_t> histogram(bins, 0);
    SoftClipper upsampler;
    upsampler.prepare(48000.0); // the filters do not depend on the rate
    std::uint64_t total = 0;
    const float* channels[2] = {left, right != nullptr ? right : left};
    for (int c = 0; c < (right != nullptr && right != left ? 2 : 1); ++c) {
        std::vector<float> ring(static_cast<std::size_t>(2 * upsampler.branchTaps_), 0.0f);
        int pos = 0;
        for (std::int64_t i = 0; i < frames; ++i) {
            pos = pos == 0 ? upsampler.branchTaps_ - 1 : pos - 1;
            ring[static_cast<std::size_t>(pos)] = channels[c][i];
            ring[static_cast<std::size_t>(pos + upsampler.branchTaps_)] = channels[c][i];
            const float* recent = ring.data() + pos;
            for (int p = 0; p < kFactor; ++p) {
                const float* h = upsampler.phases_[static_cast<std::size_t>(p)].data();
                float u = 0.0f;
                for (int k = 0; k < upsampler.branchTaps_; ++k)
                    u += h[k] * recent[k];
                const double db = 20.0 * std::log10(std::abs(u) + 1e-12);
                ++histogram[static_cast<std::size_t>(std::clamp(static_cast<int>((db - low) / step), 0, bins - 1))];
                ++total;
            }
        }
    }
    const auto wanted = static_cast<std::uint64_t>(share * static_cast<double>(total));
    std::uint64_t above = 0;
    for (int b = bins - 1; b >= 0; --b) {
        above += histogram[static_cast<std::size_t>(b)];
        if (above > wanted)
            return low + (b + 1) * step;
    }
    return low;
}

double SoftClipper::takeClippedFraction() noexcept
{
    const double fraction = counted_ > 0 ? static_cast<double>(touched_) / static_cast<double>(counted_) : 0.0;
    touched_ = counted_ = 0;
    return fraction;
}

} // namespace daw
