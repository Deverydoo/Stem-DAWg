// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Offline mixdown ("final render") and export.
//
// The render drives a private AudioEngine instance with the same snapshot the
// live engine plays, so the exported mix matches playback exactly: same
// mixer, pan law, mute/solo and gain ramps. It renders at the project sample
// rate from the original media (resampling only media whose native rate
// differs), never from the device-rate playback copies.

#include <daw/core/AudioAsset.h>
#include <daw/core/CancellationToken.h>
#include <daw/core/Result.h>
#include <daw/dsp/LoudnessMeter.h>
#include <daw/media/AudioTags.h>
#include <daw/media/WavWriter.h>
#include <daw/project/Metadata.h>
#include <daw/project/ProjectModel.h>
#include <daw/session/MediaPool.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>

namespace daw {

struct RenderOptions {
    int blockSize = 2048;
    std::function<void(double)> progress; // 0..1, called from the rendering thread
};

struct RenderReport {
    std::int64_t frames = 0;
    double sampleRate = 0.0;
    double peakDbfs = -144.0;        // sample peak of the mix before any conversion
    std::int64_t clippedSamples = 0; // samples beyond +-1.0 (they clip in PCM formats)
    LoudnessReading loudness;        // integrated loudness, range and true peak of the mix
    double renderSeconds = 0.0;
};

struct RenderedMix {
    AudioAssetPtr audio; // stereo, project rate
    RenderReport report;
};

/// The media used by the project's clips at `sampleRate`: originals that
/// already match, high-quality renditions of the rest. Fails if any media is
/// not loaded in `pool`.
Result<std::unique_ptr<MediaPool>> prepareMediaAtRate(const ProjectModel& model, const MediaPool& pool, double sampleRate,
                                                      const CancellationToken& cancel = {});

/// Renders the whole project (0 .. end of the last clip). Every referenced
/// media item must be loaded in `pool`.
Result<RenderedMix> renderMix(const ProjectModel& model, const MediaPool& pool, const RenderOptions& options = {},
                              const CancellationToken& cancel = {});

/// Converts resolved metadata into file tags. `artwork` holds the image bytes
/// (already read from metadata.artworkPath, or empty).
AudioTags makeAudioTags(const MetadataFields& resolved, std::string software, std::string artwork = {});

struct ExportRequest {
    std::filesystem::path path;
    WavSampleFormat format = WavSampleFormat::Pcm24;
    bool dither = true; // applied to PCM formats only
    MetadataFields metadata; // already resolved (resolveMetadata)
    std::string software;    // e.g. "Stem DAWg 0.1.0"
};

struct ExportReport {
    RenderReport render;
    std::uint64_t bytes = 0;
    bool artworkEmbedded = false;
    std::string artworkWarning; // non-empty when artwork was requested but not embedded
};

/// Renders, tags and writes the file atomically.
Result<ExportReport> exportMix(const ProjectModel& model, const MediaPool& pool, const ExportRequest& request,
                               const RenderOptions& options = {}, const CancellationToken& cancel = {});

} // namespace daw
