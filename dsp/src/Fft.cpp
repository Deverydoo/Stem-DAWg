// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/dsp/Fft.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace daw {

Fft::Fft(int size) : size_(size)
{
    if (size < 2 || (size & (size - 1)) != 0)
        throw std::invalid_argument("FFT size must be a power of two");
    twiddles_.resize(static_cast<std::size_t>(size / 2));
    for (int k = 0; k < size / 2; ++k) {
        const double angle = -2.0 * std::numbers::pi * k / size;
        twiddles_[static_cast<std::size_t>(k)] = {static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
    }
    int bits = 0;
    while ((1 << bits) < size)
        ++bits;
    bitReverse_.resize(static_cast<std::size_t>(size));
    for (int i = 0; i < size; ++i) {
        int r = 0;
        for (int b = 0; b < bits; ++b)
            if (i & (1 << b))
                r |= 1 << (bits - 1 - b);
        bitReverse_[static_cast<std::size_t>(i)] = r;
    }
}

void Fft::forward(std::vector<std::complex<float>>& data) const
{
    forward(data.data());
}

void Fft::inverse(std::complex<float>* data) const noexcept
{
    // ifft(x) = conj(fft(conj(x))) / N
    for (int i = 0; i < size_; ++i)
        data[i] = std::conj(data[i]);
    forward(data);
    const float scale = 1.0f / static_cast<float>(size_);
    for (int i = 0; i < size_; ++i)
        data[i] = std::conj(data[i]) * scale;
}

void Fft::forward(std::complex<float>* data) const noexcept
{
    for (int i = 0; i < size_; ++i) {
        const int j = bitReverse_[static_cast<std::size_t>(i)];
        if (j > i)
            std::swap(data[static_cast<std::size_t>(i)], data[static_cast<std::size_t>(j)]);
    }
    for (int length = 2; length <= size_; length <<= 1) {
        const int half = length / 2;
        const int step = size_ / length;
        for (int start = 0; start < size_; start += length) {
            for (int k = 0; k < half; ++k) {
                const auto w = twiddles_[static_cast<std::size_t>(k * step)];
                auto& a = data[static_cast<std::size_t>(start + k)];
                auto& b = data[static_cast<std::size_t>(start + k + half)];
                const auto t = w * b;
                b = a - t;
                a = a + t;
            }
        }
    }
}

void Fft::powerSpectrum(const float* input, std::vector<float>& power, std::vector<std::complex<float>>& scratch) const
{
    scratch.resize(static_cast<std::size_t>(size_));
    for (int i = 0; i < size_; ++i)
        scratch[static_cast<std::size_t>(i)] = {input[i], 0.0f};
    forward(scratch);
    power.resize(static_cast<std::size_t>(size_ / 2 + 1));
    for (int k = 0; k <= size_ / 2; ++k)
        power[static_cast<std::size_t>(k)] = std::norm(scratch[static_cast<std::size_t>(k)]);
}

namespace {
int chirpFftSize(int n)
{
    if (n >= 1 && (n & (n - 1)) == 0)
        return std::max(n, 2);
    int m = 2;
    while (m < 2 * n - 1)
        m *= 2;
    return m;
}
} // namespace

ChirpFft::ChirpFft(int size) : size_(std::max(size, 1)), fft_(chirpFftSize(size_))
{
    const bool direct = (size_ & (size_ - 1)) == 0 && fft_.size() == size_;
    if (direct)
        return;
    const auto n = static_cast<std::int64_t>(size_);
    const auto m = static_cast<std::size_t>(fft_.size());
    chirp_.resize(static_cast<std::size_t>(size_));
    for (std::int64_t k = 0; k < n; ++k) {
        // k^2 mod 2n keeps the angle exact for large k.
        const double angle = std::numbers::pi * static_cast<double>((k * k) % (2 * n)) / static_cast<double>(n);
        chirp_[static_cast<std::size_t>(k)] = {static_cast<float>(std::cos(angle)), static_cast<float>(-std::sin(angle))};
    }
    kernel_.assign(m, {0.0f, 0.0f});
    kernel_[0] = std::conj(chirp_[0]);
    for (std::size_t k = 1; k < static_cast<std::size_t>(size_); ++k)
        kernel_[k] = kernel_[m - k] = std::conj(chirp_[k]);
    fft_.forward(kernel_.data());
    work_.resize(m);
}

void ChirpFft::forward(std::complex<float>* data) noexcept
{
    transform(data, false);
}

void ChirpFft::inverse(std::complex<float>* data) noexcept
{
    transform(data, true);
}

void ChirpFft::transform(std::complex<float>* data, bool inverse) noexcept
{
    if (chirp_.empty()) { // power of two
        if (size_ == 1)
            return;
        inverse ? fft_.inverse(data) : fft_.forward(data);
        return;
    }
    const auto n = static_cast<std::size_t>(size_);
    // The inverse is the conjugate of the forward transform of the conjugate.
    for (std::size_t k = 0; k < n; ++k)
        work_[k] = (inverse ? std::conj(data[k]) : data[k]) * chirp_[k];
    std::fill(work_.begin() + static_cast<std::ptrdiff_t>(n), work_.end(), std::complex<float>{});
    fft_.forward(work_.data());
    for (std::size_t k = 0; k < work_.size(); ++k)
        work_[k] *= kernel_[k];
    fft_.inverse(work_.data());
    const float scale = inverse ? 1.0f / static_cast<float>(size_) : 1.0f;
    for (std::size_t k = 0; k < n; ++k) {
        const std::complex<float> y = work_[k] * chirp_[k];
        data[k] = (inverse ? std::conj(y) : y) * scale;
    }
}

std::vector<float> hannWindow(int size)
{
    std::vector<float> window(static_cast<std::size_t>(size));
    for (int i = 0; i < size; ++i)
        window[static_cast<std::size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / size));
    return window;
}

} // namespace daw
