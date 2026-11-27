// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Drum transcription (milestone 6): the hits in a drum stem, by instrument.
//
// The stem's spectrum is reduced to quarter-octave bands, and its onsets
// (the rise of each band from one frame to the next) are picked into
// events. Templates for kick, snare, hi-hat and cymbal are learned from the
// song's own hits, and each event is fitted as a non-negative mix of them,
// so hits that sound together (a kick under a hat) are found together;
// hi-hat and cymbal hits are told apart by how long they ring. Toms
// are hits whose low tone rings far longer than the song's kicks, without
// a snare's wires, sorted into up to three by pitch.

#include <cstdint>
#include <vector>

namespace daw {

enum class DrumKind {
    Kick,
    Snare,
    ClosedHat,
    OpenHat,
    Crash,
    FloorTom, // toms from low to high
    LowTom,
    HighTom,
};
inline constexpr int kDrumKindCount = 8;

const char* drumKindName(DrumKind kind) noexcept;

struct DrumHit {
    double seconds = 0.0;
    DrumKind kind = DrumKind::Kick;
    float strength = 0.0f; // relative to the instrument's loud hits: 1 = typical loud hit
};

struct DrumTranscriberParams {
    float sensitivity = 1.0f; // > 1 finds quieter hits
};

struct DrumTranscription {
    std::vector<DrumHit> hits;          // in time order
    std::vector<float> onsetEnvelope;   // all instruments' activations summed, one value per frame
    double frameRate = 0.0;             // frames per second of onsetEnvelope
    double frameOffsetSeconds = 0.0;    // time of frame 0
};

/// Transcribes a stereo (or mono: right == left) drum stem.
DrumTranscription transcribeDrums(const float* left, const float* right, std::int64_t frames, double sampleRate,
                                  const DrumTranscriberParams& params = {});

} // namespace daw
