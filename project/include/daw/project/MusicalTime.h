// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Song time to bars and beats along a project's tempo (milestone 6).
//
// With tracked beats (TempoMap::beats, two or more), beat n is the n-th
// tracked beat, and beats before the first and after the last continue at
// the neighbouring beat's spacing. Bars start at the tracked bar lines; past
// the last one they run on in whole bars of TempoMap::numerator beats, and
// before the first one they count back the same way. Without tracked beats
// the bars run at the steady TempoMap::bpm from time 0.
//
// Bars are numbered so that the bar holding time 0 is bar 1, as in a DAW.

#include <daw/project/ProjectModel.h>

#include <cstddef>
#include <vector>

namespace daw {

struct MusicalPosition {
    long long bar = 1;     // from 1
    int beat = 1;          // from 1 within the bar
    double fraction = 0.0; // [0, 1) into the beat
};

struct BeatMark {
    double seconds = 0.0;
    long long bar = 1;
    int beat = 1; // 1: a bar line
};

/// Whether the tempo follows tracked beats (else the steady bpm).
bool followsTrackedBeats(const TempoMap& tempo) noexcept;

/// The position in beats (beat 0: the first tracked beat, or time 0 at a
/// steady tempo), fractional; negative before beat 0.
double beatIndexAt(const TempoMap& tempo, double seconds);

/// The time of a (fractional) beat index; the inverse of beatIndexAt.
double secondsAtBeat(const TempoMap& tempo, double index);

/// The bar and beat of a time.
MusicalPosition musicalPosition(const TempoMap& tempo, double seconds);

/// Every whole beat in [from, to], in order (at most `limit`).
std::vector<BeatMark> beatMarks(const TempoMap& tempo, double from, double to, std::size_t limit = 100000);

/// Sets the tempo to tracked beats (seconds, increasing; bar-line indices
/// into them): bpm becomes their median tempo, numerator the bar length.
/// Fewer than two beats clears the tracked beats and keeps the steady tempo.
void setTrackedBeats(TempoMap& tempo, std::vector<double> beats, std::vector<std::uint32_t> downbeats, int beatsPerBar,
                     std::string source);

} // namespace daw
