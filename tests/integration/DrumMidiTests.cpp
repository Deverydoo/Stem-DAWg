// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

// Drums to MIDI: a drum track becomes a General MIDI file whose notes, read
// back through its tempo map, land on the hits of the audio (the tempo
// drifts, the file follows it), quantised to the grid within each beat, with
// bar lines on the downbeats; the project is not changed.

#include <daw/media/MidiFile.h>
#include <daw/session/DrumMidi.h>
#include <daw/session/ProjectOperations.h>

#include "SyntheticDrums.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace daw;
using namespace daw::test;
using Catch::Approx;

namespace {

struct Fixture {
    ProjectModel model = makeEmptyProject("drums");
    MediaPool pool;
    TrackId track;

    explicit Fixture(const AudioAssetPtr& audio)
    {
        model.sampleRate = audio->sampleRate;
        MediaRef ref{MediaId::generate(), "Drums", "drums.wav", MediaStorage::Copied, audio->sampleRate, audio->numChannels(),
                     audio->numFrames(), 1};
        model.media.push_back(ref);
        track = addMediaAsNewTrack(model, ref, 0);
        model.findTrack(track)->name = "Drums";
        LoadedMedia loaded;
        loaded.id = ref.id;
        loaded.original = audio;
        pool.put(loaded);
    }
};

/// Reads a song back through its own tempo map.
MidiContents readBack(const MidiSong& song)
{
    auto parsed = parseMidiFile(encodeMidiFile(song));
    REQUIRE(parsed.ok());
    MidiContents c;
    for (const MidiEvent& e : parsed->events)
        if ((e.status & 0xF0) == 0x90 && e.data2 > 0)
            c.notes.push_back({e.seconds, e.status & 0x0F, e.data1, e.data2});
    c.tempo = parsed->tempo;
    return c;
}

/// Share of the groove's hits with a note of the right key within `tolerance`.
double coverage(const DrumGroove& g, const MidiContents& c, double tolerance, double* worst = nullptr)
{
    int matched = 0;
    double w = 0.0;
    for (const TestHit& h : g.hits) {
        const int key = generalMidiDrumKey(static_cast<DrumKind>(h.code));
        // Open and closed hats count as hats.
        const bool hat = h.code == kTestClosedHat || h.code == kTestOpenHat;
        double best = 1.0;
        for (const MidiNote& n : c.notes) {
            const bool same = hat ? (n.key == 42 || n.key == 46) : n.key == key;
            if (same && std::abs(n.seconds - h.seconds) < std::abs(best))
                best = n.seconds - h.seconds;
        }
        if (std::abs(best) <= tolerance) {
            ++matched;
            w = std::max(w, std::abs(best));
        }
    }
    if (worst != nullptr)
        *worst = w;
    return static_cast<double>(matched) / static_cast<double>(g.hits.size());
}

} // namespace

TEST_CASE("Drums to MIDI lands the notes on the hits through a drifting tempo", "[session][drums][midi]")
{
    const DrumGroove g = makeDrumGroove(24, 120.0, 126.0);
    Fixture f(g.audio);
    const ProjectModel before = f.model;

    DrumMidiOptions options;
    options.grid = 16;
    const auto result = trackDrumsToMidi(f.model, f.pool, f.track, options);
    REQUIRE(result.ok());
    CHECK(f.model == before);
    CHECK(result->song.tracks.size() == 1);
    CHECK(result->song.tracks.front().channel == kGmDrumChannel);
    CHECK(result->song.name == "Drums");

    // A tempo change on every beat (and one for the lead-in), following the drift.
    const MidiContents back = readBack(result->song);
    CHECK(back.tempo.size() >= result->beats.beats.size() - 1);
    CHECK(back.tempo.front().bpm > 100.0);
    CHECK(back.tempo[back.tempo.size() / 4].bpm < back.tempo[3 * back.tempo.size() / 4].bpm);

    // The notes land on the hits: the groove plays on the eighths, so the
    // sixteenth grid keeps them where they were.
    double worst = 0.0;
    const double covered = coverage(g, back, 0.02, &worst);
    INFO("covered " << covered << ", worst " << worst * 1000.0 << " ms");
    CHECK(covered >= 0.85);
    CHECK(worst <= 0.02);

    // Bars: the first bar line at the first downbeat (a pickup bar before it).
    REQUIRE_FALSE(result->song.timeSignatures.empty());
    const auto& signatures = result->song.timeSignatures;
    CHECK(signatures.back().numerator == 4);
}

TEST_CASE("Drum MIDI quantises within each beat, and keeps the played timing when asked", "[session][drums][midi]")
{
    // Beats a second apart from 2 s; hits slightly off the grid.
    BeatTrack track;
    for (int i = 0; i < 16; ++i)
        track.beats.push_back(2.0 + i);
    track.downbeats = {0, 4, 8, 12};
    const std::vector<DrumHit> hits{
        {2.010, DrumKind::Kick, 1.0f},     // just after beat 0
        {3.240, DrumKind::Snare, 1.0f},    // a sixteenth after beat 1, a little early
        {4.490, DrumKind::ClosedHat, 0.25f}, // the "and" of beat 2, a little early
        {0.500, DrumKind::Crash, 1.0f},    // in the lead-in
    };
    const MidiSong song = composeDrumMidi(hits, track, 16, "q");
    // Lead-in: 2 s before the first beat at 1 s per beat = 2 beats.
    REQUIRE_FALSE(song.tempo.empty());
    CHECK(song.tempo.front().tick == 0);
    CHECK(song.tempo.front().bpm == Approx(60.0));
    // Bars: the first downbeat (beat 0) is 2 beats in: a 2/4 pickup, then 4/4.
    REQUIRE(song.timeSignatures.size() == 2);
    CHECK(song.timeSignatures[0].numerator == 2);
    CHECK(song.timeSignatures[1].tick == 2 * 480);
    CHECK(song.timeSignatures[1].numerator == 4);

    const auto& notes = song.tracks.front().notes;
    REQUIRE(notes.size() == 4);
    const auto tickOf = [&](int key) {
        for (const MidiNoteEvent& n : notes)
            if (n.key == key)
                return n.tick;
        return std::int64_t{-1};
    };
    CHECK(tickOf(36) == 2 * 480);            // beat 0
    CHECK(tickOf(38) == 3 * 480 + 120);      // beat 1 plus a sixteenth
    CHECK(tickOf(42) == 4 * 480 + 240);      // beat 2 plus an eighth
    CHECK(tickOf(49) == 240);                // 0.5 s into the 1-beat-per-second lead-in
    // Velocity follows strength: a quarter as strong is clearly softer.
    for (const MidiNoteEvent& n : notes)
        CHECK(n.velocity == (n.key == 42 ? drumVelocity(0.25f) : drumVelocity(1.0f)));

    // As played: the exact positions.
    const MidiSong played = composeDrumMidi(hits, track, 0, "p");
    for (const MidiNoteEvent& n : played.tracks.front().notes)
        if (n.key == 38)
            CHECK(n.tick == std::llround(3.24 * 480.0)); // 2 lead-in beats + beat 1 + 0.24
}

TEST_CASE("Drums to MIDI refuses a track without rhythm", "[session][drums][midi]")
{
    auto silence = AudioAsset::create(2, 48000 * 3, 48000.0);
    Fixture f(silence);
    const auto result = trackDrumsToMidi(f.model, f.pool, f.track, {});
    CHECK_FALSE(result.ok());
    CHECK_FALSE(trackDrumsToMidi(f.model, f.pool, TrackId::generate(), {}).ok());
}
