// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/session/ProjectOperations.h>

#include <daw/core/Files.h>
#include <daw/media/AudioFileDecoder.h>
#include <daw/media/PeakCache.h>
#include <daw/project/ProjectStore.h>

#include <cmath>

namespace fs = std::filesystem;

namespace daw {

Result<MediaRef> importAudioFile(const fs::path& projectRoot, const fs::path& source, MediaStorage storage)
{
    auto info = probeAudioFile(source);
    if (!info)
        return info.error();

    auto stored = ProjectStore::stageMediaFile(projectRoot, source, storage);
    if (!stored)
        return stored.error();

    MediaRef ref;
    ref.id = MediaId::generate();
    ref.name = pathToUtf8(source.stem());
    ref.path = *stored;
    ref.storage = storage;
    ref.sampleRate = info->sampleRate;
    ref.numChannels = info->numChannels;
    ref.numFrames = info->numFrames;

    auto key = computeSourceKey(ProjectStore::resolveMediaPath(projectRoot, ref));
    if (!key)
        return key.error();
    ref.sourceKey = *key;
    return ref;
}

std::int64_t mediaLengthInProjectFrames(const ProjectModel& model, const MediaRef& media)
{
    if (media.sampleRate == model.sampleRate)
        return media.numFrames;
    return static_cast<std::int64_t>(std::llround(static_cast<double>(media.numFrames) * model.sampleRate / media.sampleRate));
}

TrackId addMediaAsNewTrack(ProjectModel& model, const MediaRef& media, std::int64_t timelineStart)
{
    if (model.media.empty() && model.tracks.empty() && media.sampleRate > 0.0)
        model.sampleRate = media.sampleRate;
    if (model.findMedia(media.id) == nullptr)
        model.media.push_back(media);

    Clip clip;
    clip.id = ClipId::generate();
    clip.name = media.name;
    clip.media = media.id;
    clip.timelineStart = timelineStart;
    clip.sourceStart = 0;
    clip.length = mediaLengthInProjectFrames(model, media);

    Track track;
    track.id = TrackId::generate();
    track.name = media.name;
    track.clips.push_back(std::move(clip));
    model.tracks.push_back(std::move(track));
    return model.tracks.back().id;
}

} // namespace daw
