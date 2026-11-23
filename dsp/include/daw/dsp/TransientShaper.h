// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Transient shaper: sharpens or softens the rise of each hit (attack) and
// turns the decay after it up or down (sustain), without a threshold, so it
// follows a part at any level.
//
// AI-generated drums take about 3 ms to rise to their peak above 2 kHz (the
// stem report measures it), where a sampled hit takes about 1 ms: they sound
// soft and smeared. Turning the whole hit up does not fix that (the rise keeps
// its shape, and the peak clips). The attack control reshapes the rise
// instead, in the band above 1 kHz where the smearing is heard (below it,
// fast gain changes would only bend the waveform of a kick or a bass note),
// using a lookahead:
//
//   F  the band's RMS level over 0.5 ms, read for the middle of its window
//      (the lookahead allows it): a lagging level would itself look like a
//      rise. Not held: in a busy drum part a held level stays up from the
//      last hi-hat and hides the next hit's rise;
//   P  the highest RMS level over 1.5 ms within the lookahead: the level the
//      sound is heading for. The longer window keeps noisy hits (snares,
//      hats) from promising a peak that is only a fluctuation;
//   A  F smoothed over 3 ms. F > A means the level is climbing: a hit. (A
//      longer average would be held up by the hi-hats and the last hit, and
//      the first half of a rise would stay under it.)
//
// While a hit rises, the gain lifts the sound towards P, by at most attackDb:
// gain = min(attackDb, P/F in dB less a 0.5 dB margin) times the rise weight.
// The weight is close to on/off: steady sound is never lifted anyway, since a
// level that fluctuates above A is also at or above the level ahead.
// The early part of a slow rise comes up and the top (where F reaches P) is
// left alone, so a hit that already rises instantly is not touched and peaks
// do not grow (the lift is capped at P/F after smoothing). A negative attack turns the start of
// each hit down by attackDb instead, softening hits that click: weighted by F
// against its 20 ms average, so the whole first stretch of the hit, not just
// its rise, comes down.
//
// The band is split complementarily (high = x - lowpass(x)), so the sound is
// rebuilt exactly when the gains are 1. Sustain works on the whole signal:
//
//   R  RMS level of both channels over 10 ms, which follows a decay closely;
//   S  R held at its peaks and released slowly (600 ms). S > R means the
//      sound is dying away: the sustain region (not while it still rises).
//
// Each ratio passes a dead zone (level fluctuation, the ripple of a low bass
// note's envelope) and is scaled to a 0..1 weight. The sustain is held off
// while the whole signal rises above its 20 ms average. The attack and
// sustain gains are smoothed over 0.5 ms.
//
// process() takes the newest sample and returns the gains for the sample
// `lookahead` frames older: the caller delays the audio by that much and
// passes each delayed sample through apply().
//
// Real-time safe: prepare() allocates; nothing else does.

#include <daw/dsp/Biquad.h>

#include <array>
#include <cstdint>
#include <vector>

namespace daw {

struct TransientParams {
    float attackDb = 0.0f;  // -12 .. +12
    float sustainDb = 0.0f; // -12 .. +12

    friend bool operator==(const TransientParams&, const TransientParams&) = default;
};

class TransientShaper {
public:
    static constexpr double kLevelMs = 0.5;         // RMS window of the high band's level (F) ...
    static constexpr double kPeakLevelMs = 1.5;     // ... and of the level it heads for (P)
    static constexpr double kFastReleaseMs = 60.0;
    static constexpr double kHeadroomDeadZoneDb = 0.5; // a level this close to the coming peak is not lifted
    static constexpr double kRiseLagMs = 3.0;       // A
    static constexpr double kGateLagMs = 20.0;      // the whole signal's average, for the sustain gate
    static constexpr double kRmsMs = 10.0;
    static constexpr double kSlowReleaseMs = 600.0;
    static constexpr double kSmoothingMs = 0.5;
    static constexpr double kRiseLeadMs = 0.5;      // the rise weight is read this far ahead of the audio ...
    static constexpr double kSustainLeadMs = 2.0;   // ... and the sustain weight this far, so it lets go before a hit
    static constexpr double kRiseDeadZoneDb = 1.5;
    static constexpr double kRiseRangeDb = 1.5;     // F above A by dead zone + this: full rise weight
    static constexpr double kSoftenLagMs = 20.0;    // softening: F against this average ...
    static constexpr double kSoftenDeadZoneDb = 2.0;
    static constexpr double kSoftenRangeDb = 8.0;   // ... full weight this far above the dead zone
    static constexpr double kGateDeadZoneDb = 2.0;  // the whole signal this far above its average: no sustain
    static constexpr double kSustainDeadZoneDb = 3.0;
    static constexpr double kSustainRangeDb = 9.0;  // R below S by dead zone + this: full sustain gain
    static constexpr float kMaxDb = 12.0f;
    static constexpr double kSplitHz = 1000.0;      // attack works above this

    struct Gains {
        float high = 1.0f; // attack: the band above kSplitHz
        float all = 1.0f;  // sustain: everything
    };

    /// Allocates the lookahead (frames, >= 1; not real-time safe) and resets.
    void prepare(double sampleRate, int lookahead);
    void setParams(const TransientParams& params) noexcept;
    const TransientParams& params() const noexcept { return params_; }
    void reset() noexcept;

    /// Measures the newest stereo sample; returns the gains for the sample
    /// `lookahead` frames older. With `enabled` false the gains return to 1
    /// smoothly while the envelopes keep following.
    Gains process(float left, float right, bool enabled) noexcept;
    /// Applies gains to a delayed sample. Call it for every sample (it keeps
    /// the band split running); with both gains at 1 the sample is untouched.
    void apply(float& left, float& right, const Gains& gains) noexcept;

    /// The gain returned last, in dB.
    double lastGainDb() const noexcept { return attackDb_ + sustainDb_; }

private:
    struct Entry {
        float fast = 0.0f;    // F of the high band
        float wide = 0.0f;    // its RMS level over kPeakLevelMs (P is the highest of these ahead)
        float rise = 0.0f;    // rise weight of the high band, 0..1 (sharpening)
        float soften = 0.0f;  // weight of a hit's first stretch, 0..1 (softening)
        float sustain = 0.0f; // sustain weight, 0..1
    };

    TransientParams params_;
    double fastRelease_ = 0.0;
    double riseLag_ = 0.0;
    double softenLag_ = 0.0;
    double gateLag_ = 0.0;
    double rms_ = 0.0;
    double slowRelease_ = 0.0;
    double smoothing_ = 0.0;

    std::array<Biquad, 2> detectLow_{}; // band split of the measured signal
    std::array<Biquad, 2> outputLow_{}; // and of the delayed signal
    struct Boxcar {
        std::vector<double> ring;
        int pos = 0;
        double sum = 0.0;
        int centre = 0; // half the window: the level at sample n describes sample n - centre
        double push(double power) noexcept;
        void clear() noexcept;
    };
    Boxcar level_;     // kLevelMs of the high band's power
    Boxcar peakLevel_; // kPeakLevelMs of it
    double fast_ = 0.0;      // F and A of the high band
    double lagged_ = 0.0;
    double laggedLong_ = 0.0; // F over kSoftenLagMs
    double fastAll_ = 0.0;   // F and A of everything (rises gate the sustain)
    double laggedAll_ = 0.0;
    double power_ = 0.0;     // R squared
    double heldPower_ = 0.0; // S squared
    double attackDb_ = 0.0;  // smoothed attack gain
    double sustainDb_ = 0.0; // smoothed sustain gain

    int lookahead_ = 1;
    int riseLead_ = 0;
    int sustainLead_ = 0;
    std::vector<Entry> ring_;            // lookahead + 1 entries; sample n lives at n % size
    std::int64_t count_ = 0;             // samples measured since reset
    std::vector<std::int64_t> maxQueue_; // sample numbers whose F may still be the window's peak (monotonic deque)
    int queueHead_ = 0;
    int queueSize_ = 0;
};

} // namespace daw
