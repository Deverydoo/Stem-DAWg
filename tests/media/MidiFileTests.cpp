// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

// Writing and parsing Standard MIDI Files: a written file reads back with
// the same notes, tempo map and time signatures; times stay exact through a
// tempo change on every beat (the loader used before 0.4 drifted a third of
// a second over a Suno export); running status, SMPTE time and broken files.

#include "TempDir.h"

#include <daw/media/Midi.h>
#include <daw/media/MidiFile.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>

using namespace daw;
using namespace daw::test;
using Catch::Approx;

namespace {

/// A drum part with a tempo change on every beat, like a Suno MIDI export.
MidiSong driftingSong(int beats)
{
    MidiSong song;
    song.name = "drift";
    song.timeSignatures.push_back({0, 4, 4});
    MidiTrackData drums{"Drums", kGmDrumChannel, {}};
    for (int b = 0; b < beats; ++b) {
        song.tempo.push_back({static_cast<std::int64_t>(b) * 480, b % 2 == 0 ? 135.1 : 138.3});
        drums.notes.push_back({static_cast<std::int64_t>(b) * 480, 60, b % 4 == 0 ? 36 : 42, 100});
    }
    song.tracks.push_back(drums);
    return song;
}

/// Where beat `beat` falls, with the tempo as the file stores it: a whole
/// number of microseconds per quarter note.
double beatTime(int beat)
{
    double t = 0.0;
    for (int b = 0; b < beat; ++b)
        t += std::round(60'000'000.0 / (b % 2 == 0 ? 135.1 : 138.3)) / 1e6;
    return t;
}

} // namespace

TEST_CASE("A written MIDI file reads back with its notes, tempo and channels", "[midi][midifile]")
{
    TempDir dir("midifile");
    MidiSong song = driftingSong(8);
    song.tracks.push_back({"Bass", 1, {{240, 120, 40, 90}, {960, 120, 43, 70}}});
    REQUIRE(writeMidiFile(dir / "song.mid", song).ok());

    const auto read = readMidiFile(dir / "song.mid");
    REQUIRE(read.ok());
    REQUIRE(read->notes.size() == 10);
    REQUIRE(read->tempo.size() == 8);
    CHECK(read->tempo[1].bpm == Approx(138.3).margin(0.01));
    CHECK(read->notes.front().channel == kGmDrumChannel);
    CHECK(read->notes.front().key == 36);
    CHECK(read->notes.front().velocity == 100);
    // The bass note at tick 240 sits half a beat in, at the first tempo.
    bool found = false;
    for (const MidiNote& n : read->notes)
        if (n.channel == 1 && n.key == 40) {
            found = true;
            CHECK(n.seconds == Approx(0.5 * 60.0 / 135.1).margin(1e-6));
            CHECK(n.velocity == 90);
        }
    CHECK(found);

    // The playback probe agrees.
    const auto info = probeMidiFile(dir / "song.mid");
    REQUIRE(info.ok());
    CHECK(info->noteCount == 10);
    CHECK(info->usesDrums);
    CHECK(info->channelCount == 2);
}

TEST_CASE("MIDI times stay exact through a tempo change on every beat", "[midi][midifile]")
{
    constexpr int kBeats = 1000;
    const std::string bytes = encodeMidiFile(driftingSong(kBeats));
    const auto parsed = parseMidiFile(bytes);
    REQUIRE(parsed.ok());
    REQUIRE(parsed->tempo.size() == kBeats);
    double worst = 0.0;
    int beat = 0;
    for (const MidiEvent& e : parsed->events)
        if ((e.status & 0xF0) == 0x90)
            worst = std::max(worst, std::abs(e.seconds - beatTime(beat++)));
    REQUIRE(beat == kBeats);
    // Rounding to the millisecond at every change would put the last beats
    // about half a second early.
    CHECK(worst < 1e-6);
    CHECK(parsed->quarterNotes == Approx(kBeats - 1 + 60.0 / 480.0).margin(1e-9));
}

TEST_CASE("The MIDI parser reads running status, note-on as note-off, and SMPTE time", "[midi][midifile]")
{
    const auto header = [](unsigned division) {
        return std::string("MThd\0\0\0\x06\0\0\0\x01", 12) + std::string{static_cast<char>(division >> 8), static_cast<char>(division & 0xFF)};
    };
    const auto chunk = [](const std::string& body) {
        const auto n = static_cast<std::uint32_t>(body.size());
        return std::string("MTrk") + std::string{static_cast<char>(n >> 24), static_cast<char>(n >> 16), static_cast<char>(n >> 8), static_cast<char>(n)}
               + body;
    };
    // 96 ticks per quarter at the default 120 BPM: a note-on, then (running
    // status) a note-on with velocity 0 a quarter later and another note.
    const std::string body = std::string("\x00\x90\x3C\x64", 4) + std::string("\x60\x3C\x00", 3) + std::string("\x00\x3E\x50", 3)
                             + std::string("\x00\xFF\x2F\x00", 4);
    const auto metrical = parseMidiFile(header(96) + chunk(body));
    REQUIRE(metrical.ok());
    REQUIRE(metrical->events.size() == 3);
    CHECK(metrical->events[1].seconds == Approx(0.5));
    CHECK(metrical->events[1].status == 0x90);
    CHECK(metrical->events[1].data2 == 0); // a note-off by velocity 0
    CHECK(metrical->events[2].data1 == 0x3E);

    // SMPTE: 25 frames per second, 40 ticks per frame = 1 ms per tick.
    const auto smpte = parseMidiFile(header(0xE728) + chunk(body));
    REQUIRE(smpte.ok());
    CHECK(smpte->events[1].seconds == Approx(0.096));
}

TEST_CASE("Broken MIDI files are refused or read as far as they go", "[midi][midifile]")
{
    CHECK_FALSE(parseMidiFile("").ok());
    CHECK_FALSE(parseMidiFile("RIFF....WAVEfmt ").ok());
    CHECK_FALSE(parseMidiFile(std::string("MThd\0\0\0\x06\0\0\0\x01\0\0", 14)).ok()); // division 0

    // Truncated in the middle of a track: what was read is kept.
    std::string bytes = encodeMidiFile(driftingSong(16));
    bytes.resize(bytes.size() - 40);
    const auto cut = parseMidiFile(bytes);
    REQUIRE(cut.ok());
    CHECK(cut->events.size() < 32);

    // Garbage after a valid header and track header never reads out of bounds.
    std::string garbage = std::string("MThd\0\0\0\x06\0\x01\0\x01\x01\xE0", 14) + "MTrk" + std::string("\x7F\xFF\xFF\xFF", 4);
    for (int i = 0; i < 300; ++i)
        garbage.push_back(static_cast<char>((i * 73 + 11) & 0xFF));
    (void)parseMidiFile(garbage); // must not crash
}
