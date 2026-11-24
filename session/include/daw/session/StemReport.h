// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Stem triage: measures every track of a project as it plays (fader and pan
// applied, mute and solo ignored so muted stems are still reported), compares
// the tracks with each other and turns the numbers into plain-language advice.
//
// What it answers, for AI-separated stems (Suno and similar) in particular:
//  * How much does each stem contribute, and is any of it just residue?
//  * Where is the noise bed of each stem, and does it have gaps to clean?
//  * Which stems share the same low end (a kick or bass doubled across stems)?
//  * Which stems bleed into each other?
//  * Do drum hits rise slowly, and do narrow resonances ring through the song?
//  * Is the low end mono, and what does a mono sum lose?
//  * Does the mix clip, and how loud is it (BS.1770 / EBU R 128)?
//
// Each stem is measured as it sounds, after its clean-up chain (high-pass,
// expander, DC removal), so problems a chain already solves drop out of the
// advice. Advice that a chain can solve carries ready-made CleanupFix values.
//
// Analysis only: the audio is never changed. Runs on a background thread and
// streams the project block by block, so memory stays small however long the
// song is.

#include <daw/core/CancellationToken.h>
#include <daw/core/Result.h>
#include <daw/dsp/LoudnessMeter.h>
#include <daw/project/ProjectModel.h>
#include <daw/session/MediaPool.h>

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace daw {

/// Frequency bands of the per-stem energy distribution.
enum class StemBand { Sub, Bass, LowMid, Mid, Presence, Air, Count };
inline constexpr std::size_t kStemBandCount = static_cast<std::size_t>(StemBand::Count);
const char* toString(StemBand band) noexcept;
/// Lower edge of each band in Hz (the upper edge is the next band's lower edge).
inline constexpr std::array<double, kStemBandCount> kStemBandEdgesHz{0.0, 60.0, 250.0, 1000.0, 4000.0, 8000.0};

/// Frequency ranges compared between stems.
enum class PairBand { Low, Mid, High, Full, Count };
inline constexpr std::size_t kPairBandCount = static_cast<std::size_t>(PairBand::Count);
inline constexpr double kLowEndHz = 120.0;     // "low end" for doubling checks
inline constexpr double kHighBandHz = 4000.0;
inline constexpr double kAttackBandHz = 2000.0; // attacks are measured above this (below it the envelope is the waveform)

/// The strongest persistent narrow peak in one range, as the resonance
/// suppressor sees it (dsp/ResonanceSuppressor) with kResonanceProbeDb of depth.
struct ResonanceFinding {
    double hz = 0.0;              // 0 when nothing was measured
    double cutDb = 0.0;           // mean cut the suppressor applies there while the stem plays
    double ringingFraction = 0.0; // fraction of that time it cuts at all
    double excessDb = 0.0;        // mean height above the surrounding spectrum while it cuts
};
inline constexpr double kResonanceProbeDb = 6.0; // depth the report measures with, and suggests

/// Frequency ranges of the mono-sum check (above the low end, which mono bass handles).
inline constexpr std::size_t kMonoBandCount = 4;
inline constexpr std::array<double, kMonoBandCount + 1> kMonoBandEdgesHz{120.0, 500.0, 2000.0, 8000.0, 16000.0};

/// What one range loses when the stem is summed to mono (phones, Bluetooth
/// speakers, club systems): 0 dB for a mono signal, -3 dB for unrelated
/// channels, lower when they cancel.
struct MonoBand {
    double lossDb = 0.0;
    double alignedLossDb = 0.0; // the same with phase alignment (dsp/PhaseAligner)
};

struct TrackStemReport {
    TrackId track;
    std::string name;
    bool muted = false;
    bool soloed = false;
    bool silent = false;              // no signal at all

    LoudnessReading loudness;         // the stem on its own, post-fader
    double levelShare = 0.0;          // fraction of the summed energy of all stems, 0..1
    double activeFraction = 0.0;      // fraction of the song where the stem plays
    double silentFraction = 0.0;      // fraction that is exact digital silence
    std::optional<double> noiseFloorDbfs; // RMS of the bed between notes; empty when the stem never rests
    std::int64_t sourceOvers = 0;     // full-scale samples in the source media (clipped renders)
    double dcOffset = 0.0;            // mean of the mid signal
    std::optional<double> stereoCorrelation; // empty when one side is silent
    double sideRatio = 0.0;           // side energy / (mid + side) energy: 0 = mono, 0.5 = uncorrelated
    std::array<double, kStemBandCount> bandShare{}; // energy fraction per StemBand
    double lowEndShare = 0.0;         // energy fraction below kLowEndHz
    double centroidHz = 0.0;
    // Hits: peaks of the level above kAttackBandHz that jump at least 15 dB
    // within 15 ms. Their rise time (10 % to 90 % of the jump in amplitude)
    // shows how sharp the attacks are: about 1 ms for a sampled drum.
    double hitsPerMinute = 0.0;       // loud hits (within 12 dB of the loudest)
    std::optional<double> attackMs;   // median rise time of the loud hits; empty with too few of them
    ResonanceFinding harshness;       // 2-6 kHz
    ResonanceFinding shimmer;         // 8-17.5 kHz
    std::optional<double> lowSideDb;  // side energy over mid energy below kLowEndHz; empty without low end
    double lowSideShare = 0.0;        // fraction of all stems' side energy below kLowEndHz
    std::array<MonoBand, kMonoBandCount> mono{}; // per kMonoBandEdgesHz range
    TrackCleanup cleanup;             // the chain in place when measured
};

struct StemPairReport {
    std::size_t a = 0; // indices into StemReport::tracks
    std::size_t b = 0;
    std::array<double, kPairBandCount> correlation{}; // per PairBand, -1..1
};

/// A change to one track's clean-up chain that solves an advice item. Only
/// the stages that are set change; applying a fix also lifts the bypass.
/// Resonance fixes merge: each range keeps the larger of the two depths, so
/// a harshness fix and a shimmer fix for the same track both apply. Stereo
/// fixes merge too: mono bass and phase alignment each stay on once set.
struct CleanupFix {
    std::size_t track = 0; // index into StemReport::tracks
    std::optional<bool> dcRemoval;
    std::optional<HighPassSettings> highPass;
    std::optional<ExpanderSettings> expander;
    std::optional<TransientSettings> transient;
    std::optional<ResonanceSettings> resonance;
    std::optional<StereoSettings> stereo;

    TrackCleanup applyTo(TrackCleanup cleanup) const;
    friend bool operator==(const CleanupFix&, const CleanupFix&) = default;
};

enum class AdviceSeverity { Warning, Suggestion, Info };
const char* toString(AdviceSeverity severity) noexcept;

struct StemAdvice {
    AdviceSeverity severity = AdviceSeverity::Info;
    std::string code;                 // stable identifier, e.g. "mix-clipping", "noise-bed"
    std::optional<std::size_t> track; // the track the advice is about, if any
    std::optional<std::size_t> otherTrack;
    std::string message;              // plain language, English
    std::vector<CleanupFix> fixes;    // clean-up settings that address it (may be empty)
};

struct StemReport {
    double sampleRate = 0.0;
    std::int64_t frames = 0;
    LoudnessReading mix;              // the mix as heard (mute, solo, master fader and master bus applied)
    MasterSettings master;            // the master bus it was measured with
    std::int64_t mixOvers = 0;        // mix samples beyond full scale
    std::vector<TrackStemReport> tracks;
    std::vector<StemPairReport> pairs;
    std::vector<StemAdvice> advice;
    double analysisSeconds = 0.0;

    const StemPairReport* pair(std::size_t a, std::size_t b) const noexcept;
};

struct StemReportOptions {
    std::function<void(double)> progress; // 0..1, called from the analysing thread
};

/// Measures the project. Every media item used by a clip must be loaded.
Result<StemReport> analyzeStems(const ProjectModel& model, const MediaPool& pool, const StemReportOptions& options = {},
                                const CancellationToken& cancel = {});

/// The advice rules, applied to measured tracks, pairs and mix (analyzeStems
/// calls this; exposed so the rules can be tested and re-run on their own).
std::vector<StemAdvice> adviseStems(const StemReport& report);

/// Every fix of every advice item, in advice order.
std::vector<CleanupFix> allCleanupFixes(const StemReport& report);

/// Applies fixes to the model's tracks (matched by the report's track ids).
/// Returns the number of tracks whose chain changed.
std::size_t applyCleanupFixes(ProjectModel& model, const StemReport& report, const std::vector<CleanupFix>& fixes);

/// Short summary of a chain, e.g. "HP 120 Hz 24 dB/oct, expander -44 dB, DC"; empty when it has no stages.
std::string describeCleanup(const TrackCleanup& cleanup);

nlohmann::json toJson(const StemReport& report);

} // namespace daw
