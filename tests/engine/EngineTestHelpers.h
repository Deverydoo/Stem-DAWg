// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

#include <daw/engine/AudioEngine.h>

#include <memory>
#include <vector>

namespace daw::test {

inline std::unique_ptr<PlaybackSnapshot> singleClipSnapshot(const AudioAssetPtr& asset, std::int64_t timelineStart,
                                                            std::int64_t sourceStart, std::int64_t length,
                                                            std::uint64_t version = 1, float gain = 1.0f)
{
    auto snapshot = std::make_unique<PlaybackSnapshot>();
    snapshot->version = version;
    snapshot->sampleRate = asset->sampleRate;
    snapshot->keepAlive.push_back(asset);
    TrackPlayback track;
    track.clips.push_back(ClipPlayback{asset.get(), timelineStart, sourceStart, length, gain});
    snapshot->tracks.push_back(track);
    snapshot->endFrame = timelineStart + length;
    return snapshot;
}

struct StereoCapture {
    std::vector<float> left;
    std::vector<float> right;
};

/// Renders `totalFrames` frames in blocks of `blockSize` and returns the output.
inline StereoCapture renderBlocks(AudioEngine& engine, std::int64_t totalFrames, int blockSize)
{
    StereoCapture capture;
    capture.left.resize(static_cast<std::size_t>(totalFrames));
    capture.right.resize(static_cast<std::size_t>(totalFrames));
    std::vector<float> l(static_cast<std::size_t>(blockSize)), r(static_cast<std::size_t>(blockSize));
    for (std::int64_t done = 0; done < totalFrames; done += blockSize) {
        const int n = static_cast<int>(std::min<std::int64_t>(blockSize, totalFrames - done));
        float* outs[2] = {l.data(), r.data()};
        engine.process(outs, 2, n);
        std::copy_n(l.begin(), n, capture.left.begin() + done);
        std::copy_n(r.begin(), n, capture.right.begin() + done);
    }
    return capture;
}

} // namespace daw::test
