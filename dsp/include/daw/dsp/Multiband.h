// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Three-band downward compressor for the master bus (milestone 5). It
// evens out the loud moments of each band before the limiter, so a loud
// master needs less limiting.
//
// Crossovers: Linkwitz-Riley 24 dB/octave at kCrossoverHz. The low band
// also passes an all-pass at the upper crossover, so all three bands share
// one phase and add up to the input with a flat magnitude (an all-pass of
// the whole signal). Switched off, the signal skips the bands entirely;
// switching glides over kGlideMs.
//
// Per band, stereo-linked: an RMS detector (kDetectorMs), a soft-knee gain
// computer (threshold, ratio, kKneeDb) and attack/release ballistics on the
// gain, slower for the low band so the waveform of a bass note is not
// shaped. No lookahead, no delay: the limiter after it catches what gets
// through. No make-up gain: the master fader (Match Loudness) sets the level.
//
// Real-time safe after prepare().

#include <daw/dsp/Biquad.h>

#include <array>

namespace daw {

inline constexpr std::size_t kMultibandBands = 3;
inline constexpr std::array<double, kMultibandBands - 1> kCrossoverHz{120.0, 2500.0};

struct MultibandParams {
    bool enabled = false;
    std::array<float, kMultibandBands> thresholdDb{-20.0f, -20.0f, -20.0f}; // RMS level, dBFS
    std::array<float, kMultibandBands> ratio{2.0f, 2.0f, 2.0f};             // >= 1

    friend bool operator==(const MultibandParams&, const MultibandParams&) = default;
};

class MultibandCompressor {
public:
    static constexpr double kDetectorMs = 10.0;
    static constexpr double kKneeDb = 6.0;
    static constexpr double kGlideMs = 20.0;
    static constexpr std::array<double, kMultibandBands> kAttackMs{30.0, 15.0, 5.0};
    static constexpr std::array<double, kMultibandBands> kReleaseMs{200.0, 150.0, 100.0};
    static constexpr double kMaxRatio = 10.0;

    void prepare(double sampleRate) noexcept;
    void setParams(const MultibandParams& params) noexcept;
    const MultibandParams& params() const noexcept { return params_; }
    void reset() noexcept;
    /// True while it changes the sound or glides back to bypass.
    bool active() const noexcept { return params_.enabled || mix_ > 0.0f; }
    void process(float* left, float* right, int numFrames) noexcept;

    /// Deepest gain reduction per band since the last call, dB (<= 0).
    std::array<float, kMultibandBands> takeMinGainDb() noexcept;

    /// Static curve: gain reduction in dB (<= 0) for a level, with soft knee.
    static double gainDb(double levelDb, double thresholdDb, double ratio) noexcept;

    /// The three bands of one sample, in place of the filters' state (for
    /// analysis, e.g. measuring each band's level). Not for the audio path.
    struct Splitter {
        void prepare(double sampleRate) noexcept;
        void reset() noexcept;
        std::array<double, kMultibandBands> split(double x) noexcept;

    private:
        std::array<Biquad, 2> lowA_{}, highA_{}, lowB_{}, highB_{};
        Biquad allPassB_;
    };

private:
    double sampleRate_ = 48000.0;
    MultibandParams params_;
    std::array<Splitter, 2> splitters_{};
    std::array<double, kMultibandBands> power_{};  // detector, mean of both channels
    std::array<double, kMultibandBands> gainDb_{}; // smoothed
    std::array<float, kMultibandBands> minGainDb_{};
    double detectorCoeff_ = 0.0;
    std::array<double, kMultibandBands> attackCoeff_{}, releaseCoeff_{};
    float mix_ = 0.0f; // 0 bypassed .. 1 processed
    float glide_ = 0.0f;
};

} // namespace daw
