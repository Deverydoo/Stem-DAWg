// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/project/ProjectModel.h>

#include <daw/core/Files.h>

#include <algorithm>
#include <cmath>

namespace daw {

const MediaRef* ProjectModel::findMedia(const MediaId& mediaId) const
{
    const auto it = std::find_if(media.begin(), media.end(), [&](const MediaRef& m) { return m.id == mediaId; });
    return it == media.end() ? nullptr : &*it;
}

bool ProjectModel::anySoloed() const noexcept
{
    return std::any_of(tracks.begin(), tracks.end(), [](const Track& t) { return t.soloed; });
}

bool ProjectModel::isAudible(const Track& track) const noexcept
{
    if (track.muted)
        return false;
    return track.soloed || !anySoloed();
}

const Track* ProjectModel::findTrack(const TrackId& trackId) const
{
    const auto it = std::find_if(tracks.begin(), tracks.end(), [&](const Track& t) { return t.id == trackId; });
    return it == tracks.end() ? nullptr : &*it;
}

Track* ProjectModel::findTrack(const TrackId& trackId)
{
    const auto it = std::find_if(tracks.begin(), tracks.end(), [&](const Track& t) { return t.id == trackId; });
    return it == tracks.end() ? nullptr : &*it;
}

std::int64_t ProjectModel::contentEnd() const
{
    std::int64_t end = 0;
    for (const Track& track : tracks)
        for (const Clip& clip : track.clips)
            end = std::max(end, clip.timelineStart + clip.length);
    return end;
}

ProjectModel makeEmptyProject(std::string name)
{
    ProjectModel model;
    model.id = ProjectId::generate();
    model.name = std::move(name);
    model.createdUtc = utcTimestampIso8601();
    model.modifiedUtc = model.createdUtc;
    return model;
}

double dbToGain(double db) noexcept
{
    if (db <= kSilenceDb)
        return 0.0;
    return std::pow(10.0, db / 20.0);
}

} // namespace daw
