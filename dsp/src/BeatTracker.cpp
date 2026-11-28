// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/dsp/BeatTracker.h>
#include <daw/dsp/Fft.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>

namespace daw {

namespace {

constexpr double kMinBpm = 50.0;
constexpr double kMaxBpm = 220.0;
constexpr double kPreferredBpm = 120.0;
constexpr double kPriorWidthOctaves = 1.0;
constexpr double kTightness = 100.0;      // how strongly beats keep the local period
constexpr double kWindowSeconds = 8.0;    // local tempo windows (hop: half of it)
constexpr double kLocalRange = 0.04;      // local period within +-4 % of the song's (AI songs drift ~2 %)
constexpr double kSparseShare = 0.25;     // a window with less onset energy than this share of the median keeps the song's period
constexpr std::size_t kBreakBeats = 2;   // beats without kick, snare or crash that make a break
constexpr double kSwitchCost = 6.0;      // changing the bar phase costs this much evidence
constexpr double kReentryWeight = 6.0;   // the first hit after a break is a downbeat
// Weight of each instrument's hits in the beat envelope (DrumKind order).
constexpr std::array<double, kDrumKindCount> kHitWeight{1.0, 1.0, 0.25, 0.25, 0.5, 0.5, 0.5, 0.5};
constexpr int kBar = 4;

// Beats in any audio (trackBeatsInAudio).
constexpr double kEnvelopeRate = 100.0;   // envelope frames per second
constexpr double kFftSeconds = 0.0427;    // 2048 samples at 48 kHz
constexpr int kLevelBands = 48;           // log-spaced bands
constexpr double kLevelLowHz = 40.0;
constexpr double kLevelHighHz = 12000.0;
constexpr double kLevelRangeDb = 80.0;    // levels below the loudest by more than this are held at it
constexpr double kSlowSeconds = 0.4;      // the moving average taken off the flux
constexpr double kFallWeight = 2.0;       // falls (ducking under a kick) against rises (onsets)
constexpr double kMinFluxDb = 0.01;       // mean level change per frame below which there is no rhythm (a held tone's is ~0)
constexpr double kFluxLagSeconds = 0.020; // the flux peaks this long after the frame centre passes an onset (measured)
constexpr double kActiveShare = 1.0;      // envelope (RMS 1) above this: the music has started
constexpr double kNoveltyClip = 6.0;      // spectral change in deviations from its median, clipped to [-3, this]
constexpr double kBarSwitchCost = 16.0;   // changing the bar phase without drums

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

/// Scales `o` to RMS 1; false when it is all zero.
bool normalise(std::vector<double>& o)
{
    double square = 0.0;
    for (double x : o)
        square += x * x;
    const double rms = std::sqrt(square / static_cast<double>(std::max<std::size_t>(1, o.size())));
    if (!(rms > 0.0))
        return false;
    for (double& x : o)
        x /= rms;
    return true;
}

struct BeatPath {
    std::vector<int> frames; // the beats, in envelope frames
    double period = 0.0;     // the song's beat period, in frames
};

/// The beats along an onset envelope (RMS 1, `fr` frames per second): the
/// song's period, weighed towards 120 BPM against octave errors, refined per
/// window, then dynamic programming. Empty when the envelope is too short.
BeatPath beatPath(const std::vector<double>& o, double fr)
{
    BeatPath out;
    const std::size_t n = o.size();
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
    out.period = period;

    // The local period per window, near the song's, interpolated per frame.
    const auto window = static_cast<std::size_t>(kWindowSeconds * fr);
    const std::size_t hop = window / 2;
    std::vector<double> centres, energies;
    std::vector<std::pair<std::size_t, std::size_t>> spans;
    for (std::size_t from = 0; from + window / 2 < n; from += hop) {
        const std::size_t to = std::min(n, from + window);
        double energy = 0.0;
        for (std::size_t i = from; i < to; ++i)
            energy += o[i];
        centres.push_back(0.5 * static_cast<double>(from + to));
        energies.push_back(energy / static_cast<double>(to - from));
        spans.emplace_back(from, to);
    }
    // A window with little rhythm (a sparse intro, a break) gives the
    // autocorrelation nothing to lock on to, so it keeps the song's period.
    std::vector<double> sorted = energies;
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2), sorted.end());
    const double sparse = sorted.empty() ? 0.0 : kSparseShare * sorted[sorted.size() / 2];
    const int lo = std::max(minLag, static_cast<int>(std::floor(period * (1.0 - kLocalRange))));
    const int hi = std::min(maxLag, static_cast<int>(std::ceil(period * (1.0 + kLocalRange))));
    std::vector<double> periods;
    for (std::size_t w = 0; w < spans.size(); ++w) {
        const auto [from, to] = spans[w];
        periods.push_back(energies[w] > 1e-12 && energies[w] >= sparse
                              ? bestLag(lo, hi, [&](int lag) { return autocorrelation(o, lag, from, to); })
                              : period);
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
    for (int i = static_cast<int>(last); i >= 0; i = back[static_cast<std::size_t>(i)])
        out.frames.push_back(i);
    std::reverse(out.frames.begin(), out.frames.end());
    return out;
}

double medianBpm(const std::vector<double>& beats)
{
    std::vector<double> intervals;
    for (std::size_t i = 1; i < beats.size(); ++i)
        intervals.push_back(beats[i] - beats[i - 1]);
    std::nth_element(intervals.begin(), intervals.begin() + static_cast<std::ptrdiff_t>(intervals.size() / 2), intervals.end());
    return 60.0 / intervals[intervals.size() / 2];
}

/// The bar lines among the beats from per-beat evidence for each bar
/// position: Viterbi over the phase (the bar position of beat 0 if the bars
/// had run on regularly; beat i is at position (i - phase) mod 4), changing
/// phase at `switchCost`. A phase change can put two bar lines closer than a
/// bar: the later is kept.
std::vector<std::size_t> barLines(const std::vector<std::array<double, kBar>>& evidence, double switchCost)
{
    const std::size_t m = evidence.size();
    std::vector<std::size_t> lines;
    if (m == 0)
        return lines;
    std::vector<std::array<double, kBar>> total(m);
    std::vector<std::array<int, kBar>> from(m);
    for (std::size_t i = 0; i < m; ++i)
        for (int phase = 0; phase < kBar; ++phase) {
            const int position = static_cast<int>((i + kBar * 4 - static_cast<std::size_t>(phase)) % kBar);
            double best = 0.0;
            int arg = phase;
            if (i > 0) {
                best = -std::numeric_limits<double>::infinity();
                for (int p = 0; p < kBar; ++p) {
                    const double s = total[i - 1][static_cast<std::size_t>(p)] - (p == phase ? 0.0 : switchCost);
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
        if ((i + kBar * 4 - static_cast<std::size_t>(phases[i])) % kBar == 0) {
            while (!lines.empty() && i - lines.back() < 2)
                lines.pop_back();
            lines.push_back(i);
        }
    return lines;
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
    if (!normalise(o))
        return out;

    BeatPath path = beatPath(o, fr);
    std::vector<int>& frames = path.frames;
    // Beats more than a beat before the first hit or after the last are
    // guesses through silence: dropped.
    if (!t.hits.empty()) {
        const double first = t.hits.front().seconds - path.period / fr * 1.01;
        const double lastHit = t.hits.back().seconds + path.period / fr * 1.01;
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
    out.bpm = medianBpm(out.beats);

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
    std::vector<std::array<double, kBar>> evidence(m);
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
    out.downbeats = barLines(evidence, kSwitchCost);
    return out;
}

BeatTrack trackBeatsInAudio(const float* left, const float* right, std::int64_t length, double rate)
{
    BeatTrack out;
    if (left == nullptr || right == nullptr || !(rate > 0.0))
        return out;
    const int size = 1 << std::clamp(static_cast<int>(std::lround(std::log2(kFftSeconds * rate))), 9, 15);
    const int hop = std::max(1, static_cast<int>(std::lround(rate / kEnvelopeRate)));
    const double fr = rate / hop;
    // Frame f's time: its centre, less the flux's lag behind the onsets
    // (measured on synthesised songs whose beats are known: 19-22 ms; on the
    // real songs, 18-21 ms behind the beats tracked on their drum stems).
    const double offset = 0.5 * size / rate - kFluxLagSeconds;
    const auto frames = static_cast<std::size_t>(std::max<std::int64_t>(0, (length - size) / hop + 1));
    if (frames < static_cast<std::size_t>(4.0 * fr))
        return out;

    // Levels (dB) in log-spaced bands; a band too narrow for a bin takes the
    // bin nearest its centre.
    const double top = std::min(kLevelHighHz, 0.45 * rate);
    std::vector<int> binBand(static_cast<std::size_t>(size / 2 + 1), -1);
    for (int b = 0; b < kLevelBands; ++b) {
        const double lo = kLevelLowHz * std::pow(top / kLevelLowHz, static_cast<double>(b) / kLevelBands);
        const double hi = kLevelLowHz * std::pow(top / kLevelLowHz, static_cast<double>(b + 1) / kLevelBands);
        bool any = false;
        for (int k = 1; k <= size / 2; ++k) {
            const double hz = k * rate / size;
            if (hz >= lo && hz < hi) {
                binBand[static_cast<std::size_t>(k)] = b;
                any = true;
            }
        }
        if (!any) {
            const int k = std::clamp(static_cast<int>(std::lround(0.5 * (lo + hi) * size / rate)), 1, size / 2);
            if (binBand[static_cast<std::size_t>(k)] < 0)
                binBand[static_cast<std::size_t>(k)] = b;
        }
    }
    std::vector<float> db(frames * kLevelBands);
    {
        const Fft fft(size);
        // Blackman-Harris (sidelobes below -92 dB): with a Hann window a held
        // tone leaks into the bands below it, and the leakage beats with its
        // negative-frequency image from frame to frame, a 3 dB flicker that
        // reads as rhythm.
        std::vector<float> window(static_cast<std::size_t>(size));
        for (int i = 0; i < size; ++i) {
            const double x = 2.0 * std::numbers::pi * i / size;
            window[static_cast<std::size_t>(i)] =
                static_cast<float>(0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2.0 * x) - 0.01168 * std::cos(3.0 * x));
        }
        std::vector<float> frame(static_cast<std::size_t>(size)), power;
        std::vector<std::complex<float>> scratch;
        std::array<double, kLevelBands> band{};
        for (std::size_t f = 0; f < frames; ++f) {
            const std::int64_t start = static_cast<std::int64_t>(f) * hop;
            for (int i = 0; i < size; ++i)
                frame[static_cast<std::size_t>(i)] = 0.5f * (left[start + i] + right[start + i]) * window[static_cast<std::size_t>(i)];
            fft.powerSpectrum(frame.data(), power, scratch);
            band.fill(0.0);
            for (int k = 1; k <= size / 2; ++k)
                if (const int b = binBand[static_cast<std::size_t>(k)]; b >= 0)
                    band[static_cast<std::size_t>(b)] += power[static_cast<std::size_t>(k)];
            for (int b = 0; b < kLevelBands; ++b)
                db[f * kLevelBands + static_cast<std::size_t>(b)] = static_cast<float>(10.0 * std::log10(std::max(band[static_cast<std::size_t>(b)], 1e-12)));
        }
    }
    const float loudest = *std::max_element(db.begin(), db.end());
    for (float& x : db)
        x = std::max(x, loudest - static_cast<float>(kLevelRangeDb));

    // Rises (onsets) and falls: a part ducked under a kick falls on the beat,
    // and is often loudest between the beats, so its onsets alone put the
    // beats half a beat off. Each without its slow part.
    std::vector<double> rise(frames, 0.0), fall(frames, 0.0);
    for (std::size_t f = 1; f < frames; ++f) {
        double up = 0.0, down = 0.0;
        for (std::size_t b = 0; b < kLevelBands; ++b) {
            const double d = db[f * kLevelBands + b] - db[(f - 1) * kLevelBands + b];
            (d > 0.0 ? up : down) += std::abs(d);
        }
        rise[f] = up / kLevelBands;
        fall[f] = down / kLevelBands;
    }
    const auto withoutSlowPart = [&](std::vector<double>& o) {
        const int radius = static_cast<int>(kSlowSeconds * fr) / 2;
        std::vector<double> prefix(frames + 1, 0.0);
        for (std::size_t f = 0; f < frames; ++f)
            prefix[f + 1] = prefix[f] + o[f];
        std::vector<double> fast(frames);
        for (std::size_t f = 0; f < frames; ++f) {
            // The average over a window centred on f, zero beyond the ends.
            const std::size_t from = f >= static_cast<std::size_t>(radius) ? f - static_cast<std::size_t>(radius) : 0;
            const std::size_t to = std::min(frames, f + static_cast<std::size_t>(radius) + 1);
            fast[f] = std::max(0.0, o[f] - (prefix[to] - prefix[from]) / (2.0 * radius + 1.0));
        }
        o = std::move(fast);
        double sum = 0.0;
        for (double x : o)
            sum += x;
        return sum / static_cast<double>(frames) >= kMinFluxDb && normalise(o);
    };
    const bool rises = withoutSlowPart(rise);
    const bool falls = withoutSlowPart(fall);
    if (!rises && !falls)
        return out;
    std::vector<double> o(frames);
    for (std::size_t f = 0; f < frames; ++f)
        o[f] = rise[f] + kFallWeight * fall[f];
    if (!normalise(o))
        return out;

    BeatPath path = beatPath(o, fr);
    // Beats more than a beat before the music starts or after it ends are
    // guesses through silence: dropped.
    std::size_t firstActive = 0, lastActive = frames - 1;
    while (firstActive < frames && o[firstActive] < kActiveShare)
        ++firstActive;
    while (lastActive > firstActive && o[lastActive] < kActiveShare)
        --lastActive;
    const double reach = path.period * 1.01;
    std::erase_if(path.frames, [&](int f) { return f < firstActive - reach || f > lastActive + reach; });
    if (path.frames.size() < 8)
        return out;
    for (int f : path.frames)
        out.beats.push_back(f / fr + offset);
    out.bpm = medianBpm(out.beats);

    // The downbeat: the sound changes most across the bar line (chords,
    // parts coming in and out). The change across beat i: the mean level
    // difference per band between the beat after it and the beat before.
    const std::size_t m = out.beats.size();
    std::vector<double> change(m, 0.0);
    for (std::size_t i = 1; i + 1 < m; ++i) {
        const auto a = static_cast<std::size_t>(path.frames[i - 1]);
        const auto b = static_cast<std::size_t>(path.frames[i]);
        const auto c = static_cast<std::size_t>(path.frames[i + 1]);
        if (b <= a || c <= b || c > frames)
            continue;
        double sum = 0.0;
        for (std::size_t band = 0; band < kLevelBands; ++band) {
            double before = 0.0, after = 0.0;
            for (std::size_t f = a; f < b; ++f)
                before += db[f * kLevelBands + band];
            for (std::size_t f = b; f < c; ++f)
                after += db[f * kLevelBands + band];
            sum += std::abs(after / static_cast<double>(c - b) - before / static_cast<double>(b - a));
        }
        change[i] = sum / kLevelBands;
    }
    // In deviations from the song's typical change (median absolute
    // deviation), so the evidence is alike from song to song.
    std::vector<double> sorted = change;
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(m / 2), sorted.end());
    const double median = sorted[m / 2];
    for (double& x : sorted)
        x = std::abs(x - median);
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(m / 2), sorted.end());
    const double spread = sorted[m / 2] + 1e-9;
    std::vector<std::array<double, kBar>> evidence(m);
    for (std::size_t i = 0; i < m; ++i) {
        const double z = std::clamp((change[i] - median) / spread, -3.0, kNoveltyClip);
        evidence[i] = {z, -z / 3.0, -z / 3.0, -z / 3.0};
    }
    out.downbeats = barLines(evidence, kBarSwitchCost);
    return out;
}

} // namespace daw
