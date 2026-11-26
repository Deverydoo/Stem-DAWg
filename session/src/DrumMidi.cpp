// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/session/DrumMidi.h>

#include <daw/session/Renderer.h>

#include <algorithm>
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
    constexpr int ppq = 480;
    MidiSong song;
    song.ticksPerQuarter = ppq;
    song.name = name;
    MidiTrackData drums;
    drums.name = "Drums";
    drums.channel = kGmDrumChannel;
    const std::vector<double>& b = track.beats;
    if (b.size() < 2) {
        song.tracks.push_back(std::move(drums));
        return song;
    }

    // The lead-in: from time 0 to the anchor beat, a whole number of beats
    // (at least one) at its own tempo. A first beat too close to the start
    // for a beat of its own falls inside the lead-in.
    const std::size_t m = b.size();
    const std::size_t k = b[0] >= 0.75 * (b[1] - b[0]) || m < 3 ? 0 : 1;
    const double anchor = b[k];
    const double anchorPeriod = b[k + 1] - b[k];
    const int leadBeats = std::max(1, static_cast<int>(std::lround(anchor / anchorPeriod)));

    // Position in beats from time 0 (fractional).
    const auto position = [&](double t) {
        if (t < anchor)
            return anchor > 0.0 ? t / anchor * leadBeats : 0.0;
        const auto it = std::upper_bound(b.begin() + static_cast<std::ptrdiff_t>(k), b.end(), t);
        std::size_t j = static_cast<std::size_t>(it - b.begin()) - 1; // b[j] <= t
        j = std::min(j, m - 2);                                       // past the last beat: its interval continues
        return leadBeats + static_cast<double>(j - k) + (t - b[j]) / (b[j + 1] - b[j]);
    };

    // Tempo: the lead-in's, then one per beat.
    if (anchor > 0.0)
        song.tempo.push_back({0, 60.0 * leadBeats / anchor});
    for (std::size_t j = k; j + 1 < m; ++j)
        song.tempo.push_back({static_cast<std::int64_t>(leadBeats + static_cast<int>(j - k)) * ppq, 60.0 / (b[j + 1] - b[j])});

    // Bars: each downbeat starts a bar. The beats before the first make a
    // pickup bar of their own length; a bar a beat short or long (across a
    // break) gets its own time signature.
    const auto beatTick = [&](std::size_t j) { return static_cast<std::int64_t>(leadBeats + static_cast<int>(j) - static_cast<int>(k)) * ppq; };
    std::vector<std::int64_t> barTicks;
    for (std::size_t d : track.downbeats)
        if (d >= k)
            barTicks.push_back(beatTick(d));
    if (barTicks.empty())
        barTicks.push_back(0);
    const auto signature = [&](std::int64_t at, std::int64_t length) {
        const int beatsInBar = static_cast<int>(std::max<std::int64_t>(1, length / ppq));
        if (song.timeSignatures.empty() || song.timeSignatures.back().numerator != beatsInBar)
            song.timeSignatures.push_back({at, beatsInBar, 4});
    };
    if (barTicks.front() > 0) {
        // The pickup: whole bars of the regular length, then the remainder first.
        const std::int64_t pickup = barTicks.front() % (static_cast<std::int64_t>(track.beatsPerBar) * ppq);
        if (pickup != 0)
            signature(0, pickup);
        signature(pickup, static_cast<std::int64_t>(track.beatsPerBar) * ppq);
    }
    for (std::size_t i = 0; i < barTicks.size(); ++i) {
        const std::int64_t length = i + 1 < barTicks.size() ? barTicks[i + 1] - barTicks[i] : static_cast<std::int64_t>(track.beatsPerBar) * ppq;
        signature(barTicks[i], length);
    }

    // Notes, quantised within the beat; one per key and tick (the loudest).
    const int perBeat = grid > 0 ? std::max(1, grid / 4) : 0;
    std::map<std::pair<std::int64_t, int>, int> notes;
    for (const DrumHit& h : hits) {
        double pos = position(h.seconds);
        if (perBeat > 0)
            pos = std::round(pos * perBeat) / perBeat;
        const auto tick = static_cast<std::int64_t>(std::llround(std::max(0.0, pos) * ppq));
        int& velocity = notes[{tick, generalMidiDrumKey(h.kind)}];
        velocity = std::max(velocity, drumVelocity(h.strength));
    }
    for (const auto& [at, velocity] : notes)
        drums.notes.push_back({at.first, ppq / 8, at.second, velocity});
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
    // Only this track, heard plainly: unity gain, centred, no master bus.
    ProjectModel single = model;
    single.tracks = {*track};
    Track& only = single.tracks.front();
    only.gainDb = 0.0;
    only.pan = 0.0;
    only.muted = false;
    only.soloed = false;
    single.masterGainDb = 0.0;
    single.master = MasterSettings{};
    auto rendered = renderMix(single, pool, {}, cancel);
    if (!rendered)
        return rendered.error();
    return drumsToMidi(*rendered->audio, options, track->name, cancel);
}

} // namespace daw
