// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// The signal processing around the DeepFilterNet3 speech-enhancement model
// (Schröter et al., 2023): everything but the neural network. It mirrors the
// reference implementation (libDF) exactly, because the model was trained on
// these features:
//
//  * STFT: 960-point frames (20 ms at 48 kHz), hop 480, Vorbis window, the
//    forward transform scaled by 2 * hop / N^2. Analysis plus synthesis
//    reconstructs the input delayed by one hop (Princen-Bradley window).
//  * Features per frame: the log power of 32 ERB bands, mean-normalised; and
//    the complex spectrum of the lowest 96 bins, unit-normalised. Both
//    normalisations are exponential averages with a 1 s time constant.
//  * Output: the network's 32 ERB gains scale the spectrum above 96 bins; a
//    5-tap complex "deep filter" across frames (2 frames of lookahead)
//    rebuilds the lowest 96 bins, where speech harmonics live.
//
// No allocation after construction. Not thread-safe; one instance per channel.

#include <daw/dsp/Fft.h>

#include <array>
#include <complex>
#include <vector>

namespace daw::deepfilter {

inline constexpr int kSampleRate = 48000;
inline constexpr int kFftSize = 960;
inline constexpr int kHop = 480;
inline constexpr int kBins = kFftSize / 2 + 1; // 481
inline constexpr int kErbBands = 32;
inline constexpr int kDfBins = 96;
inline constexpr int kDfOrder = 5;
inline constexpr int kLookahead = 2; // frames the network looks ahead (conv and deep filter)

using Spectrum = std::array<std::complex<float>, kBins>;

/// Bins per ERB band (sums to kBins), as libDF's erb_fb with at least 2 bins per band.
const std::array<int, kErbBands>& erbBandWidths();

/// The Vorbis window, sin(pi/2 sin^2(pi (i + 0.5) / N)).
const std::array<float, kFftSize>& window();

/// Streaming STFT analysis: one hop of samples in, one spectrum out.
class Analysis {
public:
    Analysis();
    void reset() noexcept;
    void process(const float* hop, Spectrum& out) noexcept;

private:
    ChirpFft fft_;
    std::array<float, kHop> memory_{};
    std::array<std::complex<float>, kFftSize> buffer_{};
};

/// Streaming STFT synthesis (overlap-add): one spectrum in, one hop of samples out.
class Synthesis {
public:
    Synthesis();
    void reset() noexcept;
    void process(const Spectrum& in, float* hop) noexcept;

private:
    ChirpFft fft_;
    std::array<float, kHop> memory_{};
    std::array<std::complex<float>, kFftSize> buffer_{};
};

/// The network's input features, with their running normalisation.
class Features {
public:
    Features() { reset(); }
    void reset() noexcept;
    /// erb: kErbBands values. spec: the normalised complex spectrum of the
    /// lowest kDfBins bins as kDfBins real parts followed by kDfBins imaginary parts.
    void compute(const Spectrum& spectrum, float* erb, float* spec) noexcept;

    static float alpha() noexcept; // per-frame smoothing of both normalisations

private:
    std::array<float, kErbBands> meanState_{};
    std::array<float, kDfBins> unitState_{};
};

/// Multiplies each ERB band of `spectrum` by its gain (the network's mask).
void applyErbGains(Spectrum& spectrum, const float* gains) noexcept;

/// The deep filter: replaces the lowest kDfBins bins of `out` with
/// sum over i of frames[i][f] * c(f, i), where frames[0..kDfOrder) run from
/// oldest to newest and `coefs` holds kDfBins x kDfOrder x (re, im).
void applyDeepFilter(const Spectrum* const* frames, const float* coefs, Spectrum& out) noexcept;

} // namespace daw::deepfilter
