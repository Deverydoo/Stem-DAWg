// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Oversampled soft clipper for the master bus, before the limiter
// (milestone 5).
//
// Why: a limiter's gain moves over milliseconds, so it cannot touch the
// peak-to-body difference within a few milliseconds (kick transients, the
// top of a bass waveform). On a Suno stem mix that difference held the
// master near -9 LUFS however hard the limiter worked; rounding off the top
// decibel of the loudest few percent of samples gains 1.5 LU or more and
// keeps the song's dynamics (see docs/milestone-5/M5-Mastering.md).
//
// How: 4x oversampling (193-tap linear-phase windowed-sinc filters, about
// 65 dB of image rejection), so the clipping's harmonics do not fold back
// into the audible band; a soft knee from kKneeDb under the clip level,
// shaped by tanh, that never passes the clip level. Delay: latency() frames,
// the same when bypassed (a plain delay), so switching does not shift the
// audio; switching glides over kGlideMs.
//
// Real-time safe after prepare().

#include <array>
#include <cstdint>
#include <vector>

namespace daw {

class SoftClipper {
public:
    static constexpr int kFactor = 4;
    static constexpr int kTaps = 193; // odd: an integer delay
    static constexpr double kKneeDb = 1.0;
    static constexpr double kGlideMs = 20.0;

    void prepare(double sampleRate);
    static constexpr int latency() noexcept { return (kTaps - 1) / kFactor; } // up + down, in input frames

    /// `clipDbfs`: the level it never passes (in the oversampled signal).
    void setParams(bool enabled, float clipDbfs) noexcept;
    void reset() noexcept;
    void process(float* left, float* right, int numFrames) noexcept;

    /// Share of oversampled samples in the knee or above since the last call (0..1).
    double takeClippedFraction() noexcept;

    /// The curve itself, for a magnitude: identity below the knee, then a
    /// tanh approach to `clip`.
    static float shape(float x, float clip) noexcept;

    /// The level (dBFS) that the given share of the oversampled samples of a
    /// stereo signal exceed: with the knee there, the clipper would touch
    /// that share. Upsamples exactly as process() does. Not real-time safe.
    static double levelExceededBy(const float* left, const float* right, std::int64_t frames, double share);

private:
    bool enabled_ = false;
    float clip_ = 1.0f;
    float mix_ = 0.0f;
    float glide_ = 0.0f;
    std::vector<float> taps_;                        // prototype, sums to 1
    std::array<std::vector<float>, kFactor> phases_; // upsampler branches (x kFactor gain)
    std::array<std::vector<float>, 2> input_;        // per channel, mirrored ring of input samples
    std::array<std::vector<float>, 2> high_;         // per channel, mirrored ring of clipped oversampled samples
    std::array<std::vector<float>, 2> delay_;        // per channel, the bypass path
    int inputPos_ = 0, highPos_ = 0, delayPos_ = 0;
    int branchTaps_ = 0;
    std::uint64_t touched_ = 0, counted_ = 0;
};

} // namespace daw
