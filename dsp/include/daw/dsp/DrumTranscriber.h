// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Drum transcription (milestone 6): the hits in a drum stem, by instrument.
//
// The stem's spectrum is reduced to quarter-octave bands and its onsets
// (the rise of each band from one frame to the next) are decomposed by
// non-negative matrix factorisation into a few spectral templates: kick,
// snare, hi-hat/cymbal, plus free components that absorb what is not drums
// (bleed from other stems). The templates start from generic shapes and
// adapt to the song, so its own kick and snare are learned. Because the
// decomposition is additive, hits that sound together (a kick under a hat)
// are found together. Each instrument's activation is peak-picked into
// hits; hi-hat and cymbal hits are told apart by how long they ring.

#include <cstdint>
#include <vector>

namespace daw {

enum class DrumKind {
    Kick,
    Snare,
    ClosedHat,
    OpenHat,
    Crash,
};
inline constexpr int kDrumKindCount = 5;

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
