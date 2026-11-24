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

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace daw {

enum class MediaStorage {
    Copied,     // file lives inside the project (path is project-relative)
    Referenced, // file lives elsewhere (path is absolute)
};

/// How a processed media file was made from another (non-destructive
/// processing: the source stays in the project, so it can be restored).
struct MediaDerivation {
    MediaId source;         // the media this was made from
    std::string process;    // e.g. "voice-denoise"
    double amountDb = 0.0;  // the process's strength (voice denoise: its reduction limit)

    friend bool operator==(const MediaDerivation&, const MediaDerivation&) = default;
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
    std::optional<MediaDerivation> derivation; // set for processed copies

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

/// High-pass filter of a track's clean-up chain (Butterworth).
struct HighPassSettings {
    static constexpr double kMinHz = 20.0;
    static constexpr double kMaxHz = 1000.0;

    bool enabled = false;
    double frequencyHz = 120.0; // -3 dB point
    int slopeDbPerOctave = 24;  // 12, 24 or 48

    friend bool operator==(const HighPassSettings&, const HighPassSettings&) = default;
};

/// Downward expander of a track's clean-up chain: below the threshold the
/// level drops `ratio` times faster, by at most `rangeDb`. Used to push the
/// noise bed between notes down without touching the notes.
struct ExpanderSettings {
    bool enabled = false;
    double thresholdDb = -50.0; // RMS level (mean of both channels' power), dBFS
    double ratio = 4.0;         // 1 .. 20
    double rangeDb = 24.0;      // maximum attenuation, 0 .. 90
    double releaseMs = 80.0;    // closing time after the hold, 10 .. 2000

    friend bool operator==(const ExpanderSettings&, const ExpanderSettings&) = default;
};

/// Notches at the mains frequency and its harmonics.
struct HumSettings {
    bool enabled = false;
    double frequencyHz = 50.0; // 50 (Europe, most of the world) or 60 (the Americas); 45 .. 65
    int harmonics = 4;         // notches at f, 2f, ... ; 1 .. 8

    friend bool operator==(const HumSettings&, const HumSettings&) = default;
};

/// The noise spectrum a denoiser removes, measured on the quietest parts of a
/// track: mean power per FFT bin in dB (bin k is k * sampleRate / fftSize Hz).
struct NoiseProfile {
    double sampleRate = 0.0;
    int fftSize = 0;
    double seconds = 0.0;       // how much audio it was learned from
    bool fromSelection = false; // learned from a selected region, not the quietest frames
    std::vector<float> powerDb; // fftSize / 2 + 1 values

    bool empty() const noexcept { return powerDb.empty() || fftSize <= 0 || !(sampleRate > 0.0); }
    friend bool operator==(const NoiseProfile&, const NoiseProfile&) = default;
};

/// Learned-profile spectral noise reduction.
struct DenoiseSettings {
    bool enabled = false;
    double reductionDb = 12.0;  // maximum attenuation, 0 .. 40
    double sensitivityDb = 0.0; // scales the profile: + removes more, - keeps more; -10 .. +10
    NoiseProfile profile;       // nothing is removed until a profile is learned

    friend bool operator==(const DenoiseSettings&, const DenoiseSettings&) = default;
};

/// Transient shaper: turns the start of each hit (attack) and the decay after
/// it (sustain) up or down, at any level. A positive attack restores the snap
/// AI-generated drums lose.
struct TransientSettings {
    static constexpr double kMaxDb = 12.0;

    bool enabled = false;
    double attackDb = 6.0;  // -12 .. +12
    double sustainDb = 0.0; // -12 .. +12

    friend bool operator==(const TransientSettings&, const TransientSettings&) = default;
};

/// Harshness and shimmer control: turns down narrow peaks that persist above
/// the surrounding spectrum, by at most the set depth per range. In AI-generated
/// music these are partials ringing through many notes (2-6 kHz) and steady
/// generator tones (8-17.5 kHz). A depth of 0 leaves that range alone.
struct ResonanceSettings {
    static constexpr double kMaxDb = 12.0;

    bool enabled = false;
    double harshDb = 6.0;   // maximum cut 2-6 kHz, 0 .. 12
    double shimmerDb = 6.0; // maximum cut 8-17.5 kHz, 0 .. 12

    friend bool operator==(const ResonanceSettings&, const ResonanceSettings&) = default;
};

/// Stereo repair: mono below a frequency (separated stems often carry a wide
/// or out-of-phase low end), width (side gain), and phase alignment, which
/// brings left and right into step where they carry the same sound out of
/// step, so a mono sum keeps it.
struct StereoSettings {
    static constexpr double kMinHz = 40.0;
    static constexpr double kMaxHz = 300.0;
    static constexpr double kMaxWidthPercent = 200.0;

    bool enabled = false;
    bool monoBass = true;
    double monoBelowHz = 120.0;  // 40 .. 300
    double widthPercent = 100.0; // 0 (mono) .. 200
    bool phaseAlign = false;

    friend bool operator==(const StereoSettings&, const StereoSettings&) = default;
};

/// A tonal-balance target as a project stores it: built from reference
/// tracks (dsp/TonalBalance) and copied in, so the project compares the same
/// way on any machine. Per third-octave band from 31.5 Hz to 16 kHz.
struct TonalTargetSettings {
    static constexpr std::size_t kBands = 28;

    std::string name;
    std::vector<std::string> references; // file names, for the user's information
    std::vector<double> meanDb;          // kBands values: each band's share of the total
    std::vector<double> spreadDb;        // kBands values: how much the references disagree

    bool empty() const noexcept { return meanDb.empty(); }
    friend bool operator==(const TonalTargetSettings&, const TonalTargetSettings&) = default;
};

/// The master bus after the summed tracks (milestone 5): tonal EQ (five fixed
/// bands, see dsp/MasterChain), a three-band compressor (dsp/Multiband), a
/// true-peak limiter, the loudness the master fader is set for, and the tonal
/// target the mix is compared with. Off by default.
struct MasterSettings {
    static constexpr double kMinCeilingDbtp = -12.0;
    static constexpr double kMaxCeilingDbtp = 0.0;
    static constexpr double kMinReleaseMs = 10.0;
    static constexpr double kMaxReleaseMs = 1000.0;
    static constexpr double kMinTargetLufs = -30.0;
    static constexpr double kMaxTargetLufs = -5.0;
    static constexpr std::size_t kEqBands = 5; // low shelf 100 Hz, bells 250 Hz, 1 kHz, 3 kHz, high shelf 8 kHz
    static constexpr double kMaxEqDb = 12.0;
    static constexpr std::size_t kMultibandBands = 3; // split at 120 Hz and 2.5 kHz
    static constexpr double kMinThresholdDb = -60.0;
    static constexpr double kMaxRatio = 10.0;

    bool limiter = false;
    double ceilingDbtp = -1.0;
    double releaseMs = 100.0;
    double targetLufs = -14.0; // what Match Loudness aims for
    bool eq = false;
    std::array<double, kEqBands> eqGainDb{};
    TonalTargetSettings tonalTarget; // empty: none
    bool multiband = false;
    std::array<double, kMultibandBands> multibandThresholdDb{-20.0, -20.0, -20.0}; // RMS, dBFS
    std::array<double, kMultibandBands> multibandRatio{2.0, 2.0, 2.0};
    bool clipper = false;
    double clipBelowCeilingDb = 1.0; // the clipper's level under the ceiling, 0 .. 6

    /// Limits every value to its range (non-finite values take the defaults).
    void clamp() noexcept;
    friend bool operator==(const MasterSettings&, const MasterSettings&) = default;
};

/// Non-destructive per-track clean-up, applied before the fader in this order:
/// DC removal, high-pass, stereo imager (mono bass, width), hum removal,
/// noise reduction, resonance suppression and phase alignment (one spectral
/// stage), expander, transient shaper.
struct TrackCleanup {
    bool bypassed = false; // keeps the settings but plays the track unprocessed
    bool dcRemoval = false;
    HighPassSettings highPass;
    HumSettings hum;
    DenoiseSettings denoise;
    ExpanderSettings expander;
    TransientSettings transient;
    ResonanceSettings resonance;
    StereoSettings stereo;

    /// True when any stage is switched on (regardless of bypass).
    bool hasStages() const noexcept
    {
        return dcRemoval || highPass.enabled || hum.enabled || denoise.enabled || expander.enabled || transient.enabled
               || resonance.enabled || stereo.enabled;
    }
    /// True when the chain changes the sound.
    bool active() const noexcept { return !bypassed && hasStages(); }
    /// Limits every value to its documented range (slope snaps to 12/24/48).
    void clamp() noexcept;

    friend bool operator==(const TrackCleanup&, const TrackCleanup&) = default;
};

struct Track {
    TrackId id;
    std::string name;
    double gainDb = 0.0;   // fader; <= kSilenceDb is silence
    double pan = 0.0;      // -1 (left) .. 0 (centre) .. +1 (right), balance law, unity at centre
    bool muted = false;
    bool soloed = false;   // solo-in-place; mute wins over solo
    TrackCleanup cleanup;
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
    double masterGainDb = 0.0; // the master fader: before the master bus, so the drive into its limiter
    MasterSettings master;
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
