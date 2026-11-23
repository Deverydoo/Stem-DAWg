// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/dsp/SpectralDenoiser.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace daw {

SpectralDenoiser::SpectralDenoiser() : fft_(kFftSize) {}

std::vector<float> SpectralDenoiser::window()
{
    // sqrt(periodic Hann): analysis x synthesis = Hann, which sums to 2 at 75 % overlap.
    std::vector<float> w(static_cast<std::size_t>(kFftSize));
    for (int i = 0; i < kFftSize; ++i)
        w[static_cast<std::size_t>(i)] =
            static_cast<float>(std::sqrt(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / kFftSize)));
    return w;
}

void SpectralDenoiser::prepare(double sampleRate)
{
    window_ = window();
    noise_.assign(kBins, 0.0f);
    lastGain_.assign(kBins, 1.0f);
    lastSnr_.assign(kBins, 1.0f);
    power_.assign(kBins, 0.0f);
    gain_.assign(kBins, 1.0f);
    resonance_.prepare(sampleRate, kFftSize, kHop);
    for (int c = 0; c < 2; ++c) {
        input_[c].assign(kFftSize, 0.0f);
        output_[c].assign(kFftSize, 0.0f);
    }
    spectrum_.assign(kFftSize, {});
    hasProfile_ = false;
    setParams(params_);
    reset();
}

void SpectralDenoiser::setParams(const DenoiseParams& params) noexcept
{
    params_ = params;
    noiseScale_ = std::pow(10.0f, params.sensitivityDb / 10.0f);
    floorGain_ = std::pow(10.0f, -std::max(0.0f, params.reductionDb) / 20.0f);
}

void SpectralDenoiser::setNoiseProfile(const float* power) noexcept
{
    hasProfile_ = power != nullptr;
    if (power != nullptr && !noise_.empty())
        std::copy_n(power, kBins, noise_.begin());
}

void SpectralDenoiser::reset() noexcept
{
    for (int c = 0; c < 2; ++c) {
        std::fill(input_[c].begin(), input_[c].end(), 0.0f);
        std::fill(output_[c].begin(), output_[c].end(), 0.0f);
    }
    std::fill(lastGain_.begin(), lastGain_.end(), 1.0f);
    std::fill(lastSnr_.begin(), lastSnr_.end(), 1.0f);
    fill_ = 0;
}

void SpectralDenoiser::framePower(const float* left, const float* right, float* power, std::complex<float>* scratch,
                                  const Fft& fft, const float* window) noexcept
{
    for (int i = 0; i < kFftSize; ++i)
        scratch[i] = {left[i] * window[i], right[i] * window[i]};
    fft.forward(scratch);
    // With z = l + i r: |L[k]|^2 + |R[k]|^2 = (|Z[k]|^2 + |Z[N-k]|^2) / 2.
    for (int k = 0; k < kBins; ++k) {
        const int mirror = (kFftSize - k) % kFftSize;
        power[k] = 0.25f * (std::norm(scratch[k]) + std::norm(scratch[mirror]));
    }
}

void SpectralDenoiser::processFrame() noexcept
{
    std::complex<float>* z = spectrum_.data();
    const float* w = window_.data();
    for (int i = 0; i < kFftSize; ++i)
        z[i] = {input_[0][static_cast<std::size_t>(i)] * w[i], input_[1][static_cast<std::size_t>(i)] * w[i]};
    fft_.forward(z);

    const bool resonance = resonance_.active();
    if (hasProfile_ || resonance) {
        float* power = power_.data();
        float* gain = gain_.data();
        for (int k = 0; k < kBins; ++k) {
            const int mirror = (kFftSize - k) % kFftSize;
            power[k] = 0.25f * (std::norm(z[k]) + std::norm(z[mirror]));
            gain[k] = 1.0f;
        }
        if (hasProfile_) {
            for (int k = 0; k < kBins; ++k) {
                const auto i = static_cast<std::size_t>(k);
                const float noise = noise_[i] * noiseScale_ + 1e-20f;
                const float snr = power[k] / noise; // a-posteriori
                const float prior = kSmoothing * lastGain_[i] * lastGain_[i] * lastSnr_[i]
                                    + (1.0f - kSmoothing) * std::max(snr - 1.0f, 0.0f);
                gain[k] = std::max(prior / (1.0f + prior), floorGain_);
                lastGain_[i] = gain[k];
                lastSnr_[i] = std::min(snr, 1e6f);
            }
        }
        if (resonance)
            resonance_.process(power, gain);
        for (int k = 0; k < kBins; ++k) {
            const int mirror = (kFftSize - k) % kFftSize;
            z[k] *= gain[k];
            if (mirror != k)
                z[mirror] *= gain[k];
        }
    }

    fft_.inverse(z);
    // Shift the accumulator by one hop and overlap-add the new frame.
    // Hann windows at 75 % overlap sum to 2: scale by 1/2.
    for (int c = 0; c < 2; ++c) {
        float* out = output_[c].data();
        std::copy(out + kHop, out + kFftSize, out);
        std::fill(out + kFftSize - kHop, out + kFftSize, 0.0f);
        for (int i = 0; i < kFftSize; ++i)
            out[i] += 0.5f * w[i] * (c == 0 ? z[i].real() : z[i].imag());
        float* in = input_[c].data();
        std::copy(in + kHop, in + kFftSize, in);
    }
}

void SpectralDenoiser::process(float* left, float* right, int numFrames) noexcept
{
    float* io[2] = {left, right};
    for (int i = 0; i < numFrames; ++i) {
        for (int c = 0; c < 2; ++c) {
            input_[c][static_cast<std::size_t>(kFftSize - kHop + fill_)] = io[c][i];
            io[c][i] = output_[c][static_cast<std::size_t>(fill_)];
        }
        if (++fill_ == kHop) {
            processFrame();
            fill_ = 0;
        }
    }
}

// ---------------------------------------------------------------------------

void HumFilter::prepare(double sampleRate) noexcept
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    setParams(fundamental_, count_);
    reset();
}

void HumFilter::setParams(double fundamentalHz, int harmonics) noexcept
{
    fundamental_ = fundamentalHz;
    int count = 0;
    for (int h = 1; h <= std::clamp(harmonics, 0, kMaxHarmonics) && fundamentalHz > 0.0; ++h) {
        const double hz = fundamentalHz * h;
        if (hz >= 0.45 * sampleRate_)
            break;
        // RBJ notch with a constant bandwidth in Hz.
        const double w0 = 2.0 * std::numbers::pi * hz / sampleRate_;
        const double q = hz / kBandwidthHz;
        const double alpha = std::sin(w0) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        Section& s = sections_[count];
        s.b0 = 1.0 / a0;
        s.b1 = -2.0 * std::cos(w0) / a0;
        s.b2 = 1.0 / a0;
        s.a1 = -2.0 * std::cos(w0) / a0;
        s.a2 = (1.0 - alpha) / a0;
        ++count;
    }
    // Notches that were already running keep their state (no click when the
    // frequency is nudged); newly added ones start clean.
    for (int k = count_; k < count; ++k)
        for (auto& channel : sections_[k].z)
            channel[0] = channel[1] = 0.0;
    count_ = count;
}

void HumFilter::reset() noexcept
{
    for (Section& s : sections_)
        for (auto& channel : s.z)
            channel[0] = channel[1] = 0.0;
}

void HumFilter::process(float* left, float* right, int numFrames) noexcept
{
    float* io[2] = {left, right};
    for (int k = 0; k < count_; ++k) {
        Section& s = sections_[k];
        for (int c = 0; c < 2; ++c) {
            double z1 = s.z[c][0], z2 = s.z[c][1];
            float* x = io[c];
            for (int i = 0; i < numFrames; ++i) {
                const double in = x[i];
                const double y = s.b0 * in + z1;
                z1 = s.b1 * in - s.a1 * y + z2;
                z2 = s.b2 * in - s.a2 * y;
                x[i] = static_cast<float>(y);
            }
            s.z[c][0] = z1;
            s.z[c][1] = z2;
        }
    }
}

} // namespace daw
