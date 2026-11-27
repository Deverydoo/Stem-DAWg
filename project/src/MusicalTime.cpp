// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/project/MusicalTime.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace daw {

namespace {

int barLength(const TempoMap& tempo) noexcept
{
    return std::max(1, tempo.numerator);
}

// A time on a beat (to rounding) counts as that beat.
constexpr double kOnBeat = 1e-9;

double steadyBeatSeconds(const TempoMap& tempo) noexcept
{
    return 60.0 / (tempo.bpm > 0.0 ? tempo.bpm : 120.0);
}

/// The bar (counted from the first bar line as bar 0) and 0-based beat in
/// it of whole beat n.
std::pair<long long, int> rawBar(const TempoMap& tempo, long long n)
{
    const long long bpb = barLength(tempo);
    const bool tracked = followsTrackedBeats(tempo);
    const long long first = tracked && !tempo.downbeats.empty() ? tempo.downbeats.front() : 0;
    if (n < first) {
        const long long k = first - n; // beats before the first bar line
        return {-((k - 1) / bpb + 1), static_cast<int>((bpb - k % bpb) % bpb)};
    }
    if (!tracked || tempo.downbeats.empty())
        return {(n - first) / bpb, static_cast<int>((n - first) % bpb)};
    const auto& d = tempo.downbeats;
    const auto it = std::upper_bound(d.begin(), d.end(), static_cast<std::uint64_t>(n),
                                     [](std::uint64_t v, std::uint32_t e) { return v < e; });
    const auto j = static_cast<long long>(it - d.begin()) - 1;
    const long long k = n - d[static_cast<std::size_t>(j)];
    if (it == d.end())
        return {j + k / bpb, static_cast<int>(k % bpb)};
    return {j, static_cast<int>(k)}; // a bar tracked a beat long has a fifth beat
}

} // namespace

bool followsTrackedBeats(const TempoMap& tempo) noexcept
{
    return tempo.beats.size() >= 2;
}

double beatIndexAt(const TempoMap& tempo, double seconds)
{
    if (!followsTrackedBeats(tempo))
        return seconds / steadyBeatSeconds(tempo);
    const auto& b = tempo.beats;
    const std::size_t n = b.size();
    if (seconds < b.front())
        return (seconds - b.front()) / (b[1] - b[0]);
    if (seconds >= b.back())
        return static_cast<double>(n - 1) + (seconds - b.back()) / (b[n - 1] - b[n - 2]);
    const auto i = static_cast<std::size_t>(std::upper_bound(b.begin(), b.end(), seconds) - b.begin()) - 1;
    return static_cast<double>(i) + (seconds - b[i]) / (b[i + 1] - b[i]);
}

double secondsAtBeat(const TempoMap& tempo, double index)
{
    if (!followsTrackedBeats(tempo))
        return index * steadyBeatSeconds(tempo);
    const auto& b = tempo.beats;
    const std::size_t n = b.size();
    if (index < 0.0)
        return b[0] + index * (b[1] - b[0]);
    if (index >= static_cast<double>(n - 1))
        return b[n - 1] + (index - static_cast<double>(n - 1)) * (b[n - 1] - b[n - 2]);
    const auto i = static_cast<std::size_t>(index);
    return b[i] + (index - static_cast<double>(i)) * (b[i + 1] - b[i]);
}

MusicalPosition musicalPosition(const TempoMap& tempo, double seconds)
{
    const double index = beatIndexAt(tempo, seconds);
    const double whole = std::floor(index + kOnBeat);
    const auto origin = rawBar(tempo, static_cast<long long>(std::floor(beatIndexAt(tempo, 0.0) + kOnBeat))).first;
    const auto [bar, beat] = rawBar(tempo, static_cast<long long>(whole));
    return {bar - origin + 1, beat + 1, std::max(0.0, index - whole)};
}

std::vector<BeatMark> beatMarks(const TempoMap& tempo, double from, double to, std::size_t limit)
{
    std::vector<BeatMark> marks;
    if (!(to >= from))
        return marks;
    const auto origin = rawBar(tempo, static_cast<long long>(std::floor(beatIndexAt(tempo, 0.0) + kOnBeat))).first;
    const auto first = static_cast<long long>(std::ceil(beatIndexAt(tempo, from) - kOnBeat));
    const auto last = static_cast<long long>(std::floor(beatIndexAt(tempo, to) + kOnBeat));
    for (long long n = first; n <= last && marks.size() < limit; ++n) {
        const auto [bar, beat] = rawBar(tempo, n);
        marks.push_back({secondsAtBeat(tempo, static_cast<double>(n)), bar - origin + 1, beat + 1});
    }
    return marks;
}

double snapToGrid(const TempoMap& tempo, double seconds, int bars)
{
    const double index = beatIndexAt(tempo, seconds);
    if (bars <= 0)
        return secondsAtBeat(tempo, std::round(index));
    // Look at the whole beats around the time (tracked bars can run a beat
    // long or short) and take the nearest bar line on the grid.
    const auto origin = rawBar(tempo, static_cast<long long>(std::floor(beatIndexAt(tempo, 0.0) + kOnBeat))).first;
    const long long span = static_cast<long long>(barLength(tempo) + 2) * bars + 2;
    const auto centre = static_cast<long long>(std::round(index));
    double best = seconds;
    double bestDistance = -1.0;
    for (long long n = centre - span; n <= centre + span; ++n) {
        const auto [bar, beat] = rawBar(tempo, n);
        const long long fromOne = bar - origin; // 0 for bar 1
        if (beat != 0 || ((fromOne % bars) + bars) % bars != 0)
            continue;
        const double at = secondsAtBeat(tempo, static_cast<double>(n));
        if (bestDistance < 0.0 || std::abs(at - seconds) < bestDistance) {
            best = at;
            bestDistance = std::abs(at - seconds);
        }
    }
    return best;
}

void setTrackedBeats(TempoMap& tempo, std::vector<double> beats, std::vector<std::uint32_t> downbeats, int beatsPerBar,
                     std::string source)
{
    if (beats.size() < 2) {
        tempo.beats.clear();
        tempo.downbeats.clear();
        tempo.beatsSource.clear();
        return;
    }
    std::vector<double> gaps;
    gaps.reserve(beats.size() - 1);
    for (std::size_t i = 1; i < beats.size(); ++i)
        gaps.push_back(beats[i] - beats[i - 1]);
    std::nth_element(gaps.begin(), gaps.begin() + static_cast<std::ptrdiff_t>(gaps.size() / 2), gaps.end());
    const double median = gaps[gaps.size() / 2];
    if (median > 0.0)
        tempo.bpm = std::round(60.0 / median * 10.0) / 10.0;
    tempo.numerator = std::max(1, beatsPerBar);
    tempo.denominator = 4;
    std::erase_if(downbeats, [&](std::uint32_t d) { return d >= beats.size(); });
    tempo.beats = std::move(beats);
    tempo.downbeats = std::move(downbeats);
    tempo.beatsSource = std::move(source);
}

} // namespace daw
