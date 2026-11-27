// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

// Note transcription: Basic Pitch's decoding on hand-made activations (notes
// from onsets, repeated notes split at their onsets, the melodia pass, the
// minimum length and key range), and the compiled-in model on a synthesised
// bass line and chords whose notes are known.

#include <daw/ai/NoteTranscriber.h>

#include "SyntheticNotes.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

using namespace daw;
using namespace daw::test;

namespace {

/// Activations of `frames` frames, all zero, one frame per kHop samples.
NoteActivations blank(int frames)
{
    NoteActivations a;
    a.frames.assign(static_cast<std::size_t>(frames) * kPianoKeys, 0.0f);
    a.onsets = a.frames;
    for (int t = 0; t < frames; ++t)
        a.times.push_back(t / NoteTranscriber::kFrameRate);
    return a;
}

void sound(NoteActivations& a, int key, int from, int to, float level = 0.8f)
{
    for (int t = from; t < to; ++t)
        a.frames[static_cast<std::size_t>(t) * kPianoKeys + static_cast<std::size_t>(key - kPianoLowestKey)] = level;
}

void start(NoteActivations& a, int key, int at, float level = 0.9f)
{
    a.onsets[static_cast<std::size_t>(at) * kPianoKeys + static_cast<std::size_t>(key - kPianoLowestKey)] = level;
}

std::vector<PitchedNote> sorted(std::vector<PitchedNote> notes)
{
    std::sort(notes.begin(), notes.end(), [](const PitchedNote& a, const PitchedNote& b) { return a.start < b.start; });
    return notes;
}

double frameTime(int t)
{
    return t / NoteTranscriber::kFrameRate;
}

} // namespace

TEST_CASE("Notes start at onset peaks and last while the key sounds", "[ai][notes]")
{
    NoteActivations a = blank(200);
    sound(a, 45, 20, 60);
    start(a, 45, 20);
    // A repeated note: one long activation, two onsets.
    sound(a, 52, 80, 140);
    start(a, 52, 80);
    start(a, 52, 110);

    NoteDecodeParams params;
    params.inferOnsets = false;
    params.melodia = false;
    const auto notes = sorted(decodeNotes(a, params));
    REQUIRE(notes.size() == 3);
    CHECK(notes[0].key == 45);
    CHECK(notes[0].start == frameTime(20));
    CHECK(notes[0].end == frameTime(60));
    CHECK(std::abs(notes[0].amplitude - 0.8f) < 1e-6f);
    CHECK(notes[1].key == 52);
    CHECK(notes[1].start == frameTime(80));
    CHECK(notes[1].end == frameTime(110)); // ends where the repeat starts
    CHECK(notes[2].start == frameTime(110));
    CHECK(notes[2].end == frameTime(140));
}

TEST_CASE("The melodia pass adds notes without a clear onset; short notes and keys out of range are dropped", "[ai][notes]")
{
    NoteActivations a = blank(200);
    sound(a, 60, 30, 90); // no onset at all
    sound(a, 64, 120, 128);
    start(a, 64, 120); // shorter than the minimum (11 frames)
    sound(a, 96, 140, 190);
    start(a, 96, 140);

    NoteDecodeParams params;
    params.inferOnsets = false;
    params.melodia = false;
    params.highestKey = 90;
    CHECK(decodeNotes(a, params).empty());

    params.melodia = true;
    const auto notes = decodeNotes(a, params);
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].key == 60);
    CHECK(notes[0].start == frameTime(30));
    CHECK(notes[0].end == frameTime(89));

    // Onsets inferred from the activation's jump find it from the start.
    params.inferOnsets = true;
    params.melodia = false;
    const auto inferred = decodeNotes(a, params);
    REQUIRE(inferred.size() == 1);
    CHECK(inferred[0].start == frameTime(30));
}

#if DAW_HAS_ONNX

namespace {

struct Match {
    double f = 0.0;
    double medianOffset = 0.0;
    double worstOffset = 0.0;
};

/// One-to-one matching on key and start within 50 ms.
Match score(const std::vector<PitchedNote>& found, const std::vector<TestNote>& truth)
{
    std::vector<bool> used(truth.size(), false);
    std::vector<double> offsets;
    for (const PitchedNote& n : found) {
        int best = -1;
        for (std::size_t j = 0; j < truth.size(); ++j)
            if (!used[j] && truth[j].key == n.key && std::abs(truth[j].start - n.start) <= 0.05 &&
                (best < 0 || std::abs(truth[j].start - n.start) < std::abs(truth[static_cast<std::size_t>(best)].start - n.start)))
                best = static_cast<int>(j);
        if (best >= 0) {
            used[static_cast<std::size_t>(best)] = true;
            offsets.push_back(std::abs(n.start - truth[static_cast<std::size_t>(best)].start));
        }
    }
    Match m;
    const double p = static_cast<double>(offsets.size()) / std::max<std::size_t>(1, found.size());
    const double r = static_cast<double>(offsets.size()) / std::max<std::size_t>(1, truth.size());
    m.f = p + r > 0 ? 2 * p * r / (p + r) : 0.0;
    std::sort(offsets.begin(), offsets.end());
    if (!offsets.empty()) {
        m.medianOffset = offsets[offsets.size() / 2];
        m.worstOffset = offsets[offsets.size() * 95 / 100];
    }
    return m;
}

std::vector<PitchedNote> transcribe(const NotePiece& piece, const NoteDecodeParams& params)
{
    auto backend = createOnnxBackend();
    REQUIRE(backend.ok());
    auto transcriber = NoteTranscriber::load(*backend);
    REQUIRE(transcriber.ok());
    auto activations = (*transcriber)->analyse(piece.audio->channelData(0), static_cast<std::size_t>(piece.audio->numFrames()));
    REQUIRE(activations.ok());
    // Every frame is timed, in order, inside the audio.
    REQUIRE(activations->count() > 0);
    CHECK(activations->times.front() >= 0.0);
    CHECK(activations->times.back() < piece.audio->durationSeconds());
    CHECK(std::is_sorted(activations->times.begin(), activations->times.end()));
    return decodeNotes(*activations, params);
}

} // namespace

TEST_CASE("Basic Pitch transcribes a bass line with repeated sixteenths", "[ai][onnx][notes]")
{
    const NotePiece piece = makeNotePiece(8, 120.0, false, NoteTranscriber::kSampleRate);
    NoteDecodeParams params;
    params.minNoteSeconds = 0.058;
    params.onsetThreshold = 0.4f;
    const auto notes = transcribe(piece, params);
    const Match m = score(notes, piece.notes);
    INFO("found " << notes.size() << " of " << piece.notes.size() << ", F " << m.f << ", offsets " << m.medianOffset << " / "
                  << m.worstOffset);
    CHECK(m.f >= 0.85);
    CHECK(m.medianOffset <= 0.010);
    CHECK(m.worstOffset <= 0.020);
}

TEST_CASE("Basic Pitch transcribes chords over a bass line", "[ai][onnx][notes]")
{
    const NotePiece piece = makeNotePiece(8, 110.0, true, NoteTranscriber::kSampleRate);
    NoteDecodeParams params;
    params.minNoteSeconds = 0.058;
    params.onsetThreshold = 0.6f;
    const auto notes = transcribe(piece, params);
    const Match m = score(notes, piece.notes);
    INFO("found " << notes.size() << " of " << piece.notes.size() << ", F " << m.f << ", offsets " << m.medianOffset << " / "
                  << m.worstOffset);
    CHECK(m.f >= 0.7);
    CHECK(m.medianOffset <= 0.010);
}

TEST_CASE("Silence holds no notes", "[ai][onnx][notes]")
{
    auto backend = createOnnxBackend();
    REQUIRE(backend.ok());
    auto transcriber = NoteTranscriber::load(*backend);
    REQUIRE(transcriber.ok());
    const std::vector<float> silence(static_cast<std::size_t>(5 * NoteTranscriber::kSampleRate), 0.0f);
    auto activations = (*transcriber)->analyse(silence.data(), silence.size());
    REQUIRE(activations.ok());
    CHECK(decodeNotes(*activations).empty());
    // A short input still gets a whole window.
    auto shortInput = (*transcriber)->analyse(silence.data(), 1000);
    REQUIRE(shortInput.ok());
    CHECK(shortInput->count() == 4); // frames at samples 0, 256, 512 and 768
}

#endif
