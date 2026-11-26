// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/dsp/BeatTracker.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace daw {

namespace {

constexpr double kMinBpm = 50.0;
constexpr double kMaxBpm = 220.0;
constexpr double kPreferredBpm = 120.0;
constexpr double kPriorWidthOctaves = 1.0;
constexpr double kTightness = 100.0;      // how strongly beats keep the local period
constexpr double kWindowSeconds = 8.0;    // local tempo windows (hop: half of it)
constexpr double kLocalRange = 0.08;      // local period within +-8 % of the song's
constexpr std::size_t kBreakBeats = 2;   // beats without kick, snare or crash that make a break
constexpr double kSwitchCost = 6.0;      // changing the bar phase costs this much evidence
constexpr double kReentryWeight = 6.0;   // the first hit after a break is a downbeat
// Weight of each instrument's hits in the beat envelope (DrumKind order).
constexpr std::array<double, kDrumKindCount> kHitWeight{1.0, 1.0, 0.25, 0.25, 0.5};

/// Autocorrelation of `o` at `lag` over [from, to).
double autocorrelation(const std::vector<double>& o, int lag, std::size_t from, std::size_t to)
{
    double sum = 0.0;
    for (std::size_t i = from; i + static_cast<std::size_t>(lag) < to; ++i)
        sum += o[i] * o[i + static_cast<std::size_t>(lag)];
    return sum;
}

/// The best lag in [minLag, maxLag] by score(lag), refined between frames
/// by a parabola through its neighbours.
template <typename Score>
double bestLag(int minLag, int maxLag, Score score)
{
    std::vector<double> s(static_cast<std::size_t>(maxLag - minLag + 3));
    for (int lag = minLag - 1; lag <= maxLag + 1; ++lag)
        s[static_cast<std::size_t>(lag - minLag + 1)] = score(lag);
    int best = minLag;
    for (int lag = minLag; lag <= maxLag; ++lag)
        if (s[static_cast<std::size_t>(lag - minLag + 1)] > s[static_cast<std::size_t>(best - minLag + 1)])
            best = lag;
    const double a = s[static_cast<std::size_t>(best - minLag)];
    const double b = s[static_cast<std::size_t>(best - minLag + 1)];
    const double c = s[static_cast<std::size_t>(best - minLag + 2)];
    const double denom = a - 2.0 * b + c;
    const double shift = std::abs(denom) > 1e-12 ? std::clamp(0.5 * (a - c) / denom, -0.5, 0.5) : 0.0;
    return best + shift;
}

} // namespace

BeatTrack trackBeats(const DrumTranscription& t)
{
    BeatTrack out;
    const double fr = t.frameRate;
    const std::size_t n = t.onsetEnvelope.size();
    if (!(fr > 0.0) || n < static_cast<std::size_t>(4.0 * fr))
        return out;

    // The envelope the beats are tracked on comes from the hits, weighed by
    // instrument: the beat is where kicks and snares fall. All onsets weighed
    // alike, offbeat hats in pairs can pull the beats half a beat off.
    std::vector<double> o(n, 0.0);
    {
        const int spread = std::max(1, static_cast<int>(0.010 * fr));
        for (const DrumHit& h : t.hits) {
            const auto centre = static_cast<int>(std::lround((h.seconds - t.frameOffsetSeconds) * fr));
            const double weight = kHitWeight[static_cast<std::size_t>(h.kind)] * std::min(1.5f, h.strength);
            for (int d = -spread; d <= spread; ++d) {
                const int i = centre + d;
                if (i >= 0 && i < static_cast<int>(n))
                    o[static_cast<std::size_t>(i)] += weight * std::exp(-0.5 * (d * d) / (0.25 * spread * spread));
            }
        }
        double square = 0.0;
        for (double x : o)
            square += x * x;
        const double rms = std::sqrt(square / static_cast<double>(n));
        if (!(rms > 0.0))
            return out;
        for (double& x : o)
            x /= rms;
    }

    // The song's beat period, weighed towards 120 BPM against octave errors.
    const int minLag = std::max(2, static_cast<int>(std::floor(60.0 / kMaxBpm * fr)));
    const int maxLag = static_cast<int>(std::ceil(60.0 / kMinBpm * fr));
    if (static_cast<std::size_t>(maxLag) * 3 >= n)
        return out;
    const double period = bestLag(minLag, maxLag, [&](int lag) {
        const double bpm = 60.0 * fr / lag;
        const double octaves = std::log2(bpm / kPreferredBpm) / kPriorWidthOctaves;
        return autocorrelation(o, lag, 0, n) / static_cast<double>(n - static_cast<std::size_t>(lag))
               * std::exp(-0.5 * octaves * octaves);
    });

    // The local period per window, near the song's, interpolated per frame.
    const auto window = static_cast<std::size_t>(kWindowSeconds * fr);
    const std::size_t hop = window / 2;
    std::vector<double> centres, periods;
    for (std::size_t from = 0; from + window / 2 < n; from += hop) {
        const std::size_t to = std::min(n, from + window);
        const int lo = std::max(minLag, static_cast<int>(std::floor(period * (1.0 - kLocalRange))));
        const int hi = std::min(maxLag, static_cast<int>(std::ceil(period * (1.0 + kLocalRange))));
        double energy = 0.0;
        for (std::size_t i = from; i < to; ++i)
            energy += o[i];
        centres.push_back(0.5 * static_cast<double>(from + to));
        // A window without onsets (a break) keeps the song's period.
        periods.push_back(energy > 1e-9 ? bestLag(lo, hi, [&](int lag) { return autocorrelation(o, lag, from, to); }) : period);
    }
    std::vector<double> localPeriod(n, period);
    for (std::size_t i = 0; i < n; ++i) {
        const auto it = std::upper_bound(centres.begin(), centres.end(), static_cast<double>(i));
        if (it == centres.begin())
            localPeriod[i] = periods.front();
        else if (it == centres.end())
            localPeriod[i] = periods.back();
        else {
            const std::size_t k = static_cast<std::size_t>(it - centres.begin());
            const double frac = (static_cast<double>(i) - centres[k - 1]) / (centres[k] - centres[k - 1]);
            localPeriod[i] = periods[k - 1] + frac * (periods[k] - periods[k - 1]);
        }
    }

    // Dynamic programming: score(i) = onset(i) + the best previous beat,
    // penalised by how far the spacing strays from the local period.
    std::vector<double> score(n);
    std::vector<int> back(n, -1);
    for (std::size_t i = 0; i < n; ++i) {
        const double tau = localPeriod[i];
        const int from = static_cast<int>(i) - static_cast<int>(std::round(2.0 * tau));
        const int to = static_cast<int>(i) - static_cast<int>(std::round(0.5 * tau));
        double best = -std::numeric_limits<double>::infinity();
        int arg = -1;
        for (int p = std::max(0, from); p <= to; ++p) {
            const double ratio = std::log((static_cast<double>(i) - p) / tau);
            const double s = score[static_cast<std::size_t>(p)] - kTightness * ratio * ratio;
            if (s > best) {
                best = s;
                arg = p;
            }
        }
        score[i] = o[i] + (arg >= 0 ? best : 0.0);
        back[i] = arg;
    }
    // The last beat: the best score within one period of the end.
    std::size_t last = n - 1;
    for (std::size_t i = n - static_cast<std::size_t>(std::min<double>(static_cast<double>(n - 1), localPeriod[n - 1])); i < n; ++i)
        if (score[i] > score[last])
            last = i;
    std::vector<int> frames;
    for (int i = static_cast<int>(last); i >= 0; i = back[static_cast<std::size_t>(i)])
        frames.push_back(i);
    std::reverse(frames.begin(), frames.end());
    // Beats more than a beat before the first hit or after the last are
    // guesses through silence: dropped.
    if (!t.hits.empty()) {
        const double first = t.hits.front().seconds - period / fr * 1.01;
        const double lastHit = t.hits.back().seconds + period / fr * 1.01;
        std::erase_if(frames, [&](int f) {
            const double s = f / fr + t.frameOffsetSeconds;
            return s < first || s > lastHit;
        });
    }
    if (frames.size() < 8)
        return out;
    // Each beat moves onto the nearest hit within 35 ms: the path settles a
    // frame or two off the onsets, and a beat that is on a hit quantises it
    // exactly.
    for (int f : frames) {
        double beat = f / fr + t.frameOffsetSeconds;
        const auto it = std::lower_bound(t.hits.begin(), t.hits.end(), beat - 0.035,
                                         [](const DrumHit& h, double x) { return h.seconds < x; });
        double nearest = 1.0;
        for (auto h = it; h != t.hits.end() && h->seconds <= beat + 0.035; ++h)
            if (std::abs(h->seconds - beat) < std::abs(nearest))
                nearest = h->seconds - beat;
        if (std::abs(nearest) <= 0.035)
            beat += nearest;
        if (out.beats.empty() || beat > out.beats.back() + 0.05)
            out.beats.push_back(beat);
    }
    std::vector<double> intervals;
    for (std::size_t i = 1; i < out.beats.size(); ++i)
        intervals.push_back(out.beats[i] - out.beats[i - 1]);
    std::nth_element(intervals.begin(), intervals.begin() + static_cast<std::ptrdiff_t>(intervals.size() / 2), intervals.end());
    out.bpm = 60.0 / intervals[intervals.size() / 2];

    // The downbeat: kicks and crashes on the one, snares on the two and four.
    const double reach = 0.07;
    std::vector<std::array<double, kDrumKindCount>> near(out.beats.size());
    for (const DrumHit& h : t.hits) {
        const auto it = std::lower_bound(out.beats.begin(), out.beats.end(), h.seconds - reach);
        if (it == out.beats.end() || *it > h.seconds + reach)
            continue;
        auto& slot = near[static_cast<std::size_t>(it - out.beats.begin())][static_cast<std::size_t>(h.kind)];
        slot = std::max(slot, static_cast<double>(std::min(h.strength, 1.5f)));
    }
    // Evidence for beat i being bar position 0..3.
    const std::size_t m = out.beats.size();
    constexpr int bar = 4;
    std::vector<std::array<double, bar>> evidence(m);
    for (std::size_t i = 0; i < m; ++i) {
        const auto& h = near[i];
        const double kick = h[static_cast<std::size_t>(DrumKind::Kick)];
        const double snare = h[static_cast<std::size_t>(DrumKind::Snare)];
        const double crash = h[static_cast<std::size_t>(DrumKind::Crash)];
        evidence[i] = {kick + 2.0 * crash - snare, snare - 0.5 * kick, 0.5 * kick - snare, snare - 0.5 * kick};
        // Drums coming back after a break come in on a downbeat. A beat
        // counted through the break may have been gained or lost, so this is
        // strong evidence: as much as the cost of changing phase.
        const auto silent = [&](std::size_t j) {
            const auto& x = near[j];
            return x[static_cast<std::size_t>(DrumKind::Kick)] + x[static_cast<std::size_t>(DrumKind::Snare)]
                       + x[static_cast<std::size_t>(DrumKind::Crash)]
                   == 0.0;
        };
        if (i >= kBreakBeats && (kick > 0.0 || crash > 0.0)) {
            bool afterBreak = true;
            for (std::size_t j = i - kBreakBeats; j < i && afterBreak; ++j)
                afterBreak = silent(j);
            if (afterBreak)
                evidence[i][0] += kReentryWeight;
        }
    }
    // Viterbi over the phase (the bar position of beat 0 if the bars had
    // run on regularly): beat i is at position (i - phase) mod 4.
    std::vector<std::array<double, bar>> total(m);
    std::vector<std::array<int, bar>> from(m);
    for (std::size_t i = 0; i < m; ++i)
        for (int phase = 0; phase < bar; ++phase) {
            const int position = static_cast<int>((i + bar * 4 - static_cast<std::size_t>(phase)) % bar);
            double best = 0.0;
            int arg = phase;
            if (i > 0) {
                best = -std::numeric_limits<double>::infinity();
                for (int p = 0; p < bar; ++p) {
                    const double s = total[i - 1][static_cast<std::size_t>(p)] - (p == phase ? 0.0 : kSwitchCost);
                    if (s > best) {
                        best = s;
                        arg = p;
                    }
                }
            }
            total[i][static_cast<std::size_t>(phase)] = best + evidence[i][static_cast<std::size_t>(position)];
            from[i][static_cast<std::size_t>(phase)] = arg;
        }
    std::vector<int> phases(m);
    phases[m - 1] = static_cast<int>(std::max_element(total[m - 1].begin(), total[m - 1].end()) - total[m - 1].begin());
    for (std::size_t i = m - 1; i > 0; --i)
        phases[i - 1] = from[i][static_cast<std::size_t>(phases[i])];
    for (std::size_t i = 0; i < m; ++i)
        if ((i + bar * 4 - static_cast<std::size_t>(phases[i])) % bar == 0)
            out.downbeats.push_back(i);
    // A phase change can put two bar lines closer than a bar: keep the later.
    std::vector<std::size_t> kept;
    for (std::size_t d : out.downbeats) {
        while (!kept.empty() && d - kept.back() < 2)
            kept.pop_back();
        kept.push_back(d);
    }
    out.downbeats = std::move(kept);
    return out;
}

} // namespace daw
