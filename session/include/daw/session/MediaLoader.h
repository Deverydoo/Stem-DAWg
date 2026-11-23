// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Background media preparation: decode -> engine-rate rendition -> peaks
// (from cache when valid, otherwise built and written back to the cache).

#include <daw/core/CancellationToken.h>
#include <daw/core/Result.h>
#include <daw/media/AudioFileDecoder.h>
#include <daw/project/ProjectModel.h>
#include <daw/session/MediaPool.h>

#include <filesystem>

namespace daw {

struct MediaLoadOptions {
    double engineSampleRate = 0.0; // 0 = keep native rate (no rendition)
    bool usePeakCache = true;
    DecodeOptions decode;
    /// SoundFont for MIDI media (rendered at the media's recorded sample rate).
    std::filesystem::path soundFont;
};

Result<LoadedMedia> loadMedia(const std::filesystem::path& projectRoot, const MediaRef& media,
                              const MediaLoadOptions& options, const CancellationToken& cancel = {});

/// Re-targets an already loaded media to a new engine rate (device change).
Result<LoadedMedia> retargetMedia(const LoadedMedia& media, double engineSampleRate,
                                  const CancellationToken& cancel = {});

} // namespace daw
