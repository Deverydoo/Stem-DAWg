// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

// Beat tracking without a drum stem (trackBeatsInAudio): on a full mix (the
// synthesised drum kit), and on synthesised songs without drums whose every
// beat is known: chords changing every bar, ducked on each beat under a kick
// that is not there with a bass on the offbeats, or struck on each beat.

#include <daw/dsp/BeatTracker.h>

#include "SyntheticDrums.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

using namespace daw;
using namespace daw::test;

namespace {

struct Match {
    int found = 0, expected = 0, matched = 0;
    double meanOffset = 0.0;
    double f() const
    {
        const double p = matched / std::max(1.0, static_cast<double>(found));
        const double r = matched / std::max(1.0, static_cast<double>(expected));
        return p + r > 0.0 ? 2.0 * p * r / (p + r) : 0.0;
    }
};

Match match(const std::vector<double>& found, const std::vector<double>& expected, double tolerance)
{
    Match m;
    m.found = static_cast<int>(found.size());
    m.expected = static_cast<int>(expected.size());
    std::vector<bool> used(expected.size(), false);
    double offsets = 0.0;
    for (double t : found) {
        int best = -1;
        for (std::size_t j = 0; j < expected.size(); ++j)
            if (!used[j] && std::abs(expected[j] - t) <= tolerance
                && (best < 0 || std::abs(expected[j] - t) < std::abs(expected[static_cast<std::size_t>(best)] - t)))
                best = static_cast<int>(j);
        if (best >= 0) {
            used[static_cast<std::size_t>(best)] = true;
            ++m.matched;
            offsets += t - expected[static_cast<std::size_t>(best)];
        }
    }
    m.meanOffset = m.matched > 0 ? offsets / m.matched : 0.0;
    return m;
}

std::vector<double> barTimes(const BeatTrack& track)
{
    std::vector<double> out;
    for (std::size_t i : track.downbeats)
        out.push_back(track.beats[i]);
    return out;
}

/// A song without drums on the beats of `g`: four chords, one per bar (a
/// saw-like pad of three notes), either ducked on every beat as under a
/// kick, with a plucked bass on the offbeats, or struck on every beat and
/// left to decay, like a piano.
std::vector<float> chordSong(const DrumGroove& g, bool ducked)
{
    const double rate = 48000.0;
    const double chords[4][3] = {{220.0, 261.63, 329.63}, {174.61, 220.0, 261.63}, {261.63, 329.63, 392.0}, {196.0, 246.94, 293.66}};
    const double end = g.beats.back() + 1.0;
    std::vector<float> out(static_cast<std::size_t>(end * rate), 0.0f);
    std::size_t beat = 0;
    for (std::size_t i = static_cast<std::size_t>(g.beats.front() * rate); i < out.size(); ++i) {
        const double t = static_cast<double>(i) / rate;
        while (beat + 1 < g.beats.size() && g.beats[beat + 1] <= t)
            ++beat;
        const double sinceBeat = t - g.beats[beat];
        const double beatLength = beat + 1 < g.beats.size() ? g.beats[beat + 1] - g.beats[beat] : 0.5;
        const auto* chord = chords[(beat / 4) % 4];
        double pad = 0.0;
        for (int n = 0; n < 3; ++n)
            for (int k = 1; k <= 6; ++k)
                pad += std::sin(2.0 * std::numbers::pi * chord[n] * k * t) / k;
        double sample = 0.0;
        if (ducked) {
            const double gain = 1.0 - 0.85 * std::exp(-sinceBeat / 0.12); // ducked on the beat, back up by the offbeat
            double bass = 0.0;
            const double sinceOffbeat = sinceBeat - 0.5 * beatLength;
            if (sinceOffbeat >= 0.0)
                bass = std::sin(2.0 * std::numbers::pi * 0.5 * chord[0] * t) * std::exp(-sinceOffbeat / 0.12);
            sample = gain * (0.03 * pad + 0.25 * bass);
        } else {
            sample = 0.03 * pad * std::exp(-sinceBeat / 0.25);
        }
        out[i] = static_cast<float>(sample);
    }
    return out;
}

} // namespace

TEST_CASE("Beats and bars are tracked in a full mix", "[beats][audio]")
{
    const DrumGroove g = makeDrumGroove(24, 120.0, 126.0);
    const AudioAsset& a = *g.audio;
    const BeatTrack beats = trackBeatsInAudio(a.channels[0].data(), a.channels[1].data(), a.numFrames(), a.sampleRate);

    const Match m = match(beats.beats, g.beats, 0.03);
    INFO("beats " << m.matched << "/" << m.expected << " of " << m.found << ", offset " << 1000.0 * m.meanOffset << " ms, "
                  << beats.bpm << " BPM");
    CHECK(m.f() >= 0.95);
    CHECK(std::abs(m.meanOffset) <= 0.012);
    CHECK(beats.bpm > 120.0);
    CHECK(beats.bpm < 126.0);
    const Match bars = match(barTimes(beats), g.downbeats, 0.03);
    INFO("downbeats " << bars.matched << "/" << bars.expected << " of " << bars.found);
    CHECK(bars.matched >= static_cast<int>(0.8 * bars.expected));
}

TEST_CASE("Beats without drums fall where a ducked part dips, not on its offbeat onsets", "[beats][audio]")
{
    const DrumGroove g = makeDrumGroove(24, 120.0, 126.0);
    const std::vector<float> song = chordSong(g, true);
    const BeatTrack beats = trackBeatsInAudio(song.data(), song.data(), static_cast<std::int64_t>(song.size()), 48000.0);

    const Match m = match(beats.beats, g.beats, 0.04);
    INFO("beats " << m.matched << "/" << m.expected << " of " << m.found << ", offset " << 1000.0 * m.meanOffset << " ms");
    CHECK(m.f() >= 0.9);
    CHECK(std::abs(m.meanOffset) <= 0.02);
    // The bar lines: where the chords change.
    const Match bars = match(barTimes(beats), g.downbeats, 0.04);
    INFO("downbeats " << bars.matched << "/" << bars.expected << " of " << bars.found);
    CHECK(bars.matched >= static_cast<int>(0.8 * bars.expected));
}

TEST_CASE("Beats without drums follow chords struck on the beat", "[beats][audio]")
{
    const DrumGroove g = makeDrumGroove(24, 120.0, 126.0);
    const std::vector<float> song = chordSong(g, false);
    const BeatTrack beats = trackBeatsInAudio(song.data(), song.data(), static_cast<std::int64_t>(song.size()), 48000.0);

    const Match m = match(beats.beats, g.beats, 0.04);
    INFO("beats " << m.matched << "/" << m.expected << " of " << m.found << ", offset " << 1000.0 * m.meanOffset << " ms");
    CHECK(m.f() >= 0.9);
    CHECK(std::abs(m.meanOffset) <= 0.02);
    const Match bars = match(barTimes(beats), g.downbeats, 0.04);
    INFO("downbeats " << bars.matched << "/" << bars.expected << " of " << bars.found);
    CHECK(bars.matched >= static_cast<int>(0.8 * bars.expected));
}

TEST_CASE("Beats in audio need some rhythm", "[beats][audio]")
{
    std::vector<float> silence(48000 * 10, 0.0f);
    CHECK(trackBeatsInAudio(silence.data(), silence.data(), static_cast<std::int64_t>(silence.size()), 48000.0).beats.empty());
    std::vector<float> tone(48000 * 10);
    for (std::size_t i = 0; i < tone.size(); ++i)
        tone[i] = 0.3f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 220.0 * static_cast<double>(i) / 48000.0));
    CHECK(trackBeatsInAudio(tone.data(), tone.data(), static_cast<std::int64_t>(tone.size()), 48000.0).beats.size() < 8);
    CHECK(trackBeatsInAudio(tone.data(), tone.data(), 1000, 48000.0).beats.empty());
}
