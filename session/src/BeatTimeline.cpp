// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/session/BeatTimeline.h>

#include <daw/project/MusicalTime.h>

#include <algorithm>
#include <cmath>

namespace daw {

namespace {

constexpr double kSteadyBpm = 120.0;

} // namespace

BeatTimeline::BeatTimeline(const BeatTrack& track)
    : beats_(track.beats), downbeats_(track.downbeats), beatsPerBar_(std::max(1, track.beatsPerBar))
{
    const std::vector<double>& b = beats_;
    if (b.size() < 2)
        return;
    // The lead-in: from time 0 to the anchor beat, a whole number of beats
    // (at least one) at its own tempo. A first beat too close to the start
    // for a beat of its own falls inside the lead-in.
    const std::size_t m = b.size();
    anchorIndex_ = b[0] >= 0.75 * (b[1] - b[0]) || m < 3 ? 0 : 1;
    anchor_ = b[anchorIndex_];
    const double anchorPeriod = b[anchorIndex_ + 1] - b[anchorIndex_];
    leadBeats_ = std::max(1, static_cast<int>(std::lround(anchor_ / anchorPeriod)));
}

double BeatTimeline::position(double t) const
{
    if (!followsBeats())
        return t * kSteadyBpm / 60.0;
    const std::vector<double>& b = beats_;
    const std::size_t k = anchorIndex_;
    const std::size_t m = b.size();
    if (t < anchor_)
        return anchor_ > 0.0 ? t / anchor_ * leadBeats_ : 0.0;
    const auto it = std::upper_bound(b.begin() + static_cast<std::ptrdiff_t>(k), b.end(), t);
    std::size_t j = static_cast<std::size_t>(it - b.begin()) - 1; // b[j] <= t
    j = std::min(j, m - 2);                                       // past the last beat: its interval continues
    return leadBeats_ + static_cast<double>(j - k) + (t - b[j]) / (b[j + 1] - b[j]);
}

std::int64_t BeatTimeline::tick(double seconds, int grid) const
{
    double pos = position(seconds);
    const int perBeat = grid > 0 ? std::max(1, grid / 4) : 0;
    if (perBeat > 0)
        pos = std::round(pos * perBeat) / perBeat;
    return static_cast<std::int64_t>(std::llround(std::max(0.0, pos) * kTicksPerQuarter));
}

void BeatTimeline::conduct(MidiSong& song) const
{
    constexpr int ppq = kTicksPerQuarter;
    song.ticksPerQuarter = ppq;
    song.tempo.clear();
    song.timeSignatures.clear();
    if (!followsBeats()) {
        song.tempo.push_back({0, kSteadyBpm});
        song.timeSignatures.push_back({0, 4, 4});
        return;
    }
    const std::vector<double>& b = beats_;
    const std::size_t k = anchorIndex_;
    const std::size_t m = b.size();

    // Tempo: the lead-in's, then one per beat.
    if (anchor_ > 0.0)
        song.tempo.push_back({0, 60.0 * leadBeats_ / anchor_});
    for (std::size_t j = k; j + 1 < m; ++j)
        song.tempo.push_back({static_cast<std::int64_t>(leadBeats_ + static_cast<int>(j - k)) * ppq, 60.0 / (b[j + 1] - b[j])});

    // Bars: each downbeat starts a bar. The beats before the first make a
    // pickup bar of their own length; a bar a beat short or long (across a
    // break) gets its own time signature.
    const auto beatTick = [&](std::size_t j) {
        return static_cast<std::int64_t>(leadBeats_ + static_cast<int>(j) - static_cast<int>(k)) * ppq;
    };
    const std::int64_t barLength = static_cast<std::int64_t>(beatsPerBar_) * ppq;
    std::vector<std::int64_t> barTicks;
    for (std::size_t d : downbeats_)
        if (d >= k)
            barTicks.push_back(beatTick(d));
    if (barTicks.empty())
        barTicks.push_back(0);
    const auto signature = [&](std::int64_t at, std::int64_t length) {
        const int beatsInBar = static_cast<int>(std::max<std::int64_t>(1, length / ppq));
        if (song.timeSignatures.empty() || song.timeSignatures.back().numerator != beatsInBar)
            song.timeSignatures.push_back({at, beatsInBar, 4});
    };
    if (barTicks.front() > 0) {
        // The pickup: whole bars of the regular length, then the remainder first.
        const std::int64_t pickup = barTicks.front() % barLength;
        if (pickup != 0)
            signature(0, pickup);
        signature(pickup, barLength);
    }
    for (std::size_t i = 0; i < barTicks.size(); ++i) {
        const std::int64_t length = i + 1 < barTicks.size() ? barTicks[i + 1] - barTicks[i] : barLength;
        signature(barTicks[i], length);
    }
}

BeatTrack projectBeats(const TempoMap& tempo)
{
    BeatTrack track;
    if (!followsTrackedBeats(tempo))
        return track;
    track.beats = tempo.beats;
    track.bpm = tempo.bpm;
    track.beatsPerBar = std::max(1, tempo.numerator);
    track.downbeats.assign(tempo.downbeats.begin(), tempo.downbeats.end());
    return track;
}

void storeProjectBeats(TempoMap& tempo, const BeatTrack& beats, std::string source)
{
    std::vector<std::uint32_t> downbeats;
    downbeats.reserve(beats.downbeats.size());
    for (std::size_t d : beats.downbeats)
        downbeats.push_back(static_cast<std::uint32_t>(d));
    setTrackedBeats(tempo, beats.beats, std::move(downbeats), beats.beatsPerBar, std::move(source));
}

} // namespace daw
