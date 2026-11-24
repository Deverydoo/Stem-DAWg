// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Tonal balance (milestone 5): how a song's energy spreads over frequency,
// compared with a target built from reference tracks, and the master EQ
// moves that bring it closer.
//
// Profile: the long-term spectrum in third-octave bands from 31.5 Hz to
// 16 kHz, each band's share of the total in dB. Only the shape counts, not
// the level. Frames more than 20 dB below the song's loud part (silence,
// fades, intros) are left out, so a long quiet intro does not tilt it.
//
// Target: the average of several reference profiles per band, and how much
// they disagree (standard deviation, at least kMinSpreadDb). Genre targets
// are built from reference tracks the user owns (industrial, EBM, techno...):
// no curves are invented here.
//
// Comparison: the difference per band, lined up on its weighted mean over
// 100 Hz-8 kHz (a heavier sub-bass then does not make every other band look
// thin); bands further off than their spread are flagged. The suggested EQ
// is a weighted least-squares fit of the five master EQ gains (bands where
// the references agree count more) through the same lining-up, so the
// predicted result is what measuring the mix again reports. Gains are
// limited to kMaxSuggestedDb and rounded to 0.5 dB.

#include <daw/dsp/Fft.h>
#include <daw/dsp/MasterChain.h>

#include <array>
#include <complex>
#include <string>
#include <vector>

namespace daw {

inline constexpr std::size_t kTonalBands = 28;
inline constexpr std::array<double, kTonalBands> kTonalBandHz{
    31.5, 40.0,   50.0,   63.0,   80.0,   100.0,  125.0,  160.0,  200.0,   250.0,   315.0,   400.0,   500.0,   630.0,
    800.0, 1000.0, 1250.0, 1600.0, 2000.0, 2500.0, 3150.0, 4000.0, 5000.0, 6300.0, 8000.0, 10000.0, 12500.0, 16000.0};

struct TonalProfile {
    std::array<double, kTonalBands> levelDb{}; // each band's share of the total, dB
    double seconds = 0.0;                      // audio that counted (after gating)
    bool valid() const noexcept { return seconds > 0.0; }
};

/// Accumulates a profile from stereo audio in blocks of any size.
class TonalAnalyzer {
public:
    static constexpr int kFftSize = 8192;
    static constexpr double kGateDb = 20.0; // frames this far below the loud part do not count

    explicit TonalAnalyzer(double sampleRate);
    void process(const float* left, const float* right, int numFrames);
    TonalProfile result() const;

private:
    void frame();

    double sampleRate_;
    Fft fft_;
    std::vector<float> window_;
    std::vector<float> input_[2];
    std::vector<std::complex<float>> spectrum_;
    std::vector<int> bandOfBin_; // -1 outside the bands
    int fill_ = 0;
    std::vector<std::array<float, kTonalBands>> frames_; // band energies per frame
};

struct TonalTarget {
    static constexpr double kMinSpreadDb = 1.5;
    static constexpr double kSingleSpreadDb = 3.0; // with one reference nothing says how much songs vary

    std::string name;
    std::vector<std::string> references; // file names, for the user's information
    std::array<double, kTonalBands> meanDb{};
    std::array<double, kTonalBands> spreadDb{};
    bool valid() const noexcept { return !references.empty(); }
    friend bool operator==(const TonalTarget&, const TonalTarget&) = default;
};

TonalTarget makeTonalTarget(std::string name, std::vector<std::string> references, const std::vector<TonalProfile>& profiles);

struct TonalComparison {
    static constexpr double kMaxSuggestedDb = 6.0;

    std::array<double, kTonalBands> deviationDb{}; // mix minus target, lined up on the midrange
    std::array<bool, kTonalBands> outside{};       // further off than the target's spread
    std::array<float, kMasterEqBandCount> eqGainDb{}; // suggested master EQ
    double rmsBeforeDb = 0.0;                       // weighted RMS of the deviation
    double rmsAfterDb = 0.0;                        // what the suggested EQ leaves (predicted)
};

/// `currentEq` is the master EQ the profile was measured with (its effect is
/// already in the profile); the suggestion is the new setting, so applying it
/// replaces the current gains.
TonalComparison compareTonal(const TonalProfile& mix, const TonalTarget& target,
                             const std::array<float, kMasterEqBandCount>& currentEq = {});

} // namespace daw
