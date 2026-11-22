// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/session/SnapshotBuilder.h>

#include <daw/core/Hash.h>

#include <algorithm>
#include <numbers>

namespace daw {

ChannelGains trackChannelGains(const ProjectModel& model, const Track& track) noexcept
{
    if (!model.isAudible(track))
        return {0.0f, 0.0f};
    const double gain = dbToGain(track.gainDb);
    const double pan = std::clamp(track.pan, -1.0, 1.0);
    // cos(pi/2) is not exactly 0 in floating point; hard pan must fully mute the far side.
    const double opposite = std::abs(pan) >= 1.0 ? 0.0 : std::cos(std::abs(pan) * std::numbers::pi / 2.0);
    return {static_cast<float>(gain * (pan > 0.0 ? opposite : 1.0)), static_cast<float>(gain * (pan < 0.0 ? opposite : 1.0))};
}

std::unique_ptr<PlaybackSnapshot> buildPlaybackSnapshot(const ProjectModel& model, const MediaPool& pool,
                                                        double engineSampleRate, std::uint64_t version,
                                                        SnapshotBuildReport* report)
{
    const TimeMapper map{model.sampleRate, engineSampleRate};
    auto snapshot = std::make_unique<PlaybackSnapshot>();
    snapshot->version = version;
    snapshot->sampleRate = engineSampleRate;
    // The master fader is folded into each track's gains so the engine ramps
    // master changes exactly like track changes (no clicks).
    const auto master = static_cast<float>(dbToGain(model.masterGainDb));
    snapshot->masterGain = 1.0f;

    for (const Track& track : model.tracks) {
        TrackPlayback playback;
        const ChannelGains gains = trackChannelGains(model, track);
        playback.gainLeft = gains.left * master;
        playback.gainRight = gains.right * master;
        playback.key = fnv1a64(track.id.str()) | 1u; // never 0

        for (const Clip& clip : track.clips) {
            const auto loaded = pool.get(clip.media);
            if (!loaded || !loaded->rendition || loaded->renditionRate != engineSampleRate) {
                if (report != nullptr)
                    report->missingMedia.push_back(clip.media);
                continue;
            }
            ClipPlayback cp;
            cp.asset = loaded->rendition.get();
            cp.timelineStart = map.toEngine(clip.timelineStart);
            cp.length = map.toEngine(clip.timelineStart + clip.length) - cp.timelineStart;
            cp.sourceStart = map.toEngine(clip.sourceStart);
            cp.gain = static_cast<float>(dbToGain(clip.gainDb));
            snapshot->endFrame = std::max(snapshot->endFrame, cp.timelineStart + cp.length);
            if (std::find(snapshot->keepAlive.begin(), snapshot->keepAlive.end(), loaded->rendition)
                == snapshot->keepAlive.end())
                snapshot->keepAlive.push_back(loaded->rendition);
            playback.clips.push_back(cp);
        }
        std::sort(playback.clips.begin(), playback.clips.end(),
                  [](const ClipPlayback& a, const ClipPlayback& b) { return a.timelineStart < b.timelineStart; });
        snapshot->tracks.push_back(std::move(playback));
    }
    return snapshot;
}

} // namespace daw
