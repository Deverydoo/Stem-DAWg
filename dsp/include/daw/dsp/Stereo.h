// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Stereo repair (see docs/milestone-4/M4-Artifact-Repair.md, section 4).
//
// StereoImager works on the waveform (no delay):
//  * mono bass: the side signal (L - R) / 2 is high-passed (Linkwitz-Riley,
//    24 dB/octave), so everything below the frequency is mono. Separated AI
//    stems often carry a wide or out-of-phase low end.
//  * width: gain on the side signal (0 = mono, 1 = unchanged, 2 = twice as wide).
// Changes glide over kRampMs, so switching or moving a control never clicks.
//
// PhaseAligner works on the spectra of a short-time Fourier transform (the
// clean-up chain's spectral stage owns the transform). Some AI songs carry
// the same sound in both channels out of step: coherent, but tens of degrees
// apart, so it partly cancels when summed to mono (phones, Bluetooth
// speakers, club systems). Per bin it follows the smoothed cross-spectrum
// C = <L R*>; where the channels are more than kMaxPhaseDeg apart, it rotates
// them towards each other (each by half), weighted by their coherence
// |C|^2 / (<|L|^2> <|R|^2>). Incoherent sound (real width, reverb,
// independent parts) has no consistent phase to correct and is left alone.
// A rotation keeps each channel's level at every frequency: stereo listeners
// lose nothing, and the mono sum gets back what cancelled. A few frames
// always look coherent, so after a reset the rotations fade in over the
// averaging time.
//
// Both are real-time safe after prepare().

#include <daw/dsp/Biquad.h>

#include <array>
#include <complex>
#include <vector>

namespace daw {

struct StereoImagerParams {
    bool monoBass = false;
    float monoBelowHz = 120.0f;
    float width = 1.0f; // side gain, 0 .. 2

    friend bool operator==(const StereoImagerParams&, const StereoImagerParams&) = default;
};

class StereoImager {
public:
    static constexpr double kRampMs = 20.0;
    static constexpr double kMinHz = 40.0;
    static constexpr double kMaxHz = 300.0;
    static constexpr double kMaxWidth = 2.0;

    void prepare(double sampleRate) noexcept;
    void setParams(const StereoImagerParams& params) noexcept;
    const StereoImagerParams& params() const noexcept { return params_; }
    void reset() noexcept;

    /// True while the imager changes the sound or is still gliding back to neutral.
    bool active() const noexcept;
    void process(float* left, float* right, int numFrames) noexcept;

private:
    double sampleRate_ = 48000.0;
    StereoImagerParams params_;
    std::array<Biquad, 2> highPass_{}; // two Butterworth sections = Linkwitz-Riley 4th order
    float ramp_ = 0.0f;                // per-sample glide coefficient
    float mono_ = 0.0f;                // 0 .. 1, gliding towards params_.monoBass
    float width_ = 1.0f;               // gliding towards params_.width
};

class PhaseAligner {
public:
    static constexpr double kMaxPhaseDeg = 45.0;   // phase differences up to this are left alone
    static constexpr double kSmoothingMs = 200.0;  // cross-spectrum averaging
    static constexpr double kCoherenceLow = 0.2;   // no rotation below this coherence ...
    static constexpr double kCoherenceHigh = 0.5;  // ... the full rotation above this
    static constexpr double kLowestHz = 120.0;     // the low end is mono bass's job

    /// Allocates per-bin state for spectra of `fftSize` points every `hop` samples.
    void prepare(double sampleRate, int fftSize, int hop);
    void reset() noexcept;
    int bins() const noexcept { return static_cast<int>(rotation_.size()); }

    /// Updates the statistics from one frame (bins() values per channel) and
    /// computes the rotations.
    void analyse(const std::complex<float>* left, const std::complex<float>* right) noexcept;
    /// Rotation of a bin after the last analyse(), in radians: the left
    /// channel turns by -rotation/2, the right by +rotation/2.
    float rotation(int bin) const noexcept { return rotation_[static_cast<std::size_t>(bin)]; }
    /// Applies the rotations to a frame in place.
    void apply(std::complex<float>* left, std::complex<float>* right) const noexcept;

private:
    float coeff_ = 0.0f;
    int first_ = 1;
    int settleFrames_ = 1; // frames in kSmoothingMs
    int frames_ = 0;       // since the last reset
    std::vector<std::complex<float>> cross_; // smoothed L R*
    std::vector<float> powerLeft_, powerRight_;
    std::vector<float> phase_;    // last phase difference, unwrapped near +-pi so the direction holds
    std::vector<float> rotation_;
};

} // namespace daw
