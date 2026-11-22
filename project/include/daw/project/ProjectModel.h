// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// The persistent project model: plain value types, no JUCE, no Qt.
//
// Time base: every timeline quantity (clip start, clip source offset, clip
// length, playhead) is an integer number of frames at the PROJECT sample rate.
// Media may have a different native rate; the session layer maps project
// frames to engine frames and media frames when building playback snapshots.

#include <daw/core/Ids.h>
#include <daw/project/Metadata.h>

#include <cstdint>
#include <string>
#include <vector>

namespace daw {

enum class MediaStorage {
    Copied,     // file lives inside the project (path is project-relative)
    Referenced, // file lives elsewhere (path is absolute)
};

struct MediaRef {
    MediaId id;
    std::string name;
    std::string path; // UTF-8; relative (forward slashes) when Copied
    MediaStorage storage = MediaStorage::Copied;
    double sampleRate = 0.0;
    int numChannels = 0;
    std::int64_t numFrames = 0; // native frames
    std::uint64_t sourceKey = 0;

    friend bool operator==(const MediaRef&, const MediaRef&) = default;
};

struct Clip {
    ClipId id;
    std::string name;
    MediaId media;
    std::int64_t timelineStart = 0; // project frames
    std::int64_t sourceStart = 0;   // project frames into the media
    std::int64_t length = 0;        // project frames
    double gainDb = 0.0;

    friend bool operator==(const Clip&, const Clip&) = default;
};

struct Track {
    TrackId id;
    std::string name;
    double gainDb = 0.0;   // fader; <= kSilenceDb is silence
    double pan = 0.0;      // -1 (left) .. 0 (centre) .. +1 (right), balance law, unity at centre
    bool muted = false;
    bool soloed = false;   // solo-in-place; mute wins over solo
    std::vector<Clip> clips;

    friend bool operator==(const Track&, const Track&) = default;
};

struct TempoMap {
    double bpm = 120.0;
    int numerator = 4;
    int denominator = 4;

    friend bool operator==(const TempoMap&, const TempoMap&) = default;
};

struct AnalysisRecord {
    AnalysisId id;
    MediaId media;
    std::string analyzer;   // e.g. "daw.transients"
    int analyzerVersion = 0;
    std::string resultPath; // project-relative JSON file
    std::string createdUtc;

    friend bool operator==(const AnalysisRecord&, const AnalysisRecord&) = default;
};

struct ProjectModel {
    static constexpr int kSchemaVersion = 1;
    static constexpr const char* kFormatId = "stem-dawg-project";
    /// Format ids written by earlier builds (the project was called "Industrial DAW"); read as kFormatId.
    static constexpr const char* kLegacyFormatId = "industrial-daw-project";

    ProjectId id;
    std::string name;
    std::string createdUtc;
    std::string modifiedUtc;
    std::string appVersion;

    double sampleRate = 48000.0;
    TempoMap tempo;
    double masterGainDb = 0.0;
    std::int64_t playhead = 0; // project frames

    std::vector<MediaRef> media;
    std::vector<Track> tracks;
    std::vector<AnalysisRecord> analyses;

    // Export metadata: song-level fields plus the artist profile they combine
    // with (see Metadata.h). Both optional.
    std::string profileId;
    MetadataFields release;

    const MediaRef* findMedia(const MediaId& id) const;
    /// True when at least one track is soloed.
    bool anySoloed() const noexcept;
    /// Whether a track is heard given mute and solo-in-place (mute wins).
    bool isAudible(const Track& track) const noexcept;
    const Track* findTrack(const TrackId& id) const;
    Track* findTrack(const TrackId& id);
    /// End of the last clip in project frames.
    std::int64_t contentEnd() const;

    friend bool operator==(const ProjectModel&, const ProjectModel&) = default;
};

ProjectModel makeEmptyProject(std::string name);

/// Fader floor: gains at or below this are silence ("-inf" in the UI).
inline constexpr double kSilenceDb = -144.0;

/// Converts decibels to linear gain; <= kSilenceDb is silence.
double dbToGain(double db) noexcept;

} // namespace daw
