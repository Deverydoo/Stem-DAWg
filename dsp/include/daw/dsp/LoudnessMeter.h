// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Loudness measurement after ITU-R BS.1770-4 and EBU R 128 / Tech 3341 / 3342:
//
//  * momentary loudness   400 ms window, updated every 100 ms
//  * short-term loudness  3 s window, updated every 100 ms
//  * integrated loudness  whole programme, gated (absolute -70 LUFS, relative -10 LU)
//  * loudness range (LRA) spread of the short-term loudness (10th..95th
//                         percentile after a -70 LUFS / -20 LU gate)
//  * true peak            4x oversampled peak (see TruePeak.h), and the sample peak
//
// The meter keeps its gating statistics in fixed-size histograms (0.02 LU
// bins holding the exact block energies), so it can measure a programme of
// any length without allocating. prepare() allocates; process(), reset() and
// reading() are real-time safe, which lets the audio engine meter its output
// live with the same code that measures offline renders.
//
// Channels are weighted 1.0 (mono and stereo programmes). A mono programme
// therefore reads 3 dB below the same signal on both channels of a stereo one,
// as BS.1770 specifies.

#include <daw/dsp/Biquad.h>
#include <daw/dsp/TruePeak.h>

#include <array>
#include <cstdint>
#include <vector>

namespace daw {

/// Reported for "no signal" (JSON cannot hold -inf).
inline constexpr double kLoudnessFloor = -144.0;

struct LoudnessReading {
    double momentaryLufs = kLoudnessFloor;
    double shortTermLufs = kLoudnessFloor;
    double integratedLufs = kLoudnessFloor;
    double loudnessRangeLu = 0.0;
    double maxMomentaryLufs = kLoudnessFloor;
    double maxShortTermLufs = kLoudnessFloor;
    double truePeakDbtp = kLoudnessFloor; // maximum since the last reset
    double samplePeakDbfs = kLoudnessFloor;
    double seconds = 0.0;                 // audio measured since the last reset
};

/// K-weighting pre-filter (stage 1: high shelf, stage 2: high pass) for a
/// sample rate. At 48 kHz these equal the coefficients printed in BS.1770-4.
std::array<BiquadCoefficients, 2> kWeightingFilters(double sampleRate);

class LoudnessMeter {
public:
    /// `truePeak` false skips the oversampling (the most expensive part); the
    /// reported true peak is then the sample peak.
    void prepare(double sampleRate, int numChannels, bool truePeak = true);
    void reset() noexcept;

    /// Measures a block. Returns true when at least one 100 ms step finished,
    /// i.e. the momentary, short-term, integrated and range values changed.
    bool process(const float* const* channels, int numChannels, int numFrames) noexcept;

    const LoudnessReading& reading() const noexcept { return reading_; }
    double sampleRate() const noexcept { return sampleRate_; }

private:
    struct Histogram {
        std::vector<double> energy; // sum of block mean-square values per bin
        std::vector<std::uint32_t> count;
        double totalEnergy = 0.0;
        std::uint64_t totalCount = 0;

        void prepare();
        void clear() noexcept;
        void add(double meanSquare, double lufs) noexcept;
    };

    void finishStep() noexcept;
    double integrated() const noexcept;
    double loudnessRange() const noexcept;

    static constexpr int kMomentarySteps = 4;   // 4 x 100 ms
    static constexpr int kShortTermSteps = 30;  // 30 x 100 ms

    double sampleRate_ = 48000.0;
    int channels_ = 0;
    int stepFrames_ = 4800;
    bool measureTruePeak_ = true;
    std::vector<Biquad> filters_; // 2 per channel
    TruePeakDetector truePeak_;

    // Current 100 ms step and the ring of the last 30 step energies (sums of
    // weighted squared K-filtered samples).
    double stepEnergy_ = 0.0;
    int stepPosition_ = 0;
    std::array<double, kShortTermSteps> steps_{};
    int stepIndex_ = 0;
    std::uint64_t stepsDone_ = 0;
    std::int64_t frames_ = 0;
    float samplePeak_ = 0.0f;
    float truePeakValue_ = 0.0f;

    Histogram blocks_;     // 400 ms gating blocks (integrated loudness)
    Histogram shortTerm_;  // 3 s windows (loudness range)
    LoudnessReading reading_;
};

} // namespace daw
