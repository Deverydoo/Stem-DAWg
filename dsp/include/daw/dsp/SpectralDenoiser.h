// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Learned-profile spectral noise reduction for stationary noise (fans, air
// conditioning, hiss, the steady part of a separation bed).
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
// Delay: exactly latency() == kFftSize frames. Real-time safe after prepare().

#include <daw/dsp/Fft.h>

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

    /// Allocates the buffers (not real-time safe) and resets.
    void prepare();
    static constexpr int latency() noexcept { return kFftSize; }

    void setParams(const DenoiseParams& params) noexcept;
    /// Copies the noise power per bin (kBins values, the units framePower()
    /// produces). Null clears the profile: the gain is then 1.
    void setNoiseProfile(const float* power) noexcept;
    bool hasProfile() const noexcept { return hasProfile_; }

    void reset() noexcept;
    /// Processes a stereo block in place; the output is delayed by latency().
    void process(float* left, float* right, int numFrames) noexcept;

    /// Mean power of both channels per bin for one frame of kFftSize samples
    /// starting at the given pointers, windowed exactly as process() windows
    /// it. Used to learn profiles. `scratch` must hold kFftSize values.
    static void framePower(const float* left, const float* right, float* power, std::complex<float>* scratch,
                           const Fft& fft, const float* window) noexcept;
    /// The square-root Hann window used for analysis and synthesis.
    static std::vector<float> window();

private:
    void processFrame() noexcept;

    Fft fft_;
    DenoiseParams params_;
    float noiseScale_ = 1.0f;
    float floorGain_ = 0.25f;
    bool hasProfile_ = false;

    std::vector<float> window_;
    std::vector<float> noise_;      // per bin
    std::vector<float> lastGain_;   // per bin, previous frame
    std::vector<float> lastSnr_;    // per bin, previous a-posteriori SNR
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
