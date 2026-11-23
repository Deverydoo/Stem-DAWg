// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Small iterative radix-2 FFT. Construction allocates; the transforms do not,
// so a prepared Fft is safe on the audio thread (the spectral denoiser uses it).

#include <complex>
#include <vector>

namespace daw {

class Fft {
public:
    explicit Fft(int size); // size must be a power of two >= 2

    int size() const noexcept { return size_; }

    /// In-place forward transform.
    void forward(std::vector<std::complex<float>>& data) const;
    /// In-place forward transform of size() values. Real-time safe.
    void forward(std::complex<float>* data) const noexcept;
    /// In-place inverse transform of size() values, scaled by 1/size() (so
    /// inverse(forward(x)) == x). Real-time safe.
    void inverse(std::complex<float>* data) const noexcept;

    /// |X[k]|^2 for k = 0..size/2 of a real input of length size().
    void powerSpectrum(const float* input, std::vector<float>& power, std::vector<std::complex<float>>& scratch) const;

private:
    int size_;
    std::vector<std::complex<float>> twiddles_;
    std::vector<int> bitReverse_;
};

/// FFT of any size, by Bluestein's chirp-z algorithm on a power-of-two Fft
/// (sizes that are already powers of two use it directly). Construction
/// allocates; the transforms do not. Used where a model fixes the frame size
/// (DeepFilterNet's 960-point frames).
class ChirpFft {
public:
    explicit ChirpFft(int size); // size >= 1

    int size() const noexcept { return size_; }

    /// In-place forward transform of size() values. Real-time safe.
    void forward(std::complex<float>* data) noexcept;
    /// In-place inverse transform of size() values, scaled by 1/size().
    void inverse(std::complex<float>* data) noexcept;

private:
    void transform(std::complex<float>* data, bool inverse) noexcept;

    int size_;
    Fft fft_;                                  // size >= 2 * size - 1
    std::vector<std::complex<float>> chirp_;   // exp(-i pi k^2 / n), k < n
    std::vector<std::complex<float>> kernel_;  // FFT of the conjugate chirp, wrapped
    std::vector<std::complex<float>> work_;
};

/// Periodic Hann window.
std::vector<float> hannWindow(int size);

} // namespace daw
