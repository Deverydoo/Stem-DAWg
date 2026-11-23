// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/session/ProjectOperations.h>

#include <daw/core/Files.h>
#include <daw/media/AudioFileDecoder.h>
#include <daw/media/Midi.h>
#include <daw/media/PeakCache.h>
#include <daw/project/ProjectStore.h>

#include <algorithm>
#include <cmath>
#include <set>

namespace fs = std::filesystem;

namespace daw {

Result<MediaRef> importAudioFile(const fs::path& projectRoot, const fs::path& source, MediaStorage storage,
                                 double midiSampleRate)
{
    Result<AudioFileInfo> info = makeError(ErrorCode::Unknown, "not probed");
    if (isMidiFile(source)) {
        auto midi = probeMidiFile(source);
        if (!midi)
            return midi.error();
        AudioFileInfo rendered;
        rendered.sampleRate = midiSampleRate > 0.0 ? midiSampleRate : 48000.0;
        rendered.numChannels = 2;
        rendered.numFrames = midiRenderFrames(*midi, rendered.sampleRate);
        rendered.formatName = "MIDI";
        info = rendered;
    } else {
        info = probeAudioFile(source);
    }
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

bool setTrackSolo(ProjectModel& model, const TrackId& id, bool soloed, bool exclusive)
{
    Track* target = model.findTrack(id);
    if (target == nullptr)
        return false;
    bool changed = target->soloed != soloed;
    target->soloed = soloed;
    if (soloed && exclusive)
        for (Track& track : model.tracks)
            if (&track != target && track.soloed) {
                track.soloed = false;
                changed = true;
            }
    return changed;
}

std::vector<std::string> shortTrackNames(const std::vector<std::string>& names)
{
    if (names.size() < 2)
        return names;
    const auto isSeparator = [](char c) { return c == '(' || c == ')' || c == '[' || c == ']' || c == '_' || c == '-'; };
    const auto isTrim = [&](char c) { return c == ' ' || isSeparator(c); };

    std::size_t prefix = names.front().size();
    std::size_t suffix = names.front().size();
    for (const std::string& name : names) {
        const std::string& first = names.front();
        std::size_t p = 0;
        while (p < prefix && p < name.size() && name[p] == first[p])
            ++p;
        prefix = p;
        std::size_t s = 0;
        while (s < suffix && s < name.size() && name[name.size() - 1 - s] == first[first.size() - 1 - s])
            ++s;
        suffix = s;
    }
    // Only cut at a separator, so a shared word start ("Synth Keys",
    // "Synth Pad") is never split.
    while (prefix > 0 && !isSeparator(names.front()[prefix - 1]))
        --prefix;
    while (suffix > 0 && !isSeparator(names.front()[names.front().size() - suffix]))
        --suffix;

    std::vector<std::string> result;
    result.reserve(names.size());
    for (const std::string& name : names) {
        if (prefix + suffix >= name.size())
            return names;
        std::size_t begin = prefix, end = name.size() - suffix;
        while (begin < end && isTrim(name[begin]))
            ++begin;
        while (end > begin && isTrim(name[end - 1]))
            --end;
        if (begin == end)
            return names;
        result.push_back(name.substr(begin, end - begin));
    }
    return result;
}

bool renameTrack(ProjectModel& model, const TrackId& id, const std::string& name)
{
    Track* track = model.findTrack(id);
    constexpr const char* kSpace = " \t\r\n";
    const auto first = name.find_first_not_of(kSpace);
    if (track == nullptr || first == std::string::npos)
        return false;
    const std::string trimmed = name.substr(first, name.find_last_not_of(kSpace) - first + 1);
    if (trimmed == track->name)
        return false;
    for (Clip& clip : track->clips)
        if (clip.name == track->name)
            clip.name = trimmed;
    track->name = trimmed;
    return true;
}

bool removeTrack(ProjectModel& model, const TrackId& id)
{
    const auto it = std::find_if(model.tracks.begin(), model.tracks.end(), [&](const Track& t) { return t.id == id; });
    if (it == model.tracks.end())
        return false;
    model.tracks.erase(it);
    pruneUnusedMedia(model);
    return true;
}

void pruneUnusedMedia(ProjectModel& model)
{
    // Media a clip plays is used, and so is the source of used processed media.
    std::set<std::string> used;
    for (const Track& t : model.tracks)
        for (const Clip& c : t.clips)
            for (const MediaRef* m = model.findMedia(c.media); m != nullptr && used.insert(m->id.str()).second;)
                m = m->derivation ? model.findMedia(m->derivation->source) : nullptr;
    std::erase_if(model.media, [&](const MediaRef& m) { return !used.contains(m.id.str()); });
    std::erase_if(model.analyses, [&](const AnalysisRecord& a) { return model.findMedia(a.media) == nullptr; });
}

} // namespace daw
