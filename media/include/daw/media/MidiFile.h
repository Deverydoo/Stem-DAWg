// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Writing Standard MIDI Files (type 1), and reading their notes and tempo
// changes back with times in seconds (milestone 6: audio to MIDI).
//
// The first track carries the song name, the tempo map and the time
// signatures; each further track carries one part's notes.

#include <daw/core/Result.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace daw {

inline constexpr int kGmDrumChannel = 9; // MIDI channel 10

struct MidiNoteEvent {
    std::int64_t tick = 0;
    int lengthTicks = 60;
    int key = 60;
    int velocity = 100; // 1..127
};

struct MidiTrackData {
    std::string name;
    int channel = 0; // 0..15
    std::vector<MidiNoteEvent> notes;
    int program = -1; // General MIDI program 0..127 set at the start; -1: none
};

struct MidiTempoEvent {
    std::int64_t tick = 0;
    double bpm = 120.0; // quarter notes per minute
};

struct MidiTimeSignature {
    std::int64_t tick = 0;
    int numerator = 4;
    int denominator = 4; // a power of two
};

struct MidiSong {
    int ticksPerQuarter = 480;
    std::string name;
    std::vector<MidiTempoEvent> tempo;               // in tick order; none = 120 BPM
    std::vector<MidiTimeSignature> timeSignatures;   // in tick order; none = 4/4
    std::vector<MidiTrackData> tracks;
};

/// The file's bytes.
std::string encodeMidiFile(const MidiSong& song);

/// Writes the file atomically.
Status writeMidiFile(const std::filesystem::path& path, const MidiSong& song);

struct MidiNote {
    double seconds = 0.0; // resolved to the millisecond
    int channel = 0;
    int key = 0;
    int velocity = 0;
};

struct MidiTempoPoint {
    double seconds = 0.0;
    double bpm = 120.0;
};

struct MidiContents {
    std::vector<MidiNote> notes;          // note-ons, in time order
    std::vector<MidiTempoPoint> tempo;    // tempo changes, in time order
};

Result<MidiContents> readMidiFile(const std::filesystem::path& path);

/// A channel message (status 0x80..0xEF) at its time in seconds.
struct MidiEvent {
    double seconds = 0.0;
    std::uint8_t status = 0;
    std::uint8_t data1 = 0;
    std::uint8_t data2 = 0;
};

struct ParsedMidi {
    std::vector<MidiEvent> events;       // every track merged, in time order
    std::vector<MidiTempoPoint> tempo;
    double lastEventSeconds = 0.0;       // any event, meta events included
    double quarterNotes = 0.0;           // length in quarter notes (for the average tempo)
};

/// Parses a Standard MIDI File (formats 0-2, metrical or SMPTE time).
/// Times are computed in double precision from the tempo map: a loader that
/// rounds to the millisecond at every tempo change drifts by a third of a
/// second over a Suno MIDI export, which changes tempo on every beat.
Result<ParsedMidi> parseMidiFile(std::string_view bytes);

} // namespace daw
