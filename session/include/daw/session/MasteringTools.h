// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Mastering helpers on the whole mix (milestone 5).
//
// matchLoudness finds the master fader setting that brings the mastered mix
// to a target integrated loudness (EBU R 128) through the true-peak limiter.
// Turning the fader up by N dB raises the loudness by less than N dB once
// the limiter works, so the answer is found by trying: the mix is rendered
// once (master fader at 0 dB, no master bus) and kept in memory, then only
// the limiter and a loudness meter run over it at trial settings, refined
// by the secant method until the result is within 0.05 LU of the target.
//
// The limiter is never asked for more than kMaxLimiterReductionDb: beyond
// that a peak limiter audibly crushes the mix (on a Suno stem mix, -8 LUFS
// would take 36 dB). A target past that point is reported as not reached,
// with the loudest master within the limit.

#include <daw/core/CancellationToken.h>
#include <daw/core/Result.h>
#include <daw/project/ProjectModel.h>
#include <daw/session/MediaPool.h>

#include <functional>

namespace daw {

/// Loudness targets offered in the app. Streaming services turn louder songs
/// down to their reference (Spotify, YouTube, Tidal: -14 LUFS; Apple Music:
/// -16 LUFS); club and techno masters are usually much louder.
struct LoudnessPreset {
    const char* name;
    double lufs;
    double ceilingDbtp;
};
inline constexpr LoudnessPreset kLoudnessPresets[] = {
    {"Streaming (-14 LUFS)", -14.0, -1.0},
    {"Apple Music (-16 LUFS)", -16.0, -1.0},
    {"CD / Bandcamp (-10 LUFS)", -10.0, -1.0},
    {"Club (-8 LUFS)", -8.0, -1.0},
};

inline constexpr double kMaxLimiterReductionDb = 12.0;

struct LoudnessMatch {
    double mixLufs = 0.0;          // the mix with the master fader at 0 dB, before the master bus
    double mixTruePeakDbtp = 0.0;
    double masterGainDb = 0.0;     // the master fader setting that reaches the target
    double resultLufs = 0.0;       // what the mastered mix then measures
    double resultTruePeakDbtp = 0.0;
    double resultLoudnessRangeLu = 0.0;
    double maxReductionDb = 0.0;   // the limiter's deepest gain reduction (<= 0)
    int trials = 0;                // passes of the limiter over the mix
    bool reached = false;          // within 0.05 LU of the target
    bool limited = false;          // not reached because it would take more than kMaxLimiterReductionDb
    double seconds = 0.0;
};

struct LoudnessMatchOptions {
    std::function<void(double)> progress; // 0..1, from the calling thread
};

/// Finds the master fader setting for `targetLufs` with the limiter of
/// `master` switched on (its ceiling and release). The project is not
/// changed. Every media item used by a clip must be loaded.
Result<LoudnessMatch> matchLoudness(const ProjectModel& model, const MediaPool& pool, double targetLufs,
                                    const MasterSettings& master, const LoudnessMatchOptions& options = {},
                                    const CancellationToken& cancel = {});

} // namespace daw
