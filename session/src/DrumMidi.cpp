// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/session/DrumMidi.h>

#include <daw/core/Files.h>
#include <daw/media/Midi.h>
#include <daw/session/BeatTimeline.h>
#include <daw/session/Renderer.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <utility>

namespace daw {

int generalMidiDrumKey(DrumKind kind) noexcept
{
    switch (kind) {
    case DrumKind::Kick:
        return 36; // bass drum 1
    case DrumKind::Snare:
        return 38; // acoustic snare
    case DrumKind::ClosedHat:
        return 42;
    case DrumKind::OpenHat:
        return 46;
    case DrumKind::Crash:
        return 49; // crash cymbal 1
    case DrumKind::FloorTom:
        return 41; // low floor tom
    case DrumKind::LowTom:
        return 45;
    case DrumKind::HighTom:
        return 48; // hi-mid tom
    }
    return 36;
}

int drumVelocity(float strength) noexcept
{
    // A typical loud hit plays at 110; a quarter as strong at 60.
    const double v = 10.0 + 100.0 * std::sqrt(std::max(0.0f, strength));
    return static_cast<int>(std::clamp(std::lround(v), 1L, 127L));
}

MidiSong composeDrumMidi(const std::vector<DrumHit>& hits, const BeatTrack& track, int grid, const std::string& name)
{
    MidiSong song;
    song.name = name;
    MidiTrackData drums;
    drums.name = "Drums";
    drums.channel = kGmDrumChannel;
    if (track.beats.size() < 2) {
        song.ticksPerQuarter = BeatTimeline::kTicksPerQuarter;
        song.tracks.push_back(std::move(drums));
        return song;
    }
    const BeatTimeline timeline(track);
    timeline.conduct(song);

    // Notes, quantised within the beat; one per key and tick (the loudest).
    std::map<std::pair<std::int64_t, int>, int> notes;
    for (const DrumHit& h : hits) {
        int& velocity = notes[{timeline.tick(h.seconds, grid), generalMidiDrumKey(h.kind)}];
        velocity = std::max(velocity, drumVelocity(h.strength));
    }
    for (const auto& [at, velocity] : notes)
        drums.notes.push_back({at.first, BeatTimeline::kTicksPerQuarter / 8, at.second, velocity});
    song.tracks.push_back(std::move(drums));
    return song;
}

Result<DrumMidiResult> drumsToMidi(const AudioAsset& audio, const DrumMidiOptions& options, const std::string& name,
                                   const CancellationToken& cancel)
{
    if (audio.numChannels() < 1 || audio.numFrames() <= 0)
        return makeError(ErrorCode::InvalidArgument, "no audio to transcribe");
    DrumMidiResult result;
    DrumTranscriberParams params;
    params.sensitivity = options.sensitivity;
    result.transcription = transcribeDrums(audio.channels[0].data(), audio.channels[audio.numChannels() > 1 ? 1 : 0].data(),
                                           audio.numFrames(), audio.sampleRate, params);
    if (cancel.isCancelled())
        return makeError(ErrorCode::Cancelled, "drum transcription cancelled");
    if (result.transcription.hits.empty())
        return makeError(ErrorCode::InvalidArgument, "no drum hits found");
    result.beats = trackBeats(result.transcription);
    if (result.beats.beats.size() < 2)
        return makeError(ErrorCode::InvalidArgument, "too little rhythm to find the beats");
    result.song = composeDrumMidi(result.transcription.hits, result.beats, options.grid, name);
    result.notes = static_cast<int>(result.song.tracks.front().notes.size());
    return result;
}

Result<DrumMidiResult> trackDrumsToMidi(const ProjectModel& model, const MediaPool& pool, const TrackId& trackId,
                                        const DrumMidiOptions& options, const CancellationToken& cancel)
{
    const Track* track = model.findTrack(trackId);
    if (track == nullptr)
        return makeError(ErrorCode::NotFound, "no such track");
    auto rendered = renderTrackAlone(model, pool, trackId, cancel);
    if (!rendered)
        return rendered.error();
    return drumsToMidi(*rendered->audio, options, track->name, cancel);
}

namespace {

bool isMidiTrack(const ProjectModel& model, const Track& track)
{
    return std::any_of(track.clips.begin(), track.clips.end(), [&](const Clip& c) {
        const MediaRef* media = model.findMedia(c.media);
        return media != nullptr && isMidiFile(utf8ToPath(media->path));
    });
}

} // namespace

std::optional<TrackId> findDrumTrack(const ProjectModel& model)
{
    for (const Track& t : model.tracks) {
        std::string name = t.name;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        // Not a MIDI track (such as the drums transcribed from this one).
        if (name.find("drum") != std::string::npos && !isMidiTrack(model, t))
            return t.id;
    }
    return std::nullopt;
}

BeatSource songBeatSource(const ProjectModel& model)
{
    BeatSource source;
    if (const auto drums = findDrumTrack(model)) {
        source.drums = drums;
        source.name = model.findTrack(*drums)->name;
        return source;
    }
    for (const Track& t : model.tracks)
        if (!t.clips.empty() && !isMidiTrack(model, t))
            source.tracks.push_back(t.id);
    if (source.tracks.size() == 1)
        source.name = model.findTrack(source.tracks.front())->name;
    else if (!source.tracks.empty())
        source.name = "the mix";
    return source;
}

Result<BeatTrack> songBeats(const ProjectModel& model, const MediaPool& pool, const CancellationToken& cancel)
{
    const BeatSource source = songBeatSource(model);
    if (source.drums) {
        auto result = trackDrumsToMidi(model, pool, *source.drums, {}, cancel);
        if (!result)
            return result.error();
        return std::move(result->beats);
    }
    if (source.tracks.empty())
        return makeError(ErrorCode::NotFound, "no audio track to find the beats in");
    // The audio tracks played together as they sound (clean-up included),
    // each at unity gain and centred, without the master bus.
    ProjectModel together = model;
    together.tracks.clear();
    for (const TrackId& id : source.tracks) {
        Track t = *model.findTrack(id);
        t.gainDb = 0.0;
        t.pan = 0.0;
        t.muted = false;
        t.soloed = false;
        together.tracks.push_back(std::move(t));
    }
    together.masterGainDb = 0.0;
    together.master = MasterSettings{};
    auto rendered = renderMix(together, pool, {}, cancel);
    if (!rendered)
        return rendered.error();
    if (cancel.isCancelled())
        return makeError(ErrorCode::Cancelled, "cancelled");
    const AudioAsset& a = *rendered->audio;
    return trackBeatsInAudio(a.channelData(0), a.channelData(a.numChannels() > 1 ? 1 : 0), a.numFrames(), a.sampleRate);
}

} // namespace daw
