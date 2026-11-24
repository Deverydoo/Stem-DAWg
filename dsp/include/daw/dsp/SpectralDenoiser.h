// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// The clean-up chain's spectral stage: learned-profile noise reduction for
// stationary noise (fans, air conditioning, hiss, the steady part of a
// separation bed), resonance suppression (harshness and shimmer control, see
// ResonanceSuppressor) and phase alignment (see PhaseAligner in Stereo.h).
// All three work on the same frames, so each costs no extra delay.
//
// Short-time Fourier transform: kFftSize-point frames, 75 % overlap, square-root
// Hann analysis and synthesis windows (perfect reconstruction). Both channels
// share one complex FFT (left + i*right) and one real gain per bin, so stereo
// costs one forward and one inverse transform per hop and the image is kept.
//
// Gain per bin: a Wiener gain from the decision-directed a-priori SNR
// (Ephraim and Malah, 1984), which follows the music without the warbling
// "musical noise" of plain spectral subtraction, limited to `reductionDb` of
// attenuation. The noise power per bin comes from a profile measured on the
// quietest parts of the track (session/NoiseProfile).
//
// The resonance gains are measured on the same input spectrum and multiply
// the noise gains. Phase alignment then turns each channel's bins; it needs
// the channels apart (splitChannels), which the gains alone do not.
//
// Delay: exactly latency() == kFftSize frames. Real-time safe after prepare().

#include <daw/dsp/Fft.h>
#include <daw/dsp/ResonanceSuppressor.h>
#include <daw/dsp/Stereo.h>

#include <complex>
#include <vector>

namespace daw {

struct DenoiseParams {
    float reductionDb = 12.0f;  // maximum attenuation per bin
    float sensitivityDb = 0.0f; // scales the profile: + removes more, - keeps more
};

class SpectralDenoiser {
public:
    static constexpr int kFftSize = 2048;
    static constexpr int kHop = kFftSize / 4;
    static constexpr int kBins = kFftSize / 2 + 1;
    static constexpr float kSmoothing = 0.98f; // decision-directed weight

    SpectralDenoiser();

    /// Allocates the buffers (not real-time safe) and resets, including the
    /// resonance detector's memory.
    void prepare(double sampleRate = 48000.0);
    static constexpr int latency() noexcept { return kFftSize; }

    void setParams(const DenoiseParams& params) noexcept;
    /// Copies the noise power per bin (kBins values, the units framePower()
    /// produces). Null clears the profile: the gain is then 1.
    void setNoiseProfile(const float* power) noexcept;
    bool hasProfile() const noexcept { return hasProfile_; }

    /// Harshness and shimmer depths; zero depths turn resonance suppression off.
    void setResonance(const ResonanceParams& params) noexcept { resonance_.setParams(params); }
    const ResonanceSuppressor& resonance() const noexcept { return resonance_; }

    /// Phase alignment on or off. It starts from fresh statistics when switched on.
    void setPhaseAlign(bool on) noexcept;
    bool phaseAlign() const noexcept { return align_; }

    /// Clears the frames and the noise gains. The resonance detector keeps its
    /// memory (see ResonanceSuppressor).
    void reset() noexcept;
    /// Processes a stereo block in place; the output is delayed by latency().
    void process(float* left, float* right, int numFrames) noexcept;

    /// Mean power of both channels per bin for one frame of kFftSize samples
    /// starting at the given pointers, windowed exactly as process() windows
    /// it. Used to learn profiles. `scratch` must hold kFftSize values.
    static void framePower(const float* left, const float* right, float* power, std::complex<float>* scratch,
                           const Fft& fft, const float* window) noexcept;
    /// Separates the spectrum of z = l + i r (kFftSize values) into the
    /// spectra of l and r (kBins values each), and joins them back.
    static void splitChannels(const std::complex<float>* z, std::complex<float>* left, std::complex<float>* right) noexcept;
    static void joinChannels(const std::complex<float>* left, const std::complex<float>* right, std::complex<float>* z) noexcept;
    /// The square-root Hann window used for analysis and synthesis.
    static std::vector<float> window();

private:
    void processFrame() noexcept;

    Fft fft_;
    DenoiseParams params_;
    float noiseScale_ = 1.0f;
    float floorGain_ = 0.25f;
    bool hasProfile_ = false;
    ResonanceSuppressor resonance_;
    PhaseAligner aligner_;
    bool align_ = false;

    std::vector<float> window_;
    std::vector<float> noise_;      // per bin
    std::vector<float> lastGain_;   // per bin, previous frame
    std::vector<float> lastSnr_;    // per bin, previous a-posteriori SNR
    std::vector<float> power_;      // per bin, current frame
    std::vector<float> gain_;       // per bin, current frame
    std::vector<std::complex<float>> left_, right_; // per bin, current frame, for phase alignment
    std::vector<float> input_[2];   // last kFftSize input samples
    std::vector<float> output_[2];  // overlap-add accumulator
    std::vector<std::complex<float>> spectrum_;
    int fill_ = 0;                  // samples of the current hop
};

/// Notch filters at a mains frequency and its harmonics (hum removal).
/// Fixed 3 Hz notch bandwidth, so harmonics are cut as narrowly as the
/// fundamental. Real-time safe.
class HumFilter {
public:
    static constexpr int kMaxHarmonics = 8;
    static constexpr double kBandwidthHz = 3.0;

    void prepare(double sampleRate) noexcept;
    /// `harmonics` notches at f0, 2 f0, ... (those below 0.45 x the rate).
    void setParams(double fundamentalHz, int harmonics) noexcept;
    void reset() noexcept;
    void process(float* left, float* right, int numFrames) noexcept;

private:
    struct Section {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z[2][2] = {{0, 0}, {0, 0}};
    };
    double sampleRate_ = 48000.0;
    double fundamental_ = 0.0;
    int count_ = 0;
    Section sections_[kMaxHarmonics];
};

} // namespace daw
