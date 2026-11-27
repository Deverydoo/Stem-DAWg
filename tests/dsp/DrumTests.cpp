// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

// Drum transcription and beat tracking on a synthesised kit whose every hit
// is known: each instrument is found where it plays, at the right time, the
// beats follow a drifting tempo, and the bars start on the right beat.

#include <daw/dsp/BeatTracker.h>
#include <daw/dsp/DrumTranscriber.h>

#include "SyntheticDrums.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace daw;
using namespace daw::test;

namespace {

struct Score {
    int found = 0, expected = 0, matched = 0;
    double worstOffset = 0.0;
    double f() const
    {
        const double p = matched / std::max(1.0, static_cast<double>(found));
        const double r = matched / std::max(1.0, static_cast<double>(expected));
        return p + r > 0.0 ? 2.0 * p * r / (p + r) : 0.0;
    }
};

Score score(std::vector<double> found, std::vector<double> expected, double tolerance = 0.03)
{
    std::sort(found.begin(), found.end());
    std::sort(expected.begin(), expected.end());
    Score s;
    s.found = static_cast<int>(found.size());
    s.expected = static_cast<int>(expected.size());
    std::vector<bool> used(expected.size(), false);
    for (double t : found) {
        int best = -1;
        for (std::size_t j = 0; j < expected.size(); ++j)
            if (!used[j] && std::abs(expected[j] - t) <= tolerance
                && (best < 0 || std::abs(expected[j] - t) < std::abs(expected[static_cast<std::size_t>(best)] - t)))
                best = static_cast<int>(j);
        if (best >= 0) {
            used[static_cast<std::size_t>(best)] = true;
            ++s.matched;
            s.worstOffset = std::max(s.worstOffset, std::abs(expected[static_cast<std::size_t>(best)] - t));
        }
    }
    return s;
}

std::vector<double> truth(const DrumGroove& g, std::initializer_list<int> codes)
{
    std::vector<double> out;
    for (const TestHit& h : g.hits)
        if (std::find(codes.begin(), codes.end(), h.code) != codes.end())
            out.push_back(h.seconds);
    return out;
}

std::vector<double> found(const DrumTranscription& t, std::initializer_list<DrumKind> kinds)
{
    std::vector<double> out;
    for (const DrumHit& h : t.hits)
        if (std::find(kinds.begin(), kinds.end(), h.kind) != kinds.end())
            out.push_back(h.seconds);
    return out;
}

DrumTranscription transcribe(const DrumGroove& g)
{
    const AudioAsset& a = *g.audio;
    return transcribeDrums(a.channels[0].data(), a.channels[1].data(), a.numFrames(), a.sampleRate);
}

} // namespace

TEST_CASE("Drum transcription finds each instrument where it plays", "[drums]")
{
    const DrumGroove g = makeDrumGroove(24, 120.0, 126.0);
    const DrumTranscription t = transcribe(g);

    const Score kick = score(found(t, {DrumKind::Kick}), truth(g, {kTestKick}));
    const Score snare = score(found(t, {DrumKind::Snare}), truth(g, {kTestSnare}));
    const Score hats = score(found(t, {DrumKind::ClosedHat, DrumKind::OpenHat}), truth(g, {kTestClosedHat, kTestOpenHat}));
    const Score crash = score(found(t, {DrumKind::Crash}), truth(g, {kTestCrash}));
    INFO("kick " << kick.matched << "/" << kick.expected << " of " << kick.found << ", snare " << snare.matched << "/"
                 << snare.expected << " of " << snare.found << ", hats " << hats.matched << "/" << hats.expected << " of "
                 << hats.found << ", crash " << crash.matched << "/" << crash.expected << " of " << crash.found);
    CHECK(kick.f() >= 0.95);
    CHECK(snare.f() >= 0.85); // a kick half a beat after another, still booming, can read as a snare
    CHECK(hats.f() >= 0.9);
    CHECK(crash.matched == crash.expected);
    CHECK(crash.found <= crash.expected + 1);
    // On time: every matched hit within 15 ms.
    CHECK(kick.worstOffset <= 0.015);
    CHECK(snare.worstOffset <= 0.015);
    CHECK(hats.worstOffset <= 0.015);

    // Open and closed hats are told apart by their ring.
    const Score open = score(found(t, {DrumKind::OpenHat}), truth(g, {kTestOpenHat}));
    INFO("open hats " << open.matched << "/" << open.expected << " of " << open.found);
    CHECK(open.f() >= 0.8);

    // Strength: the kicks' typical loud hit is about 1.
    std::vector<float> strengths;
    for (const DrumHit& h : t.hits)
        if (h.kind == DrumKind::Kick)
            strengths.push_back(h.strength);
    std::sort(strengths.begin(), strengths.end());
    CHECK(strengths[strengths.size() / 2] > 0.5f);
    CHECK(strengths[strengths.size() / 2] < 1.5f);
}

TEST_CASE("Toms are found in fills and sorted by pitch, without taking kicks or snares", "[drums][toms]")
{
    const DrumGroove g = makeDrumGroove(24, 120.0, 126.0, 48000.0, 0.5, true);
    const DrumTranscription t = transcribe(g);

    const Score toms = score(found(t, {DrumKind::FloorTom, DrumKind::LowTom, DrumKind::HighTom}),
                             truth(g, {kTestFloorTom, kTestLowTom, kTestHighTom}));
    const Score kick = score(found(t, {DrumKind::Kick}), truth(g, {kTestKick}));
    const Score snare = score(found(t, {DrumKind::Snare}), truth(g, {kTestSnare}));
    INFO("toms " << toms.matched << "/" << toms.expected << " of " << toms.found << ", kick " << kick.matched << "/" << kick.expected
                 << " of " << kick.found << ", snare " << snare.matched << "/" << snare.expected << " of " << snare.found);
    CHECK(toms.f() >= 0.85);
    CHECK(toms.worstOffset <= 0.015);
    CHECK(kick.f() >= 0.95);
    CHECK(snare.f() >= 0.8); // as without toms, a kick still booming can read as a snare (here also over a floor tom's ring)

    // Each tom, matched by time, mostly comes out as its own kind.
    const DrumKind kinds[] = {DrumKind::FloorTom, DrumKind::LowTom, DrumKind::HighTom};
    const int codes[] = {kTestFloorTom, kTestLowTom, kTestHighTom};
    for (int k = 0; k < 3; ++k) {
        const Score own = score(found(t, {kinds[k]}), truth(g, {codes[k]}));
        INFO("tom " << k << ": " << own.matched << "/" << own.expected << " as its own kind");
        CHECK(own.matched >= static_cast<int>(0.8 * own.expected));
    }

    // A groove without toms has none.
    const DrumTranscription plain = transcribe(makeDrumGroove(24, 120.0, 126.0));
    CHECK(found(plain, {DrumKind::FloorTom, DrumKind::LowTom, DrumKind::HighTom}).empty());
}

TEST_CASE("Drum transcription finds nothing in silence and stays quiet on a held tone", "[drums]")
{
    std::vector<float> silence(48000 * 5, 0.0f);
    CHECK(transcribeDrums(silence.data(), silence.data(), static_cast<std::int64_t>(silence.size()), 48000.0).hits.empty());

    std::vector<float> tone(48000 * 5);
    for (std::size_t i = 0; i < tone.size(); ++i)
        tone[i] = 0.3f * static_cast<float>(std::sin(2.0 * 3.14159265 * 220.0 * static_cast<double>(i) / 48000.0));
    CHECK(transcribeDrums(tone.data(), tone.data(), static_cast<std::int64_t>(tone.size()), 48000.0).hits.size() <= 2);
}

TEST_CASE("Beat tracking follows a drifting tempo and finds the bars", "[drums][beats]")
{
    const DrumGroove g = makeDrumGroove(24, 120.0, 126.0);
    const DrumTranscription t = transcribe(g);
    const BeatTrack beats = trackBeats(t);

    const Score s = score(beats.beats, g.beats, 0.03);
    INFO("beats " << s.matched << "/" << s.expected << " of " << s.found << ", " << beats.bpm << " BPM");
    CHECK(s.matched >= static_cast<int>(0.95 * s.expected));
    CHECK(s.found <= s.expected + 2);
    CHECK(beats.bpm > 120.0);
    CHECK(beats.bpm < 126.0);

    // Bars: tracked downbeats on the true ones.
    std::vector<double> downbeats;
    for (std::size_t i : beats.downbeats)
        downbeats.push_back(beats.beats[i]);
    const Score bars = score(downbeats, g.downbeats, 0.03);
    INFO("downbeats " << bars.matched << "/" << bars.expected << " of " << bars.found);
    CHECK(bars.matched >= static_cast<int>(0.9 * bars.expected));
    CHECK(bars.found <= bars.expected + 2);
}

TEST_CASE("Beat tracking needs some rhythm", "[drums][beats]")
{
    std::vector<float> silence(48000 * 2, 0.0f);
    const DrumTranscription t = transcribeDrums(silence.data(), silence.data(), static_cast<std::int64_t>(silence.size()), 48000.0);
    CHECK(trackBeats(t).beats.empty());
}
