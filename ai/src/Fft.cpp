// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/ai/Fft.h>

#include <cmath>
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

std::vector<float> hannWindow(int size)
{
    std::vector<float> window(static_cast<std::size_t>(size));
    for (int i = 0; i < size; ++i)
        window[static_cast<std::size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / size));
    return window;
}

} // namespace daw
