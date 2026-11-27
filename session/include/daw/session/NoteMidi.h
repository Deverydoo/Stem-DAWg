// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Notes to MIDI (milestone 6): a pitched stem (bass, guitar, keys, synth,
// voice) becomes a MIDI file of its notes, transcribed by Basic Pitch
// (ai/NoteTranscriber).
//
// With the song's beats (tracked on its drum track), the file carries a tempo
// change on every beat like the drum file, so its bars fall on the song's
// bars and both files line up in a DAW. Without them it is a steady 120 BPM
// file with the notes as played. The instrument (General MIDI program) is
// guessed from the track's name.

#include <daw/ai/NoteTranscriber.h>
#include <daw/core/AudioAsset.h>
#include <daw/core/CancellationToken.h>
#include <daw/core/Result.h>
#include <daw/dsp/BeatTracker.h>
#include <daw/media/MidiFile.h>
#include <daw/project/ProjectModel.h>
#include <daw/session/MediaPool.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace daw {

/// A General MIDI program for a stem's name: "Bass" plays Electric Bass,
/// "Synth Pad" a pad, "String Section" strings... Acoustic Grand Piano when
/// the name says nothing.
int generalMidiProgramFor(const std::string& trackName);

/// Note decoding for a stem's name. Measured on Suno's MIDI parts rendered
/// through the General MIDI SoundFont: notes as short as a sixteenth at
/// 140 BPM (109 ms) need a minimum below Basic Pitch's 127.7 ms (58 ms);
/// busy lines (bass, guitars, synths) are found best from onsets of 0.4,
/// sustained and sung parts (voices, pads, piano, strings, organ) from 0.6,
/// where a lower threshold splits held notes.
NoteDecodeParams noteDecodeParamsFor(const std::string& trackName);

/// MIDI velocity from a note's mean activation (0..1).
int noteVelocity(float amplitude) noexcept;

struct NoteMidiOptions {
    int grid = 0;     // notes per whole note to quantise starts and ends to (16: sixteenths); 0 = as played
    int program = -1; // General MIDI program; -1 = from the track name
    std::optional<NoteDecodeParams> decode; // none = noteDecodeParamsFor(the track name)
};

/// The MIDI file for notes on a beat track (no beats: 120 BPM, as played).
MidiSong composeNoteMidi(const std::vector<PitchedNote>& notes, const BeatTrack& beats, int grid, int program,
                         const std::string& name);

struct NoteMidiResult {
    std::vector<PitchedNote> notes; // in time order
    MidiSong song;
    bool onBeats = false; // the file follows the song's beats
    int program = 0;
};

/// Transcribes the notes of `audio` (any rate and channels) and composes the
/// MIDI file. `progress` receives 0..1.
Result<NoteMidiResult> notesToMidi(const AudioAsset& audio, const NoteTranscriber& transcriber, const NoteMidiOptions& options,
                                   const BeatTrack& beats, const std::string& name, const CancellationToken& cancel = {},
                                   const std::function<void(double)>& progress = {});

/// The same for a track of a project, as it sounds (renderTrackAlone). The
/// project is not changed.
Result<NoteMidiResult> trackNotesToMidi(const ProjectModel& model, const MediaPool& pool, const TrackId& track,
                                        const NoteTranscriber& transcriber, const NoteMidiOptions& options,
                                        const BeatTrack& beats, const CancellationToken& cancel = {},
                                        const std::function<void(double)>& progress = {});

} // namespace daw
