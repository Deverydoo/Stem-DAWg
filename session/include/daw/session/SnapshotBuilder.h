// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// ProjectModel + MediaPool -> PlaybackSnapshot for the engine.

#include <daw/engine/PlaybackSnapshot.h>
#include <daw/project/ProjectModel.h>
#include <daw/session/MediaPool.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace daw {

/// Maps project-rate frames to engine-rate frames (identity when equal).
struct TimeMapper {
    double projectRate = 48000.0;
    double engineRate = 48000.0;

    std::int64_t toEngine(std::int64_t projectFrames) const noexcept
    {
        if (projectRate == engineRate)
            return projectFrames;
        return static_cast<std::int64_t>(std::llround(static_cast<double>(projectFrames) * engineRate / projectRate));
    }

    std::int64_t toProject(std::int64_t engineFrames) const noexcept
    {
        if (projectRate == engineRate)
            return engineFrames;
        return static_cast<std::int64_t>(std::llround(static_cast<double>(engineFrames) * projectRate / engineRate));
    }
};

/// Linear output gains for one track.
struct ChannelGains {
    float left = 1.0f;
    float right = 1.0f;
};

/// Resolves a track's fader, pan, mute and solo-in-place into output gains.
/// Pan is a balance law with unity at centre (a centred, 0 dB track renders
/// bit-identically): moving right attenuates the left channel by cos(pan*pi/2)
/// and vice versa. Mono media feeds both sides before the balance is applied.
ChannelGains trackChannelGains(const ProjectModel& model, const Track& track) noexcept;

struct SnapshotBuildReport {
    std::vector<MediaId> missingMedia; // referenced but not loaded at the engine rate
};

std::unique_ptr<PlaybackSnapshot> buildPlaybackSnapshot(const ProjectModel& model, const MediaPool& pool,
                                                        double engineSampleRate, std::uint64_t version,
                                                        SnapshotBuildReport* report = nullptr);

} // namespace daw
