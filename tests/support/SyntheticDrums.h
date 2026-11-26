// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#pragma once

// A synthesised drum kit playing a groove whose tempo drifts, with every hit
// known: ground truth for drum transcription and beat tracking.
//
// Kick: a 150 -> 50 Hz pitch sweep with a short click. Snare: a 190 Hz body
// under band-limited noise. Closed hat: bright noise, 20 ms decay; open hat:
// the same ringing 250 ms (choked by the next hat). Crash: broad noise,
// 1.2 s decay.

#include <daw/core/AudioAsset.h>

#include <memory>
#include <vector>

namespace daw::test {

/// Instrument codes, in the order of daw::DrumKind.
enum DrumCode { kTestKick, kTestSnare, kTestClosedHat, kTestOpenHat, kTestCrash };

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
/// 4, and a crash on the first beat of every eighth bar.
DrumGroove makeDrumGroove(int bars, double startBpm, double endBpm, double sampleRate = 48000.0, double startSeconds = 0.5);

} // namespace daw::test
