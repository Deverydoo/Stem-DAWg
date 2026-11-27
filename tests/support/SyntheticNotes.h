// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Pitched test material for note transcription: a bass line in eighths and
// sixteenths with repeated notes, under held three-note chords, played by
// plucked harmonic tones (eight partials, fast attack, exponential decay)
// whose keys and times are known exactly.

#include <daw/core/AudioAsset.h>

#include <memory>
#include <vector>

namespace daw::test {

struct TestNote {
    double start;
    double end;
    int key; // MIDI key
};

struct NotePiece {
    std::shared_ptr<AudioAsset> audio;
    std::vector<TestNote> notes; // in time order
    std::vector<double> beats;   // every quarter note
};

/// `bars` of 4/4 at `bpm`, starting at `startSeconds`. `chords` adds the held
/// chords over the bass line.
NotePiece makeNotePiece(int bars, double bpm, bool chords, double sampleRate = 48000.0, double startSeconds = 0.5);

} // namespace daw::test
