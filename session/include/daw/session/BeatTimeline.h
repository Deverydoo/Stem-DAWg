// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// Song time to MIDI time along a song's tracked beats (milestone 6).
//
// AI songs drift in tempo, so a MIDI file made from one carries a tempo change
// on every beat: the notes stay on the audio, and a DAW's bar lines fall on
// the song's bars. Before the first beat, a lead-in of whole beats at its own
// tempo keeps time 0 at time 0; the beats before the first downbeat form a
// pickup bar with its own time signature, and a bar a beat short or long
// (where the bar phase changed across a break) gets one too.
//
// Without beats (fewer than two), the timeline is a steady 120 BPM in 4/4
// and keeps the played timing.

#include <daw/dsp/BeatTracker.h>
#include <daw/media/MidiFile.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace daw {

class BeatTimeline {
public:
    static constexpr int kTicksPerQuarter = 480;

    explicit BeatTimeline(const BeatTrack& beats);

    /// Whether the timeline follows tracked beats (else a steady 120 BPM).
    bool followsBeats() const noexcept { return beats_.size() >= 2; }

    /// Position in beats (quarter notes) from time 0, fractional.
    double position(double seconds) const;

    /// The tick of a time, quantised within its beat to `grid` notes per
    /// whole note (16: sixteenths; 0: as played). Never negative.
    std::int64_t tick(double seconds, int grid = 0) const;

    /// Writes the tempo map and time signatures into `song`.
    void conduct(MidiSong& song) const;

private:
    std::vector<double> beats_;
    std::vector<std::size_t> downbeats_;
    int beatsPerBar_ = 4;
    std::size_t anchorIndex_ = 0; // the beat the lead-in ends on
    double anchor_ = 0.0;         // its time
    int leadBeats_ = 0;           // beats in the lead-in
};

} // namespace daw
