// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/session/NoteMidi.h>

#include <daw/media/Resampler.h>
#include <daw/session/BeatTimeline.h>
#include <daw/session/Renderer.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <utility>

namespace daw {

int generalMidiProgramFor(const std::string& trackName)
{
    std::string n = trackName;
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto has = [&](const char* word) { return n.find(word) != std::string::npos; };
    // General MIDI programs, counted from 0.
    if (has("bass"))
        return has("synth") ? 38 : 33; // Synth Bass 1, Electric Bass (finger)
    if (has("pad"))
        return 89; // Pad 2 (warm)
    if (has("string") || has("violin") || has("cello") || has("orchestra"))
        return 48; // String Ensemble 1
    if (has("organ"))
        return 16; // Drawbar Organ
    if (has("piano"))
        return has("electric") ? 4 : 0; // Electric Piano 1, Acoustic Grand Piano
    if (has("keys"))
        return 4;
    if (has("guitar"))
        return has("lead") || has("distort") ? 29 : has("acoustic") ? 25 : 27; // Overdriven, Acoustic (steel), Electric (clean)
    if (has("choir") || has("backing"))
        return 52; // Choir Aahs
    if (has("vocal") || has("voice") || has("vox"))
        return 53; // Voice Oohs
    if (has("brass") || has("horn") || has("trumpet"))
        return 61; // Brass Section
    if (has("flute"))
        return 73;
    if (has("synth") || has("lead"))
        return 81; // Lead 2 (sawtooth)
    return 0;
}

NoteDecodeParams noteDecodeParamsFor(const std::string& trackName)
{
    std::string n = trackName;
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto has = [&](const char* word) { return n.find(word) != std::string::npos; };
    NoteDecodeParams params;
    params.minNoteSeconds = 0.058;
    const bool sustained = has("vocal") || has("voice") || has("vox") || has("choir") || has("pad") || has("piano") ||
                           has("string") || has("organ");
    params.onsetThreshold = sustained && !has("bass") ? 0.6f : 0.4f;
    return params;
}

int noteVelocity(float amplitude) noexcept
{
    // Basic Pitch's mapping: velocity = 127 x mean activation.
    return static_cast<int>(std::clamp(std::lround(127.0 * amplitude), 1L, 127L));
}

MidiSong composeNoteMidi(const std::vector<PitchedNote>& notes, const BeatTrack& beats, int grid, int program,
                         const std::string& name)
{
    MidiSong song;
    song.name = name;
    const BeatTimeline timeline(beats);
    timeline.conduct(song);
    MidiTrackData part;
    part.name = name;
    part.channel = 0;
    part.program = std::clamp(program, 0, 127);
    // One note per key and start tick (the longest); a note quantised to no
    // length keeps one grid step.
    const int step = grid > 0 ? BeatTimeline::kTicksPerQuarter * 4 / grid : 1;
    std::map<std::pair<std::int64_t, int>, MidiNoteEvent> placed;
    for (const PitchedNote& note : notes) {
        const std::int64_t start = timeline.tick(note.start, grid);
        const std::int64_t end = std::max(start + step, timeline.tick(note.end, grid));
        MidiNoteEvent event{start, static_cast<int>(std::min<std::int64_t>(end - start, 1 << 30)), note.key, noteVelocity(note.amplitude)};
        auto [it, added] = placed.try_emplace({start, note.key}, event);
        if (!added && event.lengthTicks > it->second.lengthTicks)
            it->second = event;
    }
    // A note does not run into the next one on its key.
    std::map<int, MidiNoteEvent*> lastOnKey;
    for (auto& [at, event] : placed) {
        if (auto it = lastOnKey.find(event.key); it != lastOnKey.end()) {
            MidiNoteEvent& previous = *it->second;
            if (previous.tick + previous.lengthTicks > event.tick)
                previous.lengthTicks = static_cast<int>(std::max<std::int64_t>(1, event.tick - previous.tick));
        }
        lastOnKey[event.key] = &event;
    }
    part.notes.reserve(placed.size());
    for (const auto& [at, event] : placed)
        part.notes.push_back(event);
    song.tracks.push_back(std::move(part));
    return song;
}

Result<NoteMidiResult> notesToMidi(const AudioAsset& audio, const NoteTranscriber& transcriber, const NoteMidiOptions& options,
                                   const BeatTrack& beats, const std::string& name, const CancellationToken& cancel,
                                   const std::function<void(double)>& progress)
{
    if (audio.numChannels() < 1 || audio.numFrames() <= 0 || audio.sampleRate <= 0.0)
        return makeError(ErrorCode::InvalidArgument, "no audio to transcribe");
    // Mono, at the model's rate.
    AudioAsset mono;
    mono.sampleRate = audio.sampleRate;
    mono.channels.assign(1, std::vector<float>(static_cast<std::size_t>(audio.numFrames()), 0.0f));
    const float scale = 1.0f / static_cast<float>(audio.numChannels());
    for (const auto& channel : audio.channels)
        for (std::size_t i = 0; i < channel.size(); ++i)
            mono.channels[0][i] += scale * channel[i];
    const AudioAsset* input = &mono;
    AudioAssetPtr resampled;
    if (mono.sampleRate != NoteTranscriber::kSampleRate) {
        auto converted = resampleAudio(mono, NoteTranscriber::kSampleRate, {}, cancel);
        if (!converted)
            return converted.error();
        resampled = *converted;
        input = resampled.get();
    }
    auto activations = transcriber.analyse(input->channelData(0), static_cast<std::size_t>(input->numFrames()), cancel, progress);
    if (!activations)
        return activations.error();

    NoteMidiResult result;
    result.notes = decodeNotes(*activations, options.decode ? *options.decode : noteDecodeParamsFor(name));
    if (result.notes.empty())
        return makeError(ErrorCode::InvalidArgument, "no notes found");
    std::sort(result.notes.begin(), result.notes.end(), [](const PitchedNote& a, const PitchedNote& b) {
        return a.start != b.start ? a.start < b.start : a.key < b.key;
    });
    result.program = options.program >= 0 ? options.program : generalMidiProgramFor(name);
    result.onBeats = beats.beats.size() >= 2;
    result.song = composeNoteMidi(result.notes, beats, options.grid, result.program, name);
    return result;
}

Result<NoteMidiResult> trackNotesToMidi(const ProjectModel& model, const MediaPool& pool, const TrackId& trackId,
                                        const NoteTranscriber& transcriber, const NoteMidiOptions& options,
                                        const BeatTrack& beats, const CancellationToken& cancel,
                                        const std::function<void(double)>& progress)
{
    const Track* track = model.findTrack(trackId);
    if (track == nullptr)
        return makeError(ErrorCode::NotFound, "no such track");
    auto rendered = renderTrackAlone(model, pool, trackId, cancel);
    if (!rendered)
        return rendered.error();
    return notesToMidi(*rendered->audio, transcriber, options, beats, track->name, cancel, progress);
}

} // namespace daw
