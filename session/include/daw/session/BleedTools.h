// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Cross-stem bleed suppression for a project's stems (dsp/BleedSuppressor).
//
// Every track that plays recorded audio takes part as a reference: each
// stem's share of every time-frequency bin is judged against all the others,
// on the project timeline, from the tracks' *original* recordings. The
// tracks asked for (targets) get a processed copy of each original they play,
// rendered into Media/Processed; their clips then play the copies, exactly as
// voice denoise does (VoiceTools), so it is non-destructive and undoable and
// Original switches back. Applying it replaces any other rendered process on
// the track, and re-applying renders from the originals again.
//
// Only the media frames the track's clips play are processed; frames outside
// them keep the original. A media played by several clips is processed for
// the last of them.

#include <daw/core/AudioAsset.h>
#include <daw/core/CancellationToken.h>
#include <daw/core/Result.h>
#include <daw/project/ProjectModel.h>
#include <daw/session/MediaPool.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <vector>

namespace daw {

inline constexpr const char* kBleedSuppressProcess = "bleed-suppress";
/// A track whose louder half drops by more than this probably mostly
/// duplicated other stems (or the strength is too high): the app says so.
inline constexpr double kBleedLossWarningDb = -3.0;

/// The strengths offered. The processed copy records the reduction limit as
/// its amount, which identifies the strength.
struct BleedStrength {
    const char* name;
    float exponent;    // mask sharpening power
    float reductionDb; // the most any bin is turned down
};

inline constexpr BleedStrength kBleedStrengths[3] = {
    {"Light", 1.5f, 12.0f},
    {"Medium", 2.0f, 18.0f},
    {"Strong", 3.0f, 24.0f},
};

/// The strength whose reduction limit is nearest `reductionDb`.
const BleedStrength& bleedStrengthFor(double reductionDb);

/// Tracks that take part: those with clips playing recorded audio (MIDI is
/// synthesised and carries no bleed), in project order.
std::vector<TrackId> bleedTracks(const ProjectModel& model);

struct BleedTrackOutcome {
    TrackId track;
    std::vector<MediaRef> media;     // processed copies, one per original the track plays
    std::vector<LoadedMedia> loaded; // ready for the media pool
    double musicChangeDb = 0.0;      // level change over the louder half of the 20 ms blocks
    double bleedChangeDb = 0.0;      // over the quietest tenth (where bleed is heard)
};

/// Decoded original media, at its native rate (the media pool's `original`).
using OriginalAudioLookup = std::function<AudioAssetPtr(const MediaId&)>;

/// Renders bleed-suppressed copies for `targets` (each must be one of
/// bleedTracks(); at least two tracks must take part). Copies are written at
/// the project's sample rate and loaded for `engineRate`. `progress`
/// receives 0..1.
Result<std::vector<BleedTrackOutcome>> suppressBleed(const std::filesystem::path& projectRoot, const ProjectModel& model,
                                                     const OriginalAudioLookup& originals,
                                                     const std::vector<TrackId>& targets, const BleedStrength& strength,
                                                     double engineRate, const CancellationToken& cancel = {},
                                                     const std::function<void(double)>& progress = {});

/// The bleed suppression the track's clips play, when every clip plays a
/// bleed-suppressed copy.
std::optional<MediaDerivation> trackBleedSuppression(const ProjectModel& model, const TrackId& track);

} // namespace daw
