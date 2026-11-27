// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// A synthesised drum kit playing a groove whose tempo drifts, with every hit
// known: ground truth for drum transcription and beat tracking.
//
// Kick: a 150 -> 50 Hz pitch sweep with a short click. Snare: a 190 Hz body
// under band-limited noise. Closed hat: bright noise, 20 ms decay; open hat:
// the same ringing 250 ms (choked by the next hat). Crash: broad noise,
// 1.2 s decay. Toms (floor 82 Hz, low 123 Hz, high 165 Hz): a tone gliding
// down a little, ringing 350 ms, with a weaker inharmonic second mode and a
// stick click.

#include <daw/core/AudioAsset.h>

#include <memory>
#include <vector>

namespace daw::test {

/// Instrument codes, in the order of daw::DrumKind.
enum DrumCode { kTestKick, kTestSnare, kTestClosedHat, kTestOpenHat, kTestCrash, kTestFloorTom, kTestLowTom, kTestHighTom };

struct TestHit {
    double seconds;
    int code;
};

struct DrumGroove {
    std::shared_ptr<AudioAsset> audio;
    std::vector<TestHit> hits;        // in time order
    std::vector<double> beats;        // every quarter note
    std::vector<double> downbeats;    // the first beat of every bar
};

/// `bars` bars of 4/4 from `startSeconds`, the tempo moving linearly from
/// `startBpm` to `endBpm`. Each bar: kick on 1, 3 and the "and" of 3, snare
/// on 2 and 4, closed hats on the eighths with an open hat on the "and" of
/// 4, and a crash on the first beat of every eighth bar. With `fills`,
/// every fourth bar ends with a tom fill instead of beats 3 and 4:
/// sixteenths on the high, high, low, low and four times the floor tom.
DrumGroove makeDrumGroove(int bars, double startBpm, double endBpm, double sampleRate = 48000.0, double startSeconds = 0.5,
                          bool fills = false);

} // namespace daw::test
