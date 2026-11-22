// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Small iterative radix-2 FFT for offline analysis (not used on the audio thread).

#include <complex>
#include <vector>

namespace daw {

class Fft {
public:
    explicit Fft(int size); // size must be a power of two >= 2

    int size() const noexcept { return size_; }

    /// In-place forward transform.
    void forward(std::vector<std::complex<float>>& data) const;

    /// |X[k]|^2 for k = 0..size/2 of a real input of length size().
    void powerSpectrum(const float* input, std::vector<float>& power, std::vector<std::complex<float>>& scratch) const;

private:
    int size_;
    std::vector<std::complex<float>> twiddles_;
    std::vector<int> bitReverse_;
};

/// Periodic Hann window.
std::vector<float> hannWindow(int size);

} // namespace daw
