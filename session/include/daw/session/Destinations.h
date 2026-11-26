// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Export presets per destination (milestone 5).
//
// One mix, mastered for where it is going: each destination sets the
// loudness target, the true-peak ceiling, the sample format and the sample
// rate. An export for a destination runs Match Loudness for its target on a
// copy of the project (the project itself is not changed), at the rate the
// file will have, then renders the master with the result. The tracks, the
// master EQ and the multiband settings stay as mixed.
//
// - Streaming: services turn louder songs down to -14 LUFS (Spotify,
//   YouTube, Tidal, Amazon; Apple Music -16), so one master at -14 LUFS
//   under -1 dBTP plays at full level everywhere and leaves the codecs
//   headroom. 24-bit at the project rate, what distributors prefer.
// - Bandcamp: no loudness normalisation, and buyers compare with other
//   releases; -10 LUFS. Bandcamp also sells and streams lossy copies, so
//   -1 dBTP. 24-bit at the project rate (it serves the lossless download as
//   uploaded).
// - Club: -8 LUFS, the level of club and techno masters; DJ players handle
//   16/24-bit WAV at 44.1 or 48 kHz, so higher project rates come down to
//   48 kHz (44.1 kHz for multiples of 44.1).
// - CD: Red Book is 16-bit at 44.1 kHz (dithered); -10 LUFS, and -0.3 dBTP
//   since no lossy codec follows on the disc.

#include <daw/core/CancellationToken.h>
#include <daw/core/Result.h>
#include <daw/media/WavWriter.h>
#include <daw/project/ProjectModel.h>
#include <daw/session/MasteringTools.h>
#include <daw/session/MediaPool.h>
#include <daw/session/Renderer.h>

#include <filesystem>
#include <span>

namespace daw {

struct Destination {
    const char* key;          // short name: file-name suffix, logs
    const char* name;         // shown in the app
    double lufs;              // loudness target
    double ceilingDbtp;       // true-peak ceiling
    WavSampleFormat format;
    double sampleRate;        // fixed rate, or 0 for the project rate
    double maxSampleRate;     // 0: no limit
};

inline constexpr Destination kDestinations[] = {
    {"Streaming", "Streaming (Spotify, Apple Music, YouTube, Tidal)", -14.0, -1.0, WavSampleFormat::Pcm24, 0.0, 0.0},
    {"Bandcamp", "Bandcamp and downloads", -10.0, -1.0, WavSampleFormat::Pcm24, 0.0, 0.0},
    {"Club", "Club and DJs", -8.0, -1.0, WavSampleFormat::Pcm24, 0.0, 48000.0},
    {"CD", "CD (Red Book)", -10.0, -0.3, WavSampleFormat::Pcm16, 44100.0, 0.0},
};

inline std::span<const Destination> destinations() noexcept
{
    return kDestinations;
}

/// The sample rate a destination's file gets for a project at `projectRate`.
double destinationSampleRate(const Destination& destination, double projectRate) noexcept;

/// "Song.wav" -> "Song (CD).wav": the file for one destination of several.
std::filesystem::path destinationPath(const std::filesystem::path& path, const Destination& destination);

/// The project as it is mastered for a destination: the master fader and the
/// master bus a match led to (the limiter on, the destination's target and
/// ceiling, the clipper if needed).
ProjectModel masteredFor(const ProjectModel& model, const Destination& destination, const LoudnessMatch& match);

struct DestinationExportReport {
    LoudnessMatch match;  // the match for the destination's target
    ExportReport file;    // what was written (render.loudness measures the file's mix)
};

/// Matches the destination's loudness and exports the result to
/// `request.path` (its format is the destination's; `request.dither`,
/// metadata and software are kept). The project is not changed.
Result<DestinationExportReport> exportForDestination(const ProjectModel& model, const MediaPool& pool,
                                                     const Destination& destination, const ExportRequest& request,
                                                     const RenderOptions& options = {}, const CancellationToken& cancel = {});

} // namespace daw
