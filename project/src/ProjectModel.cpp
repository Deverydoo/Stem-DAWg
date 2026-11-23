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

void TrackCleanup::clamp() noexcept
{
    const auto limit = [](double value, double lo, double hi, double fallback) {
        return std::isfinite(value) ? std::clamp(value, lo, hi) : fallback;
    };
    const TrackCleanup defaults;
    highPass.frequencyHz = limit(highPass.frequencyHz, HighPassSettings::kMinHz, HighPassSettings::kMaxHz,
                                 defaults.highPass.frequencyHz);
    highPass.slopeDbPerOctave = highPass.slopeDbPerOctave <= 18 ? 12 : highPass.slopeDbPerOctave <= 36 ? 24 : 48;
    expander.thresholdDb = limit(expander.thresholdDb, -90.0, 0.0, defaults.expander.thresholdDb);
    expander.ratio = limit(expander.ratio, 1.0, 20.0, defaults.expander.ratio);
    expander.rangeDb = limit(expander.rangeDb, 0.0, 90.0, defaults.expander.rangeDb);
    expander.releaseMs = limit(expander.releaseMs, 10.0, 2000.0, defaults.expander.releaseMs);
    hum.frequencyHz = limit(hum.frequencyHz, 45.0, 65.0, defaults.hum.frequencyHz);
    hum.harmonics = std::clamp(hum.harmonics, 1, 8);
    denoise.reductionDb = limit(denoise.reductionDb, 0.0, 40.0, defaults.denoise.reductionDb);
    denoise.sensitivityDb = limit(denoise.sensitivityDb, -10.0, 10.0, defaults.denoise.sensitivityDb);
    NoiseProfile& p = denoise.profile;
    const bool consistent = p.fftSize >= 16 && (p.fftSize & (p.fftSize - 1)) == 0 && p.sampleRate > 0.0
                            && std::isfinite(p.sampleRate) && p.powerDb.size() == static_cast<std::size_t>(p.fftSize / 2 + 1);
    if (!consistent)
        p = {};
    for (float& v : p.powerDb)
        v = std::isfinite(v) ? std::clamp(v, -300.0f, 100.0f) : -300.0f;
    p.seconds = std::isfinite(p.seconds) ? std::max(0.0, p.seconds) : 0.0;
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
