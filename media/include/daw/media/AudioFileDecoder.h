// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Audio file probing and decoding into memory-backed AudioAssets.
// Runs on background threads only. Formats: WAV, AIFF, FLAC, Ogg Vorbis, MP3.
//
// Milestone 1 keeps whole files in memory, bounded by DecodeOptions. Disk
// streaming replaces this in Milestone 2 without changing callers that only
// consume AudioAssetPtr.

#include <daw/core/AudioAsset.h>
#include <daw/core/CancellationToken.h>
#include <daw/core/Result.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace daw {

struct AudioFileInfo {
    double sampleRate = 0.0;
    int numChannels = 0;
    std::int64_t numFrames = 0;
    int bitsPerSample = 0;
    std::string formatName;
};

struct DecodeOptions {
    std::int64_t maxFrames = 192000LL * 60 * 30;       // 30 minutes at 192 kHz
    int maxChannels = 8;
    std::size_t maxBytes = std::size_t{2} * 1024 * 1024 * 1024; // decoded float32 budget per file
};

using ProgressCallback = std::function<void(float)>; // 0..1

Result<AudioFileInfo> probeAudioFile(const std::filesystem::path& path);

Result<AudioAssetPtr> decodeAudioFile(const std::filesystem::path& path,
                                      const DecodeOptions& options = {},
                                      const CancellationToken& cancel = {},
                                      const ProgressCallback& progress = {});

/// Lowercase extensions including the dot, e.g. ".wav".
std::vector<std::string> supportedAudioExtensions();

} // namespace daw
