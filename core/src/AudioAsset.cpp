// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/core/AudioAsset.h>

namespace daw {

std::shared_ptr<AudioAsset> AudioAsset::create(int numChannels, std::int64_t numFrames, double sampleRate)
{
    auto asset = std::make_shared<AudioAsset>();
    asset->sampleRate = sampleRate;
    asset->channels.resize(static_cast<std::size_t>(numChannels));
    for (auto& channel : asset->channels)
        channel.assign(static_cast<std::size_t>(numFrames), 0.0f);
    return asset;
}

} // namespace daw
