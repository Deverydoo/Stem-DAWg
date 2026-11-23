// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Measurements that set up the noise stages of a track's clean-up chain:
//
//  * learnNoiseProfile: the noise spectrum of the track's quietest moments,
//    which the spectral denoiser then removes everywhere.
//  * detectHum: whether those quiet moments carry mains hum (50 or 60 Hz and
//    harmonics) and how many harmonics stand out.
//
// Both measure the track as it enters that stage of its chain: after DC
// removal and the high-pass (and, for the profile, after hum removal), before
// the fader. "Quiet" means the quietest kQuietFraction of the frames that are
// not digital silence: in a home recording the room between phrases, in an
// AI stem the bed between notes.
//
// Given a region (the user selected a stretch of pure noise), both measure
// every frame inside it that is not digital silence instead.
//
// Both stream the track twice (level pass, then spectrum pass), so memory
// stays small for long songs. Run them on a worker thread.

#include <daw/core/CancellationToken.h>
#include <daw/core/Result.h>
#include <daw/project/ProjectModel.h>
#include <daw/session/MediaPool.h>

#include <cstdint>
#include <optional>

namespace daw {

/// A stretch of the timeline in project frames, [start, end).
struct FrameRange {
    std::int64_t start = 0;
    std::int64_t end = 0;
    std::int64_t length() const noexcept { return end - start; }
    bool operator==(const FrameRange&) const = default;
};

struct NoiseLearnOptions {
    double quietFraction = 0.10; // share of the non-silent frames treated as noise
    double minSeconds = 0.25;    // refuse to learn from less noise than this
    /// Learn from this region only, all of it (quietFraction is then unused).
    std::optional<FrameRange> region;
};

/// Learns the noise profile of a track (fftSize = SpectralDenoiser::kFftSize at
/// the project rate). Fails with InvalidArgument when the track has too little
/// non-silent audio.
Result<NoiseProfile> learnNoiseProfile(const ProjectModel& model, const MediaPool& pool, const TrackId& track,
                                       const NoiseLearnOptions& options = {}, const CancellationToken& cancel = {});

struct HumDetection {
    double frequencyHz = 0.0; // 50 or 60
    int harmonics = 0;        // harmonics standing out by at least kHumHarmonicDb
    double prominenceDb = 0.0; // strongest of the first three harmonics above its surroundings
};

inline constexpr double kHumDetectDb = 12.0;   // a harmonic this far above its neighbourhood is hum
inline constexpr double kHumHarmonicDb = 6.0;  // further harmonics count from this prominence

/// Looks for mains hum in the quiet parts of a track, or in all of `region`
/// when one is given. Empty when there is none.
Result<std::optional<HumDetection>> detectHum(const ProjectModel& model, const MediaPool& pool, const TrackId& track,
                                              const CancellationToken& cancel = {},
                                              const std::optional<FrameRange>& region = std::nullopt);

} // namespace daw
