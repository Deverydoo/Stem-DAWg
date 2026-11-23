// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Harshness and shimmer control: turns down narrow peaks that persist above
// the surrounding spectrum, in the 2-6 kHz ("harshness") and 8-17.5 kHz
// ("shimmer") ranges.
//
// AI-generated music has two kinds of such peaks (measured on Suno exports,
// see docs/milestone-4/M4-Artifact-Repair.md):
//  * a high partial that rings through many notes, 6-12 dB above its
//    neighbours: piercing at 2-4 kHz, where the ear is most sensitive;
//  * steady tones at fixed frequencies (8.0, 16.0, 16.6, 16.8, 16.9 kHz),
//    the same in unrelated songs: generator artifacts, heard as a whine.
//
// Works on the spectra of a short-time Fourier transform (it computes gains;
// the caller owns the transform, see SpectralDenoiser). Per frame and bin:
//
//   excess = power / max(mean power of the bins to the left, ... to the right)
//
// over 1/6 octave on each side, leaving out the bins next to the centre. A
// peak must stand above both sides, so slopes and the low-pass cliff of a
// generator's output do not count. The excess, in dB, is smoothed over time
// (rising in about 1 s, falling in about 2 s): a partial of a note that comes
// and goes averages out, a resonance that outlasts the notes does not. The
// cut is the smoothed excess above kThresholdDb, limited per range to the set
// depth, so a peak is brought down towards its surroundings, never below.
// Each bin's cut also covers the bins on either side (kSpreadBins): a partial
// between two bins, or one that drifts, spreads over its neighbours.
//
// The detector's memory survives reset() (the engine resets on every seek,
// and the resonances it follows last for the whole song); prepare() clears
// it. A fresh detector averages over a few frames at first, so it settles
// within a fraction of a second.
//
// Real-time safe after prepare().

#include <vector>

namespace daw {

struct ResonanceParams {
    float harshDb = 0.0f;   // maximum cut in 2-6 kHz, 0 .. kMaxDb
    float shimmerDb = 0.0f; // maximum cut in 8-17.5 kHz, 0 .. kMaxDb

    friend bool operator==(const ResonanceParams&, const ResonanceParams&) = default;
};

class ResonanceSuppressor {
public:
    static constexpr double kThresholdDb = 2.0;      // excess left alone
    static constexpr double kAttackMs = 1000.0;      // excess smoothing, rising
    static constexpr double kReleaseMs = 2000.0;     // excess smoothing, falling
    static constexpr double kNeighbourOctaves = 1.0 / 6.0;
    static constexpr double kGapHz = 47.0;           // left out on each side of the centre (the peak's own skirt)
    static constexpr double kHarshLowHz = 2000.0;
    static constexpr double kHarshHighHz = 6000.0;
    static constexpr double kShimmerLowHz = 8000.0;
    static constexpr double kShimmerHighHz = 17500.0;
    static constexpr double kLowTaperOctaves = 0.5;  // the depth fades in over this below a range
    static constexpr double kHarshTaperOctaves = 0.5;
    static constexpr double kShimmerTaperOctaves = 0.25;
    static constexpr double kMaxDb = 12.0;
    static constexpr int kWarmupFrames = 8;          // a fresh detector starts with this much history
    static constexpr int kSpreadBins = 1;            // a cut also applies this many bins either side

    /// Allocates per-bin state for spectra of `fftSize` points taken every
    /// `hop` samples (not real-time safe). Clears the detector.
    void prepare(double sampleRate, int fftSize, int hop);
    int bins() const noexcept { return static_cast<int>(excessDb_.size()); }

    void setParams(const ResonanceParams& params) noexcept;
    const ResonanceParams& params() const noexcept { return params_; }
    /// True when either range has a depth: process() then changes the sound.
    bool active() const noexcept { return params_.harshDb > 0.0f || params_.shimmerDb > 0.0f; }

    /// Clears the detector's memory.
    void reset() noexcept;

    /// Measures one frame (bins() powers, any consistent scale) and updates
    /// the detector, without computing gains. For analysis.
    void analyse(const float* power) noexcept;
    /// Measures one frame and multiplies `gain` (bins() amplitude gains) by
    /// the cut of every bin.
    void process(const float* power, float* gain) noexcept;

    /// The cut the detector asks for at a bin for the current frame and
    /// depths, in dB (>= 0), before it is spread to the neighbours.
    float cutDb(int bin) const noexcept;
    /// Smoothed excess at a bin, in dB.
    float excessDb(int bin) const noexcept { return excessDb_[static_cast<std::size_t>(bin)]; }
    /// How much of each range's depth applies at a bin (0 .. 1).
    float harshWeight(int bin) const noexcept { return harshWeight_[static_cast<std::size_t>(bin)]; }
    float shimmerWeight(int bin) const noexcept { return shimmerWeight_[static_cast<std::size_t>(bin)]; }
    double binHz() const noexcept { return binHz_; }

private:
    double binHz_ = 0.0;
    float attackCoeff_ = 0.0f;
    float releaseCoeff_ = 0.0f;
    int first_ = 0; // bins [first_, last_) can be cut
    int last_ = 0;
    int gap_ = 1;
    int frames_ = 0; // since the last reset, for the warm-up
    ResonanceParams params_;

    std::vector<int> low_;  // per bin: left neighbourhood [low_, k - gap_)
    std::vector<int> high_; // per bin: right neighbourhood (k + gap_, high_]
    std::vector<float> harshWeight_;
    std::vector<float> shimmerWeight_;
    std::vector<float> maxCutDb_;  // per bin, from the depths and weights
    std::vector<float> excessDb_;  // smoothed, per bin
    std::vector<float> cut_;       // scratch: cutDb() per bin
    std::vector<double> cumulative_; // prefix sums of the frame's power
};

} // namespace daw
