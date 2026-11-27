// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

// Notes to MIDI: a pitched track becomes a MIDI file whose notes, read back
// through its tempo map, land on the notes of the audio, on the song's beats
// or at a steady 120 BPM, with the instrument named by the track; how the
// file is put together (one note per key and start, no note running into the
// next); and the drum track the song's beats come from.

#include <daw/media/MidiFile.h>
#include <daw/session/BeatTimeline.h>
#include <daw/session/DrumMidi.h>
#include <daw/session/NoteMidi.h>
#include <daw/session/ProjectOperations.h>

#include "SyntheticNotes.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

using namespace daw;
using namespace daw::test;
using Catch::Approx;

namespace {

struct Fixture {
    ProjectModel model = makeEmptyProject("notes");
    MediaPool pool;
    TrackId track;

    Fixture(const AudioAssetPtr& audio, const std::string& name)
    {
        model.sampleRate = audio->sampleRate;
        MediaRef ref{MediaId::generate(), name, "part.wav", MediaStorage::Copied, audio->sampleRate, audio->numChannels(),
                     audio->numFrames(), 1};
        model.media.push_back(ref);
        track = addMediaAsNewTrack(model, ref, 0);
        model.findTrack(track)->name = name;
        LoadedMedia loaded;
        loaded.id = ref.id;
        loaded.original = audio;
        pool.put(loaded);
    }
};

struct ReadBack {
    std::vector<MidiEvent> notes; // note-ons
    std::vector<MidiEvent> programs;
    std::vector<MidiTempoPoint> tempo;
};

ReadBack readBack(const MidiSong& song)
{
    auto parsed = parseMidiFile(encodeMidiFile(song));
    REQUIRE(parsed.ok());
    ReadBack r;
    for (const MidiEvent& e : parsed->events) {
        if ((e.status & 0xF0) == 0x90 && e.data2 > 0)
            r.notes.push_back(e);
        if ((e.status & 0xF0) == 0xC0)
            r.programs.push_back(e);
    }
    r.tempo = parsed->tempo;
    return r;
}

/// Share of the true notes with a note of the same key starting within `tolerance`.
double coverage(const std::vector<TestNote>& truth, const ReadBack& r, double tolerance)
{
    int found = 0;
    for (const TestNote& n : truth)
        found += std::any_of(r.notes.begin(), r.notes.end(),
                             [&](const MidiEvent& e) { return e.data1 == n.key && std::abs(e.seconds - n.start) <= tolerance; });
    return static_cast<double>(found) / static_cast<double>(truth.size());
}

BeatTrack beatsOf(const NotePiece& piece, double bpm)
{
    BeatTrack beats;
    beats.beats = piece.beats;
    beats.bpm = bpm;
    for (std::size_t i = 0; i < piece.beats.size(); i += 4)
        beats.downbeats.push_back(i);
    return beats;
}

} // namespace

TEST_CASE("Instruments and note decoding follow the track name", "[notes]")
{
    CHECK(generalMidiProgramFor("Bass") == 33);
    CHECK(generalMidiProgramFor("Synth Bass") == 38);
    CHECK(generalMidiProgramFor("Bass Guitar") == 33);
    CHECK(generalMidiProgramFor("Synth Pad") == 89);
    CHECK(generalMidiProgramFor("String Section") == 48);
    CHECK(generalMidiProgramFor("Electric Piano") == 4);
    CHECK(generalMidiProgramFor("Lead Electric Guitar") == 29);
    CHECK(generalMidiProgramFor("Rhythm Electric Guitar") == 27);
    CHECK(generalMidiProgramFor("Backing Vocals") == 52);
    CHECK(generalMidiProgramFor("Lead Vocal") == 53);
    CHECK(generalMidiProgramFor("Synth") == 81);
    CHECK(generalMidiProgramFor("Track 7") == 0);

    CHECK(noteDecodeParamsFor("Bass").onsetThreshold == 0.4f);
    CHECK(noteDecodeParamsFor("Rhythm Electric Guitar").onsetThreshold == 0.4f);
    CHECK(noteDecodeParamsFor("Lead Vocal").onsetThreshold == 0.6f);
    CHECK(noteDecodeParamsFor("Synth Pad").onsetThreshold == 0.6f);
    CHECK(noteDecodeParamsFor("Lead Vocal").minNoteSeconds == Approx(0.058));
}

TEST_CASE("A note file keeps one note per key and start, and no note runs into the next on its key", "[notes]")
{
    const std::vector<PitchedNote> notes = {
        {0.50, 1.40, 40, 0.8f}, // runs into the next 40
        {1.00, 1.50, 40, 0.5f},
        {2.00, 2.20, 45, 0.4f}, // the same start and key twice: the longer stays
        {2.00, 2.60, 45, 0.6f},
    };
    const MidiSong song = composeNoteMidi(notes, BeatTrack{}, 0, 33, "Bass");
    REQUIRE(song.tracks.size() == 1);
    const MidiTrackData& part = song.tracks.front();
    CHECK(part.program == 33);
    CHECK(part.name == "Bass");
    REQUIRE(part.notes.size() == 3);
    // A steady 120 BPM: a quarter note (480 ticks) is half a second.
    CHECK(part.notes[0].tick == 480);
    CHECK(part.notes[0].lengthTicks == 480); // cut where the next 40 starts
    CHECK(part.notes[1].tick == 960);
    CHECK(part.notes[1].lengthTicks == 480);
    CHECK(part.notes[2].tick == 1920);
    CHECK(part.notes[2].lengthTicks == 576);
    CHECK(part.notes[2].velocity == noteVelocity(0.6f));
    REQUIRE(song.tempo.size() == 1);
    CHECK(song.tempo.front().bpm == 120.0);

    // Quantised to sixteenths, a note never shrinks to nothing.
    const MidiSong grid = composeNoteMidi({{0.51, 0.53, 50, 0.5f}}, BeatTrack{}, 16, 0, "Keys");
    CHECK(grid.tracks.front().notes.front().tick == 480);
    CHECK(grid.tracks.front().notes.front().lengthTicks == 120);
}

TEST_CASE("The song's beats come from its drum track, not a MIDI track made from it", "[notes]")
{
    ProjectModel model = makeEmptyProject("song");
    const auto add = [&](const std::string& name, const std::string& path) {
        MediaRef ref{MediaId::generate(), name, path, MediaStorage::Copied, 48000.0, 2, 48000, 1};
        model.media.push_back(ref);
        const TrackId id = addMediaAsNewTrack(model, ref, 0);
        model.findTrack(id)->name = name;
        return id;
    };
    CHECK_FALSE(findDrumTrack(model).has_value());
    add("Bass", "bass.wav");
    add("Drums (drums)", "Exports/Drums (drums).mid");
    const TrackId drums = add("2 Drums", "drums.wav");
    REQUIRE(findDrumTrack(model).has_value());
    CHECK(*findDrumTrack(model) == drums);
}

#if DAW_HAS_ONNX

namespace {

std::shared_ptr<NoteTranscriber> loadTranscriber()
{
    auto backend = createOnnxBackend();
    REQUIRE(backend.ok());
    auto transcriber = NoteTranscriber::load(*backend);
    REQUIRE(transcriber.ok());
    return *transcriber;
}

} // namespace

TEST_CASE("A bass track becomes a MIDI file on the song's beats", "[notes][onnx]")
{
    const NotePiece piece = makeNotePiece(8, 124.0, false);
    Fixture f(piece.audio, "Bass");
    const ProjectModel before = f.model;
    const auto transcriber = loadTranscriber();
    const BeatTrack beats = beatsOf(piece, 124.0);

    auto result = trackNotesToMidi(f.model, f.pool, f.track, *transcriber, {}, beats);
    REQUIRE(result.ok());
    CHECK(f.model == before);
    CHECK(result->onBeats);
    CHECK(result->program == 33);
    CHECK(std::is_sorted(result->notes.begin(), result->notes.end(),
                         [](const PitchedNote& a, const PitchedNote& b) { return a.start < b.start; }));

    const ReadBack r = readBack(result->song);
    INFO(r.notes.size() << " notes for " << piece.notes.size());
    CHECK(coverage(piece.notes, r, 0.03) >= 0.9);
    REQUIRE(r.programs.size() == 1);
    CHECK(r.programs.front().data1 == 33);
    // A tempo change per beat: the lead-in's, then every beat but the last.
    CHECK(r.tempo.size() == beats.beats.size());
    CHECK(r.tempo.back().bpm == Approx(124.0).epsilon(0.001));

    // Quantised to sixteenths: every note on a sixteenth of the beat grid,
    // still on the audio (a sixteenth is 121 ms here).
    NoteMidiOptions grid;
    grid.grid = 16;
    auto quantised = trackNotesToMidi(f.model, f.pool, f.track, *transcriber, grid, beats);
    REQUIRE(quantised.ok());
    for (const MidiNoteEvent& n : quantised->song.tracks.front().notes)
        CHECK(n.tick % (BeatTimeline::kTicksPerQuarter / 4) == 0);
    CHECK(coverage(piece.notes, readBack(quantised->song), 0.03) >= 0.9);
}

TEST_CASE("Without the song's beats, a note file is a steady 120 BPM with the notes as played", "[notes][onnx]")
{
    const NotePiece piece = makeNotePiece(4, 100.0, true);
    Fixture f(piece.audio, "Electric Piano");
    const auto transcriber = loadTranscriber();
    auto result = trackNotesToMidi(f.model, f.pool, f.track, *transcriber, {}, BeatTrack{});
    REQUIRE(result.ok());
    CHECK_FALSE(result->onBeats);
    CHECK(result->program == 4);
    const ReadBack r = readBack(result->song);
    REQUIRE(r.tempo.size() == 1);
    CHECK(r.tempo.front().bpm == 120.0);
    CHECK(coverage(piece.notes, r, 0.03) >= 0.8);

    // A silent track has no notes to write.
    auto silent = std::make_shared<AudioAsset>();
    silent->sampleRate = 48000.0;
    silent->channels.assign(2, std::vector<float>(48000 * 3, 0.0f));
    Fixture quiet(silent, "Synth");
    CHECK_FALSE(trackNotesToMidi(quiet.model, quiet.pool, quiet.track, *transcriber, {}, BeatTrack{}).ok());
}

#endif
