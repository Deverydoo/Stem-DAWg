// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Voice denoise (speech) for a track: the DeepFilterNet3 model renders a
// processed copy of the track's media into the project (Media/Processed), and
// the track's clips then play the copy. The original stays in the project, so
// the track can go back to it at any time (and the change is undoable): the
// processing is non-destructive, it just cannot run live.
//
// It always processes the original, so changing the strength re-renders from
// the untouched recording rather than processing twice.
//
// The model is made for speech. It also reports how much it changed the
// loudest parts (the voice) and the quietest parts (the noise): a sung or
// heavily effected voice can be taken for noise, and the app warns when the
// voice drops by more than kVoiceLossWarningDb.

#include <daw/ai/VoiceDenoiser.h>
#include <daw/core/AudioAsset.h>
#include <daw/core/CancellationToken.h>
#include <daw/core/Result.h>
#include <daw/project/ProjectModel.h>
#include <daw/session/MediaPool.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <vector>

namespace daw {

inline constexpr const char* kVoiceDenoiseProcess = "voice-denoise";
inline constexpr double kVoiceLossWarningDb = -2.0;

/// The voice model Stem DAWg offers to download: DeepFilterNet3's ONNX export
/// (Hendrik Schröter, MIT or Apache-2.0), pinned to a commit of its repository.
/// The archive and each extracted graph are verified by SHA-256.
struct VoiceModelFile {
    const char* name;
    std::uint64_t bytes;
    const char* sha256;
};

struct VoiceModelPackage {
    const char* displayName;
    const char* url;
    const char* fallbackUrl;
    std::uint64_t archiveBytes;
    const char* archiveSha256;
    VoiceModelFile files[3];
    const char* folderName; // under appDataDirectory()/Models
    const char* licence;
};

inline constexpr VoiceModelPackage kDeepFilterNet3{
    "DeepFilterNet3",
    "https://raw.githubusercontent.com/Rikorose/DeepFilterNet/84d57ec2c08fe08e68a13fb32a58cd7092060a0f/models/DeepFilterNet3_onnx.tar.gz",
    "https://github.com/Rikorose/DeepFilterNet/raw/84d57ec2c08fe08e68a13fb32a58cd7092060a0f/models/DeepFilterNet3_onnx.tar.gz",
    7983136,
    "c94d91f70911001c946e0fabb4aa9adc37045f45a03b56008cb0c8244cb63616",
    {{VoiceDenoiser::kEncoderFile, 1954042, "7c5399d3da8a50ebef1c1a0ae421b33376aa5e45d0e92df16da7e83c9c131916"},
     {VoiceDenoiser::kErbDecoderFile, 3292397, "ab669a1d10afe20911728b33053a452071042317a90581092b325da7b2f9d895"},
     {VoiceDenoiser::kDfDecoderFile, 3340803, "23114ce3b0f6464b763ee62f7bb8aab6b2a129a21eabd5bcfe59413db05f278a"}},
    "DeepFilterNet3",
    "Copyright (c) 2021 Hendrik Schröter. MIT or Apache-2.0.",
};

/// Where the voice model is installed: appDataDirectory()/Models/DeepFilterNet3.
std::filesystem::path voiceModelDirectory();

/// Whether `directory` holds the three graphs at their expected sizes (cheap;
/// the hashes were checked when they were installed).
bool isVoiceModelInstalled(const std::filesystem::path& directory = voiceModelDirectory());

/// Verifies a downloaded archive against kDeepFilterNet3, extracts the graphs
/// into `directory` and verifies each. Nothing is left behind on failure.
Status installVoiceModel(const std::filesystem::path& archive, const std::filesystem::path& directory = voiceModelDirectory(),
                         const CancellationToken& cancel = {});

struct VoiceDenoiseOutcome {
    MediaRef media;             // the processed copy, derived from the original
    LoadedMedia loaded;         // ready for the media pool
    double voiceChangeDb = 0.0; // level change over the louder half of the original's 20 ms blocks
    double noiseChangeDb = 0.0; // level change over its quietest tenth
};

/// Renders `audio` (the decoded `original`, at its native rate) through the
/// voice model into Media/Processed and loads the result for `engineRate`.
/// `progress` receives 0..1.
Result<VoiceDenoiseOutcome> denoiseVoiceMedia(const std::filesystem::path& projectRoot, const MediaRef& original,
                                              const AudioAsset& audio, const VoiceDenoiser& denoiser, double reductionDb,
                                              double engineRate, const CancellationToken& cancel = {},
                                              const std::function<void(double)>& progress = {});

/// Follows processed media back to the recording it was made from.
const MediaRef* originalMediaOf(const ProjectModel& model, const MediaId& media);

/// The distinct original media the track's clips play (before any processing).
std::vector<MediaRef> trackOriginalMedia(const ProjectModel& model, const TrackId& track);

/// The rendered process the track's clips play (voice denoise, bleed
/// suppression...), when every clip plays media processed the same way.
std::optional<MediaDerivation> trackProcessing(const ProjectModel& model, const TrackId& track);

/// The voice denoise the track's clips play, when every clip plays voice-denoised media.
std::optional<MediaDerivation> trackVoiceDenoise(const ProjectModel& model, const TrackId& track);

/// Makes the track's clips play the processed copies (each replaces the clips
/// whose original is its source) and adds them to the project. Processed media
/// left unused is dropped from the model. Returns false when nothing changed.
bool applyProcessedMedia(ProjectModel& model, const TrackId& track, const std::vector<MediaRef>& processed);

/// Makes the track's clips play their original media again. Returns false
/// when the track plays no processed media.
bool revertTrackProcessing(ProjectModel& model, const TrackId& track);

/// Clips playing a processed copy whose file is gone (deleted as unused when
/// the project was closed, then a backup was restored) play their original
/// again. Returns how many clips changed.
std::size_t fallBackFromMissingProcessedMedia(ProjectModel& model, const std::filesystem::path& projectRoot);

} // namespace daw
