// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/dsp/BleedSuppressor.h>

#include <algorithm>
#include <cmath>

namespace daw {

namespace {

constexpr float kPi = 3.14159265358979323846f;

} // namespace

BleedSuppressor::BleedSuppressor(int numStems)
    : fft_(kFftSize)
{
    window_.resize(kFftSize);
    for (int i = 0; i < kFftSize; ++i)
        window_[static_cast<std::size_t>(i)] = std::sqrt(0.5f - 0.5f * std::cos(2.0f * kPi * static_cast<float>(i) / kFftSize));
    stems_.resize(static_cast<std::size_t>(std::max(1, numStems)));
    for (Stem& s : stems_) {
        for (int c = 0; c < 2; ++c) {
            s.input[c].assign(kFftSize, 0.0f);
            s.output[c].assign(kFftSize, 0.0f);
        }
        s.spectra.assign(static_cast<std::size_t>(kFrames) * kFftSize, {});
        s.amplitude.assign(static_cast<std::size_t>(kFrames) * kBins, 0.0f);
        s.gains.assign(static_cast<std::size_t>(kMaxHops) * kBins, 1.0f);
        s.scratch.assign(kFftSize, {});
        s.raw.assign(kBins, 0.0f);
    }
    share_.assign(stems_.size(), 0.0f);
    setParams(params_);
}

void BleedSuppressor::setParams(const BleedParams& params) noexcept
{
    params_ = params;
    params_.exponent = std::clamp(params.exponent, 1.0f, 4.0f);
    floorGain_ = std::pow(10.0f, -std::max(0.0f, params.reductionDb) / 20.0f);
}

void BleedSuppressor::reset() noexcept
{
    for (Stem& s : stems_) {
        for (int c = 0; c < 2; ++c) {
            std::fill(s.input[c].begin(), s.input[c].end(), 0.0f);
            std::fill(s.output[c].begin(), s.output[c].end(), 0.0f);
        }
        std::fill(s.spectra.begin(), s.spectra.end(), std::complex<float>{});
        std::fill(s.amplitude.begin(), s.amplitude.end(), 0.0f);
        std::fill(s.gains.begin(), s.gains.end(), 1.0f);
        s.hops = 0;
    }
}

void BleedSuppressor::analyse(int stem, const float* left, const float* right, int hops) noexcept
{
    Stem& s = stems_[static_cast<std::size_t>(stem)];
    hops = std::clamp(hops, 1, kMaxHops);
    // Carry the previous block's last two frames to the front.
    if (s.hops > 0) {
        std::copy_n(s.spectra.begin() + static_cast<std::ptrdiff_t>(s.hops) * kFftSize, 2 * kFftSize, s.spectra.begin());
        std::copy_n(s.amplitude.begin() + static_cast<std::ptrdiff_t>(s.hops) * kBins, 2 * kBins, s.amplitude.begin());
    }
    s.hops = hops;
    const float* w = window_.data();
    for (int h = 0; h < hops; ++h) {
        const float* in[2] = {left + static_cast<std::ptrdiff_t>(h) * kHop,
                              (right != nullptr ? right : left) + static_cast<std::ptrdiff_t>(h) * kHop};
        for (int c = 0; c < 2; ++c) {
            float* buffer = s.input[c].data();
            std::copy(buffer + kHop, buffer + kFftSize, buffer);
            std::copy_n(in[c], kHop, buffer + kFftSize - kHop);
        }
        std::complex<float>* z = s.spectra.data() + static_cast<std::ptrdiff_t>(h + 2) * kFftSize;
        for (int i = 0; i < kFftSize; ++i)
            z[i] = {s.input[0][static_cast<std::size_t>(i)] * w[i], s.input[1][static_cast<std::size_t>(i)] * w[i]};
        fft_.forward(z);
        // With z = l + i r: |L[k]|^2 + |R[k]|^2 = (|Z[k]|^2 + |Z[N-k]|^2) / 2.
        for (int k = 0; k < kBins; ++k) {
            const int mirror = (kFftSize - k) % kFftSize;
            s.raw[static_cast<std::size_t>(k)] = std::sqrt(0.25f * (std::norm(z[k]) + std::norm(z[mirror])));
        }
        float* a = s.amplitude.data() + static_cast<std::ptrdiff_t>(h + 2) * kBins;
        const float* r = s.raw.data();
        for (int k = 0; k < kBins; ++k)
            a[k] = r[std::max(k - 1, 0)] + r[k] + r[std::min(k + 1, kBins - 1)];
    }
}

void BleedSuppressor::computeGains() noexcept
{
    const int hops = stems_.front().hops;
    const float p = params_.exponent;
    const std::size_t count = stems_.size();
    // m^p and m^(p-1) for the common strengths without std::pow.
    const int mode = p == 1.0f ? 0 : p == 2.0f ? 2 : p == 3.0f ? 3 : p == 1.5f ? 15 : -1;
    for (int i = 1; i <= hops; ++i) {
        for (int k = 0; k < kBins; ++k) {
            float total = 0.0f;
            for (std::size_t n = 0; n < count; ++n) {
                const float* a = stems_[n].amplitude.data() + k;
                const float v = a[(i - 1) * kBins] + a[i * kBins] + a[(i + 1) * kBins];
                share_[n] = v;
                total += v;
            }
            const std::size_t at = static_cast<std::size_t>(i - 1) * kBins + static_cast<std::size_t>(k);
            if (mode == 0 || count == 1 || !(total > 1e-20f)) {
                for (Stem& s : stems_)
                    s.gains[at] = 1.0f;
                continue;
            }
            float sum = 0.0f;
            for (std::size_t n = 0; n < count; ++n) {
                const float m = share_[n] / total;
                float pm1 = 0.0f;
                switch (mode) {
                case 2: pm1 = m; break;
                case 3: pm1 = m * m; break;
                case 15: pm1 = std::sqrt(m); break;
                default: pm1 = std::pow(m, p - 1.0f); break;
                }
                share_[n] = pm1;     // m^(p-1)
                sum += pm1 * m;      // m^p
            }
            for (std::size_t n = 0; n < count; ++n)
                stems_[n].gains[at] = std::clamp(share_[n] / sum, floorGain_, 1.0f);
        }
    }
}

void BleedSuppressor::synthesise(int stem, float* left, float* right) noexcept
{
    Stem& s = stems_[static_cast<std::size_t>(stem)];
    const float* w = window_.data();
    std::complex<float>* z = s.scratch.data();
    for (int i = 1; i <= s.hops; ++i) {
        std::copy_n(s.spectra.data() + static_cast<std::ptrdiff_t>(i) * kFftSize, kFftSize, z);
        const float* g = s.gains.data() + static_cast<std::ptrdiff_t>(i - 1) * kBins;
        for (int k = 0; k < kBins; ++k) {
            const int mirror = (kFftSize - k) % kFftSize;
            z[k] *= g[k];
            if (mirror != k)
                z[mirror] *= g[k];
        }
        fft_.inverse(z);
        // Overlap-add; square-root Hann squared at 75 % overlap sums to 2.
        float* out[2] = {s.output[0].data(), s.output[1].data()};
        for (int n = 0; n < kFftSize; ++n) {
            out[0][n] += 0.5f * w[n] * z[n].real();
            out[1][n] += 0.5f * w[n] * z[n].imag();
        }
        const std::ptrdiff_t offset = static_cast<std::ptrdiff_t>(i - 1) * kHop;
        std::copy_n(out[0], kHop, left + offset);
        if (right != nullptr)
            std::copy_n(out[1], kHop, right + offset);
        for (int c = 0; c < 2; ++c) {
            std::copy(out[c] + kHop, out[c] + kFftSize, out[c]);
            std::fill(out[c] + kFftSize - kHop, out[c] + kFftSize, 0.0f);
        }
    }
}

void BleedSuppressor::process(const float* const* left, const float* const* right, float* const* outLeft,
                              float* const* outRight, int hops) noexcept
{
    for (int n = 0; n < numStems(); ++n)
        analyse(n, left[n], right != nullptr ? right[n] : nullptr, hops);
    computeGains();
    for (int n = 0; n < numStems(); ++n)
        if (outLeft != nullptr && outLeft[n] != nullptr)
            synthesise(n, outLeft[n], outRight != nullptr ? outRight[n] : nullptr);
}

float BleedSuppressor::lastGain(int stem, int bin) const noexcept
{
    const Stem& s = stems_[static_cast<std::size_t>(stem)];
    if (s.hops <= 0 || bin < 0 || bin >= kBins)
        return 1.0f;
    return s.gains[static_cast<std::size_t>(s.hops - 1) * kBins + static_cast<std::size_t>(bin)];
}

} // namespace daw
