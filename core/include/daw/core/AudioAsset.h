// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Decoded, immutable, memory-resident audio. Produced on background threads,
// shared as std::shared_ptr<const AudioAsset>, and read (never written) by the
// audio thread through raw pointers held inside a PlaybackSnapshot.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace daw {

struct AudioAsset {
    double sampleRate = 0.0;
    std::vector<std::vector<float>> channels; // planar, all channels equal length
    std::string sourcePath;                   // UTF-8, informational

    int numChannels() const noexcept { return static_cast<int>(channels.size()); }
    std::int64_t numFrames() const noexcept
    {
        return channels.empty() ? 0 : static_cast<std::int64_t>(channels.front().size());
    }
    double durationSeconds() const noexcept { return sampleRate > 0.0 ? static_cast<double>(numFrames()) / sampleRate : 0.0; }
    const float* channelData(int channel) const noexcept { return channels[static_cast<std::size_t>(channel)].data(); }
    std::size_t memoryBytes() const noexcept
    {
        return static_cast<std::size_t>(numFrames()) * channels.size() * sizeof(float);
    }

    static std::shared_ptr<AudioAsset> create(int numChannels, std::int64_t numFrames, double sampleRate);
};

using AudioAssetPtr = std::shared_ptr<const AudioAsset>;

} // namespace daw
