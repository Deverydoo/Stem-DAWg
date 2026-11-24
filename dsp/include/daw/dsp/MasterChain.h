// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// The master bus, after the tracks are summed: tonal EQ -> true-peak
// limiter. The master fader comes before it (the snapshot folds it into
// every track's gains), so it is the drive into the limiter. See
// docs/milestone-5/M5-Mastering.md.
//
// The EQ has five fixed bands (kMasterEqBands): broad moves for tonal
// balance, not surgery. Only their gains are set, which keeps suggested
// moves (TonalBalance) and hand edits on the same five controls.
//
// TruePeakLimiter keeps the true peak (ITU-R BS.1770-4, 4x oversampled) of
// its output at or under the ceiling, so neither a DAC nor a lossy encoder
// clips it. Per input sample it computes the gain that brings the true peak
// of that stretch of signal to the ceiling, then
//
//   1. takes the minimum of that gain over the lookahead window (W + 1 samples),
//   2. lets it rise again with the release time (it falls at once),
//   3. averages it over the window (W samples),
//
// and applies it to the audio delayed by the window. At every peak the
// averaged gain is at most what that peak needs (the whole averaging window
// then lies inside the minimum's window), and the gain moves smoothly: a
// linear fade over W samples into each peak, the release out of it. The
// minimum's window is one sample longer because a true peak lies between two
// original samples and both must be turned down.
//
// The delay (latency()) is the same whether the limiter is on or off, so
// switching it does not shift the audio. Real-time safe after prepare().

#include <daw/dsp/Biquad.h>
#include <daw/dsp/TruePeak.h>

#include <array>
#include <cstdint>
#include <vector>

namespace daw {

class TruePeakLimiter {
public:
    static constexpr double kLookaheadMs = 5.0;
    static constexpr double kMinCeilingDb = -12.0;
    static constexpr double kMaxCeilingDb = 0.0;
    static constexpr double kMinReleaseMs = 10.0;
    static constexpr double kMaxReleaseMs = 1000.0;

    /// Allocates the delay lines (not real-time safe) and resets.
    void prepare(double sampleRate);
    int latency() const noexcept { return latency_; }

    void setParams(bool enabled, float ceilingDb, float releaseMs) noexcept;
    void reset() noexcept;
    /// Processes a stereo block in place (left and right may be the same
    /// buffer for mono). The output is delayed by latency().
    void process(float* left, float* right, int numFrames) noexcept;

    /// Lowest gain applied since the last call, in dB (<= 0), for a meter.
    float takeMinGainDb() noexcept;

private:
    float delayed(int channel, int age) const noexcept; // input sample `age` samples ago

    TruePeakDetector detector_;
    int window_ = 1;  // W
    int interp_ = 0;  // interpolator group delay
    int latency_ = 0; // W - 1 + interp_
    bool enabled_ = false;
    float ceiling_ = 1.0f;
    float releaseCoeff_ = 0.0f;
    double sampleRate_ = 48000.0;

    std::array<std::vector<float>, 2> audio_; // input ring, latency_ + 2 samples
    int audioPos_ = 0;                        // where the newest sample is
    std::vector<float> minValue_;             // monotonic deque (ring) for the sliding minimum
    std::vector<std::int64_t> minIndex_;
    int minHead_ = 0, minSize_ = 0;
    std::int64_t index_ = 0;
    float envelope_ = 1.0f;
    std::vector<float> average_; // last W envelope values
    int averagePos_ = 0;
    double averageSum_ = 0.0;
    float minGain_ = 1.0f;
};

enum class EqShape { LowShelf, Peak, HighShelf };

struct MasterEqBand {
    EqShape shape;
    double hz;
    double q;
};
inline constexpr std::size_t kMasterEqBandCount = 5;
inline constexpr std::array<MasterEqBand, kMasterEqBandCount> kMasterEqBands{{
    {EqShape::LowShelf, 100.0, 0.7071}, // low end
    {EqShape::Peak, 250.0, 0.8},        // low mids (mud)
    {EqShape::Peak, 1000.0, 0.8},       // mids
    {EqShape::Peak, 3000.0, 0.8},       // presence
    {EqShape::HighShelf, 8000.0, 0.7071}, // air
}};
inline constexpr double kMasterEqMaxDb = 12.0;

/// Coefficients of one master EQ band at a gain.
BiquadCoefficients masterEqCoefficients(std::size_t band, double gainDb, double sampleRate) noexcept;

struct MasterParams {
    bool limiter = false;
    float ceilingDbtp = -1.0f;
    float releaseMs = 100.0f;
    bool eq = false;
    std::array<float, kMasterEqBandCount> eqGainDb{};

    friend bool operator==(const MasterParams&, const MasterParams&) = default;
};

class MasterChain {
public:
    void prepare(double sampleRate);
    int latency() const noexcept { return limiter_.latency(); }
    void setParams(const MasterParams& params) noexcept;
    const MasterParams& params() const noexcept { return params_; }
    /// Clears the delay and the limiter.
    void reset() noexcept;
    void process(float* left, float* right, int numFrames) noexcept;
    float takeMinGainDb() noexcept { return limiter_.takeMinGainDb(); }

private:
    MasterParams params_;
    double sampleRate_ = 48000.0;
    std::array<std::array<Biquad, kMasterEqBandCount>, 2> eq_{};
    std::array<bool, kMasterEqBandCount> eqOn_{}; // bands with a gain
    TruePeakLimiter limiter_;
};

} // namespace daw
