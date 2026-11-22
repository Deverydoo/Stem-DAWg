// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

#include <daw/core/AudioAsset.h>

#include <cmath>
#include <vector>

namespace daw::detail {

/// Average of all channels.
inline std::vector<float> monoMix(const AudioAsset& audio)
{
    std::vector<float> mono(static_cast<std::size_t>(audio.numFrames()), 0.0f);
    if (audio.numChannels() == 0)
        return mono;
    const float scale = 1.0f / static_cast<float>(audio.numChannels());
    for (int c = 0; c < audio.numChannels(); ++c) {
        const float* in = audio.channelData(c);
        for (std::size_t i = 0; i < mono.size(); ++i)
            mono[i] += in[i] * scale;
    }
    return mono;
}

/// Linear amplitude to dB, floored (JSON cannot represent -inf).
inline double amplitudeToDb(double amplitude, double floorDb = -200.0)
{
    if (amplitude <= 0.0)
        return floorDb;
    return std::max(floorDb, 20.0 * std::log10(amplitude));
}

inline double powerToDb(double power, double floorDb = -200.0)
{
    if (power <= 0.0)
        return floorDb;
    return std::max(floorDb, 10.0 * std::log10(power));
}

} // namespace daw::detail
