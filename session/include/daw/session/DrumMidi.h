// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Drums to MIDI (milestone 6): a drum stem becomes a General MIDI drum file
// that lines up with the song.
//
// The hits (dsp/DrumTranscriber) are placed on the song's beats
// (dsp/BeatTracker) and quantised to a grid within each beat. The file
// carries a tempo change on every beat, so although AI songs drift in tempo
// the notes stay on the audio, and a DAW's bar lines fall on the song's
// bars: the time before the first downbeat becomes a lead-in and a pickup
// bar.

#include <daw/core/AudioAsset.h>
#include <daw/core/CancellationToken.h>
#include <daw/core/Result.h>
#include <daw/dsp/BeatTracker.h>
#include <daw/dsp/DrumTranscriber.h>
#include <daw/media/MidiFile.h>
#include <daw/project/ProjectModel.h>
#include <daw/session/MediaPool.h>

#include <optional>
#include <string>

namespace daw {

/// General MIDI drum keys (channel 10).
int generalMidiDrumKey(DrumKind kind) noexcept;

/// MIDI velocity from a hit's strength (1 = the instrument's typical loud hit).
int drumVelocity(float strength) noexcept;

struct DrumMidiOptions {
    int grid = 16;            // notes per whole note to quantise to: 8, 16 or 32; 0 = keep the played timing
    float sensitivity = 1.0f; // > 1 finds quieter hits
};

/// The MIDI file for hits on a beat track.
MidiSong composeDrumMidi(const std::vector<DrumHit>& hits, const BeatTrack& beats, int grid, const std::string& name);

struct DrumMidiResult {
    DrumTranscription transcription;
    BeatTrack beats;
    MidiSong song;
    int notes = 0;
};

/// Transcribes a drum stem and composes its MIDI file. Fails when the audio
/// holds too little rhythm to find beats in.
Result<DrumMidiResult> drumsToMidi(const AudioAsset& audio, const DrumMidiOptions& options, const std::string& name,
                                   const CancellationToken& cancel = {});

/// The same for a track of a project, as it sounds (its clean-up and
/// processed copies included) at unity gain, from project time 0, so the
/// file lines up with the song. The project is not changed.
Result<DrumMidiResult> trackDrumsToMidi(const ProjectModel& model, const MediaPool& pool, const TrackId& track,
                                        const DrumMidiOptions& options, const CancellationToken& cancel = {});

/// The first track named like a drum stem ("Drums", "Drum Kit"...), if any.
std::optional<TrackId> findDrumTrack(const ProjectModel& model);

/// The song's beats, tracked on its drum track (findDrumTrack). Fails when
/// the project has no drum track or it holds too little rhythm.
Result<BeatTrack> songBeats(const ProjectModel& model, const MediaPool& pool, const CancellationToken& cancel = {});

} // namespace daw
