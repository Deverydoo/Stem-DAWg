// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Beat tracking (milestone 6): the song's beats from an onset envelope, with
// the tempo allowed to wander. AI-generated songs do: a Suno song measured
// for this drifts from 135 to 138 BPM and back, so a fixed grid would be
// hundreds of milliseconds off by the end.
//
// The beats are tracked on an envelope built from the drum hits, weighed by
// instrument (kick and snare 1, crash 0.5, hats 0.25): the beat is where
// kicks and snares fall, and with every onset weighed alike, paired offbeat
// hats pull the beats half a beat off. The tempo comes from the envelope's
// autocorrelation (weighed towards 120 BPM against octave errors), refined
// per 8-second window. The beats are
// placed by dynamic programming (Ellis, "Beat tracking by dynamic
// programming", 2007): each beat sits on strong onsets while the spacing to
// the previous beat stays close to the local beat period.
//
// Downbeats (the "one" of each 4/4 bar) are where kicks and crashes fall
// and snares do not, with snares on the two and four. The bar position is
// decided beat by beat (Viterbi over the four phases, a change of phase
// costing as much as about three bars of evidence), so a beat gained or lost
// across a break, where there is nothing to track, does not shift the bars
// of the rest of the song: that bar is simply a beat short or long.

#include <daw/dsp/DrumTranscriber.h>

#include <vector>

namespace daw {

struct BeatTrack {
    std::vector<double> beats;            // seconds, increasing
    double bpm = 0.0;                     // median tempo
    int beatsPerBar = 4;
    std::vector<std::size_t> downbeats;   // indices into beats of the bar lines, increasing
};

/// Tracks the beats of a transcription (its onset envelope; its hits find
/// the downbeat). Empty when there is too little to go on.
BeatTrack trackBeats(const DrumTranscription& transcription);

} // namespace daw
