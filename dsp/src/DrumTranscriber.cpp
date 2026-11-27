// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/dsp/DrumTranscriber.h>

#include <daw/dsp/Biquad.h>
#include <daw/dsp/Fft.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>

namespace daw {

const char* drumKindName(DrumKind kind) noexcept
{
    switch (kind) {
    case DrumKind::Kick:
        return "kick";
    case DrumKind::Snare:
        return "snare";
    case DrumKind::ClosedHat:
        return "closed hat";
    case DrumKind::OpenHat:
        return "open hat";
    case DrumKind::Crash:
        return "crash";
    case DrumKind::FloorTom:
        return "floor tom";
    case DrumKind::LowTom:
        return "low tom";
    case DrumKind::HighTom:
        return "high tom";
    }
    return "?";
}

namespace {

constexpr double kLowestHz = 30.0;
constexpr double kHighestHz = 16000.0;
constexpr double kBandsPerOctave = 4.0;

enum Instrument { kKick, kSnare, kHat, kCymbal, kInstruments };

constexpr double kKickTopHz = 120.0;  // the kick is measured below this
constexpr double kRingFromHz = 6000.0; // the ring is measured above this
constexpr double kHatMeasureHz = 8000.0; // a hat's share is measured above this (kicks have little there)
constexpr double kOpenRingDb = -7.0;    // 40-100 ms after the peak: closed hats are lower
constexpr double kLongRingDb = -6.0;    // 100-400 ms after the peak: open hats are lower
constexpr double kCrashSoonDb = -4.0;   // a crash barely falls in its first 100 ms...
constexpr double kCrashLaterDb = -8.0;  // ...nor much by 400 ms
constexpr double kCrashJumpDb = 6.0;    // and it starts with a jump of the top band
constexpr int kMinCymbalEvents = 4;
constexpr double kMinRelativeRise = 0.03; // mean over bands of (rise / level before), capped at 1 per band
constexpr int kRegions = 6;
// Toms (findToms).
constexpr double kToneBefore = -0.09; // spectra start this long after the hit (s)
constexpr double kToneEarly = 0.0;
constexpr double kToneMiddle = 0.06;
constexpr double kToneLate = 0.16;
constexpr double kToneSeconds = 0.085;  // each spectrum's length (about; 4096 samples at 48 kHz)
constexpr double kSameDrumOctaves = 1.0 / 12.0; // a hit within a semitone is the same drum struck again
constexpr double kToneLowHz = 40.0;
constexpr double kToneHighHz = 800.0;
constexpr double kFundamentalShare = 0.2;   // the lowest peak this share of the strongest is the tone
constexpr double kTomKickShare = 0.1;       // events with this share of a loud kick's amount...
constexpr double kTomSnareShare = 0.15;     // ...or of a loud snare's are measured
constexpr double kTomRingMarginDb = 12.0;   // a tom's tone rings this much longer than the song's kicks
constexpr double kTomMinRingDb = -45.0;     // and keeps at least this much of its level
constexpr double kFillStrokeShare = 0.25;   // a stroke within a fill holds this share of its strongest
constexpr double kTomOnsetDb = 6.0;         // and its tone starts with the hit: this much above the 85 ms before
constexpr double kTomWireMarginDb = 2.0;    // and has this much less above 2 kHz than the song's snares
constexpr double kTomApartOctaves = 1.0 / 6.0; // a tone this close to the kick's pitch...
constexpr double kTomKickPitchRingDb = 20.0;   // ...must ring this much longer than the kicks
constexpr double kTomHigherRingDb = 0.0;       // or: rings at least as long as the kicks
constexpr double kTomHigherOctaves = 1.0 / 3.0; // at least a major third above the kick's pitch
constexpr double kTomHigherWireMarginDb = 6.0; // with this much less above 2 kHz than the snares
constexpr double kSnarePitchOctaves = 1.0 / 24.0; // a snare-like hit within half a semitone of the snare's pitch is a snare
constexpr std::size_t kMinTomHits = 3;
constexpr double kTomMergeOctaves = 1.0 / 6.0; // toms closer than a whole tone are one
constexpr double kMinTomGroupShare = 0.1;   // a tom group holds at least this share of the tom hits

struct Spectrogram {
    int bands = 0;
    int frames = 0;
    std::vector<double> centreHz;
    std::vector<float> value; // [frame * bands + band], compressed magnitude
    float at(int frame, int band) const { return value[static_cast<std::size_t>(frame) * bands + band]; }
};

/// Quarter-octave band magnitudes, square-root compressed so the quiet high
/// bands count next to the loud low end.
Spectrogram bandSpectrogram(const float* left, const float* right, std::int64_t length, double rate, int size, int hop)
{
    Spectrogram s;
    const double top = std::min(kHighestHz, 0.45 * rate);
    std::vector<int> binBand(static_cast<std::size_t>(size / 2 + 1), -1);
    const int rawBands = static_cast<int>(std::ceil(std::log2(top / kLowestHz) * kBandsPerOctave));
    std::vector<int> rawToBand(static_cast<std::size_t>(rawBands), -1);
    for (int k = 1; k <= size / 2; ++k) {
        const double hz = k * rate / size;
        if (hz < kLowestHz || hz >= top)
            continue;
        const int raw = std::min(rawBands - 1, static_cast<int>(std::log2(hz / kLowestHz) * kBandsPerOctave));
        if (rawToBand[static_cast<std::size_t>(raw)] < 0) {
            rawToBand[static_cast<std::size_t>(raw)] = s.bands++;
            s.centreHz.push_back(kLowestHz * std::pow(2.0, (raw + 0.5) / kBandsPerOctave));
        }
        binBand[static_cast<std::size_t>(k)] = rawToBand[static_cast<std::size_t>(raw)];
    }

    s.frames = static_cast<int>(std::max<std::int64_t>(0, (length - size) / hop + 1));
    s.value.assign(static_cast<std::size_t>(s.frames) * s.bands, 0.0f);
    const Fft fft(size);
    std::vector<float> window(static_cast<std::size_t>(size)), frame(static_cast<std::size_t>(size)), power;
    std::vector<std::complex<float>> scratch;
    for (int i = 0; i < size; ++i)
        window[static_cast<std::size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / size));
    std::vector<double> bandPower(static_cast<std::size_t>(s.bands));
    for (int f = 0; f < s.frames; ++f) {
        const std::int64_t start = static_cast<std::int64_t>(f) * hop;
        for (int i = 0; i < size; ++i)
            frame[static_cast<std::size_t>(i)] = 0.5f * (left[start + i] + right[start + i]) * window[static_cast<std::size_t>(i)];
        fft.powerSpectrum(frame.data(), power, scratch);
        std::fill(bandPower.begin(), bandPower.end(), 0.0);
        for (int k = 1; k <= size / 2; ++k)
            if (const int b = binBand[static_cast<std::size_t>(k)]; b >= 0)
                bandPower[static_cast<std::size_t>(b)] += power[static_cast<std::size_t>(k)];
        for (int b = 0; b < s.bands; ++b)
            s.value[static_cast<std::size_t>(f) * s.bands + b] = static_cast<float>(std::pow(bandPower[static_cast<std::size_t>(b)], 0.25));
    }
    return s;
}

double percentile(std::vector<double> values, double q)
{
    if (values.empty())
        return 0.0;
    const auto k = static_cast<std::size_t>(q * static_cast<double>(values.size() - 1));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(k), values.end());
    return values[k];
}

using Vec = std::vector<double>;

double regionSum(const Vec& v, const std::vector<double>& hz, double fromHz, double toHz)
{
    double sum = 0.0;
    for (std::size_t b = 0; b < v.size(); ++b)
        if (hz[b] >= fromHz && hz[b] < toHz)
            sum += v[b];
    return sum;
}

/// The shape most of the given events share: per band, a percentile
/// (`share`) of each event normalised by its sum over [fromHz, toHz). A low
/// percentile leaves out what only some of them carry (a hat on some of the
/// kicks); the median suits events of one instrument alone.
Vec sharedShape(const std::vector<Vec>& events, const std::vector<double>& hz, double fromHz, double toHz,
               double share = 0.3)
{
    const std::size_t bands = hz.size();
    Vec shape(bands, 0.0);
    if (events.empty())
        return shape;
    std::vector<double> column;
    for (std::size_t b = 0; b < bands; ++b) {
        column.clear();
        for (const Vec& e : events) {
            const double norm = regionSum(e, hz, fromHz, toHz);
            if (norm > 0.0)
                column.push_back(e[b] / norm);
        }
        shape[b] = percentile(column, share);
    }
    double sum = 0.0;
    for (double x : shape)
        sum += x;
    for (double& x : shape)
        x /= std::max(sum, 1e-12);
    return shape;
}

/// The events ranked highest by `key`: the top `share` of those with key > 0.
std::vector<Vec> topEvents(const std::vector<Vec>& events, const std::vector<double>& keys, double share)
{
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < events.size(); ++i)
        if (keys[i] > 0.0)
            order.push_back(i);
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return keys[a] > keys[b]; });
    order.resize(static_cast<std::size_t>(std::ceil(share * static_cast<double>(order.size()))));
    std::vector<Vec> out;
    for (std::size_t i : order)
        out.push_back(events[i]);
    return out;
}

/// Non-negative least squares for a handful of instruments: every subset
/// of the present instruments is solved exactly (normal equations) and the
/// best fit with no negative amount wins.
std::array<double, kInstruments> nonNegativeFit(const std::array<double, kRegions>& r,
                                                const std::array<std::array<double, kRegions>, kInstruments>& t,
                                                const std::array<bool, kInstruments>& present)
{
    std::array<double, kInstruments> best{};
    double bestError = 0.0;
    for (int k = 0; k < kRegions; ++k)
        bestError += r[static_cast<std::size_t>(k)] * r[static_cast<std::size_t>(k)];
    for (unsigned mask = 1; mask < (1u << kInstruments); ++mask) {
        int idx[kInstruments];
        int n = 0;
        bool usable = true;
        for (int c = 0; c < kInstruments; ++c)
            if ((mask >> c) & 1u) {
                usable = usable && present[static_cast<std::size_t>(c)];
                idx[n++] = c;
            }
        if (!usable)
            continue;
        // Normal equations A x = b, solved by Gaussian elimination.
        double a[kInstruments][kInstruments + 1] = {};
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j)
                for (int k = 0; k < kRegions; ++k)
                    a[i][j] += t[static_cast<std::size_t>(idx[i])][static_cast<std::size_t>(k)] * t[static_cast<std::size_t>(idx[j])][static_cast<std::size_t>(k)];
            for (int k = 0; k < kRegions; ++k)
                a[i][n] += t[static_cast<std::size_t>(idx[i])][static_cast<std::size_t>(k)] * r[static_cast<std::size_t>(k)];
        }
        bool singular = false;
        for (int col = 0; col < n && !singular; ++col) {
            int pivot = col;
            for (int row = col + 1; row < n; ++row)
                if (std::abs(a[row][col]) > std::abs(a[pivot][col]))
                    pivot = row;
            if (std::abs(a[pivot][col]) < 1e-12) {
                singular = true;
                break;
            }
            for (int j = 0; j <= n; ++j)
                std::swap(a[col][j], a[pivot][j]);
            for (int row = 0; row < n; ++row) {
                if (row == col)
                    continue;
                const double factor = a[row][col] / a[col][col];
                for (int j = col; j <= n; ++j)
                    a[row][j] -= factor * a[col][j];
            }
        }
        if (singular)
            continue;
        std::array<double, kInstruments> x{};
        bool negative = false;
        for (int i = 0; i < n; ++i) {
            x[static_cast<std::size_t>(idx[i])] = a[i][n] / a[i][i];
            negative = negative || x[static_cast<std::size_t>(idx[i])] < 0.0;
        }
        if (negative)
            continue;
        double error = 0.0;
        for (int k = 0; k < kRegions; ++k) {
            double v = r[static_cast<std::size_t>(k)];
            for (int c = 0; c < kInstruments; ++c)
                v -= x[static_cast<std::size_t>(c)] * t[static_cast<std::size_t>(c)][static_cast<std::size_t>(k)];
            error += v * v;
        }
        if (error < bestError) {
            bestError = error;
            best = x;
        }
    }
    return best;
}

/// What a hit's low tone does, from finer spectra than the band
/// spectrogram (whose 30-60 Hz bands are mostly leakage).
struct ToneFeatures {
    double hz = 0.0;     // the tone: the lowest clear peak of what rises 40-800 Hz
    double ringDb = 0.0; // the tone 160-245 ms after the hit against its first 85 ms
    double wiresDb = 0.0; // 2-8 kHz against 40-800 Hz in the first 85 ms
    double onsetDb = 0.0; // the tone in the first 85 ms against the 85 ms before the hit
};

ToneFeatures toneOf(const float* left, const float* right, std::int64_t length, double rate, double seconds, const Fft& fft,
                    int size, const std::vector<float>& window)
{
    std::vector<float> frame(static_cast<std::size_t>(size));
    std::vector<std::complex<float>> scratch;
    const auto spectrum = [&](double from) {
        const auto start = static_cast<std::int64_t>(std::llround((seconds + from) * rate));
        for (int i = 0; i < size; ++i) {
            const std::int64_t j = start + i;
            const float x = j >= 0 && j < length ? 0.5f * (left[j] + right[j]) : 0.0f;
            frame[static_cast<std::size_t>(i)] = x * window[static_cast<std::size_t>(i)];
        }
        std::vector<float> power;
        fft.powerSpectrum(frame.data(), power, scratch);
        return power;
    };
    const auto before = spectrum(kToneBefore);
    const auto early = spectrum(kToneEarly);
    const auto middle = spectrum(kToneMiddle);
    const auto late = spectrum(kToneLate);
    const double binHz = rate / size;
    const auto binOf = [&](double hz) { return std::clamp(static_cast<int>(hz / binHz), 0, size / 2); };
    const auto sum = [&](const std::vector<float>& p, double fromHz, double toHz) {
        double s = 0.0;
        for (int k = binOf(fromHz); k <= binOf(toHz); ++k)
            s += p[static_cast<std::size_t>(k)];
        return s + 1e-20;
    };

    ToneFeatures f;
    const int lo = binOf(kToneLowHz), hi = binOf(kToneHighHz);
    std::vector<double> rise(static_cast<std::size_t>(hi + 2), 0.0);
    double top = 0.0;
    int best = lo;
    for (int k = lo; k <= hi; ++k) {
        rise[static_cast<std::size_t>(k)] = std::max(0.0, static_cast<double>(early[static_cast<std::size_t>(k)]) - before[static_cast<std::size_t>(k)]);
        if (rise[static_cast<std::size_t>(k)] > top) {
            top = rise[static_cast<std::size_t>(k)];
            best = k;
        }
    }
    // A tom's overtones can be stronger than its fundamental: the lowest
    // peak that is clearly there.
    for (int k = lo + 1; k < hi; ++k) {
        const double r = rise[static_cast<std::size_t>(k)];
        if (r >= kFundamentalShare * top && r >= rise[static_cast<std::size_t>(k) - 1] && r >= rise[static_cast<std::size_t>(k) + 1]) {
            best = k;
            break;
        }
    }
    double offset = 0.0;
    const double a = rise[static_cast<std::size_t>(best) - 1], b = rise[static_cast<std::size_t>(best)], c = rise[static_cast<std::size_t>(best) + 1];
    if (a > 0.0 && b > 0.0 && c > 0.0) {
        const double la = std::log(a), lb = std::log(b), lc = std::log(c);
        const double d = la - 2.0 * lb + lc;
        if (d != 0.0)
            offset = std::clamp(0.5 * (la - lc) / d, -0.5, 0.5);
    }
    f.hz = (best + offset) * binHz;
    // The tone's level: its bin and one either side (the window's main lobe).
    const auto toneLevel = [&](const std::vector<float>& p) {
        const int centre = std::clamp(static_cast<int>(std::lround(f.hz / binHz)), 1, size / 2 - 1);
        return static_cast<double>(p[static_cast<std::size_t>(centre) - 1]) + p[static_cast<std::size_t>(centre)]
               + p[static_cast<std::size_t>(centre) + 1] + 1e-20;
    };
    f.ringDb = 10.0 * std::log10(toneLevel(late) / toneLevel(early));
    f.onsetDb = 10.0 * std::log10(toneLevel(early) / toneLevel(before));
    f.wiresDb = 10.0 * std::log10(sum(early, 2000.0, 8000.0) / sum(early, kToneLowHz, kToneHighHz));
    return f;
}

/// The tom group of each event (-1: not a tom), groups numbered from the
/// lowest tone up. A tom rings: its tone, 160-245 ms after the hit, keeps
/// far more of its level than the song's kicks keep of theirs (measured
/// against the kicks' 25th percentile, which leaves out toms the fit took
/// for kicks). And it has no wires: less above 2 kHz than the song's snares.
std::vector<int> findToms(const float* left, const float* right, std::int64_t length, double rate, const std::vector<int>& events,
                          double frameRate, double frameOffset, const std::vector<std::array<double, kInstruments>>& amounts,
                          const std::array<double, kInstruments>& loud, double kickFloor, double snareFloor)
{
    std::vector<int> group(events.size(), -1);
    if (events.empty() || loud[kKick] <= 0.0)
        return group;
    int size = 1024;
    while (size * 2 <= static_cast<int>(rate * 0.1))
        size *= 2; // 4096 at 44.1 / 48 kHz: 85-93 ms
    const Fft fft(size);
    std::vector<float> window(static_cast<std::size_t>(size));
    for (int i = 0; i < size; ++i)
        window[static_cast<std::size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / size));

    // Only events with some low end or body are measured.
    std::vector<ToneFeatures> tone(events.size());
    std::vector<bool> lowMid(events.size(), false);
    for (std::size_t i = 0; i < events.size(); ++i) {
        lowMid[i] = amounts[i][kKick] >= kTomKickShare * loud[kKick] || amounts[i][kSnare] >= kTomSnareShare * loud[kSnare];
        if (lowMid[i])
            tone[i] = toneOf(left, right, length, rate, events[i] / frameRate + frameOffset - 0.005, fft, size, window);
    }
    // The same drum struck again before the ring is measured (a roll, a
    // double kick, a tom played twice) sounds in this hit's ring window at
    // its pitch: such a hit takes the ring of the run's last stroke, whose
    // window is clear.
    {
        std::size_t next = events.size();
        for (std::size_t i = events.size(); i-- > 0;) {
            if (!lowMid[i])
                continue;
            if (next < events.size() && (events[next] - events[i]) / frameRate < kToneLate + kToneSeconds
                && std::abs(std::log2(std::max(20.0, tone[next].hz) / std::max(20.0, tone[i].hz))) <= kSameDrumOctaves)
                tone[i].ringDb = tone[next].ringDb;
            next = i;
        }
    }
    // The song's kicks (kick hits without a snare): how long their tone
    // rings (the 25th percentile, which leaves out toms the fit took for
    // kicks) and its pitch.
    const auto pitchOf = [&](std::size_t i) { return std::log2(std::max(20.0, tone[i].hz)); };
    std::vector<double> kickRings, kickPitches;
    std::vector<bool> snareLike(events.size(), false);
    std::vector<double> snarePitches;
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (!lowMid[i])
            continue;
        const double kick = amounts[i][kKick] / loud[kKick];
        const double snare = loud[kSnare] > 0.0 ? amounts[i][kSnare] / loud[kSnare] : 0.0;
        if (amounts[i][kKick] >= kickFloor && amounts[i][kSnare] < snareFloor) {
            kickRings.push_back(tone[i].ringDb);
            kickPitches.push_back(pitchOf(i));
        } else if (amounts[i][kSnare] >= snareFloor && snare >= kick) {
            snareLike[i] = true;
            snarePitches.push_back(pitchOf(i));
        }
    }
    if (kickRings.size() < 8)
        return group; // too few kicks to know how long they ring
    const double kickRing = percentile(kickRings, 0.25);
    const double kickPitch = percentile(kickPitches, 0.5);
    // The song's snare is struck at one pitch, its toms at several: among
    // the hits holding more of a loud snare than of a loud kick, the most
    // common pitch (half-semitone bins, smoothed over a semitone) is the
    // snare's. Those hits set the snare's wires.
    double snarePitch = -1.0;
    if (!snarePitches.empty()) {
        std::array<int, 24 * 6> bins{}; // 40-2560 Hz... in half semitones from log2(40)
        const double base = std::log2(kToneLowHz);
        for (double p : snarePitches)
            ++bins[static_cast<std::size_t>(std::clamp(static_cast<int>((p - base) * 24.0), 0, static_cast<int>(bins.size()) - 1))];
        int best = 0, bestCount = -1;
        for (int b = 0; b < static_cast<int>(bins.size()); ++b) {
            int count = 0;
            for (int d = -1; d <= 1; ++d)
                if (b + d >= 0 && b + d < static_cast<int>(bins.size()))
                    count += bins[static_cast<std::size_t>(b + d)];
            if (count > bestCount) {
                bestCount = count;
                best = b;
            }
        }
        // The peak's own pitch: the median of the hits around the best bin
        // (neighbouring bins tie when the snare's hits fill one bin).
        std::vector<double> near;
        const double centre = base + (best + 0.5) / 24.0;
        for (double p : snarePitches)
            if (std::abs(p - centre) <= 1.5 / 24.0)
                near.push_back(p);
        snarePitch = percentile(near, 0.5);
    }
    const auto atSnarePitch = [&](std::size_t i) { return snareLike[i] && std::abs(pitchOf(i) - snarePitch) <= kSnarePitchOctaves; };
    std::vector<double> snareWires;
    for (std::size_t i = 0; i < events.size(); ++i)
        if (atSnarePitch(i))
            snareWires.push_back(tone[i].wiresDb);
    const double snareRef = snareWires.size() >= 4 ? percentile(snareWires, 0.5) : 0.0;

    // A tom has no wires, and either rings far longer than the kicks (at a
    // pitch of its own: a tone at the kick's pitch is a kick, unless it rings
    // very long) or rings somewhat longer at a clearly higher pitch, with
    // clearly fewer wires.
    std::vector<std::size_t> toms;
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (!lowMid[i] || atSnarePitch(i) || tone[i].ringDb < kTomMinRingDb || tone[i].wiresDb > snareRef - kTomWireMarginDb)
            continue;
        const double excess = tone[i].ringDb - kickRing;
        const double above = std::log2(std::max(20.0, tone[i].hz)) - kickPitch;
        const bool longRing = excess >= kTomRingMarginDb && (std::abs(above) >= kTomApartOctaves || excess >= kTomKickPitchRingDb);
        const bool higher = excess >= kTomHigherRingDb && above >= kTomHigherOctaves && tone[i].wiresDb <= snareRef - kTomHigherWireMarginDb;
        if (longRing || higher)
            toms.push_back(i);
    }
    // A tom's tone starts with the hit; a low note bleeding into the stem
    // was already sounding. Toms within a ring of each other are one fill,
    // whose strokes start over the earlier ones' rings: the fill is judged
    // by its first stroke.
    {
        // Within a fill, a stroke far weaker than the fill's strongest so far
        // is the ring of two toms beating, not a stroke.
        std::vector<std::size_t> kept;
        bool fillStarts = false;
        double strongest = 0.0;
        for (std::size_t k = 0; k < toms.size(); ++k) {
            const std::size_t i = toms[k];
            const double amount = amounts[i][kKick] / loud[kKick] + (loud[kSnare] > 0.0 ? amounts[i][kSnare] / loud[kSnare] : 0.0);
            const bool sameFill = k > 0 && (events[i] - events[toms[k - 1]]) / frameRate < kToneLate + kToneSeconds;
            if (!sameFill) {
                fillStarts = tone[i].onsetDb >= kTomOnsetDb;
                strongest = 0.0;
            }
            strongest = std::max(strongest, amount);
            if (fillStarts && amount >= kFillStrokeShare * strongest)
                kept.push_back(i);
        }
        toms = std::move(kept);
    }
    if (toms.size() < kMinTomHits)
        return group;

    // Up to three toms by tone, on the octave scale: the best split of the
    // sorted pitches into three, two or one groups (exact 1-D k-means by
    // dynamic programming), taking the most groups for which every group
    // holds a fair share of the hits (a few stray pitch readings are not a
    // tom of their own) and neighbouring groups are a whole tone apart.
    std::vector<std::pair<double, std::size_t>> byPitch; // pitch, index into toms
    for (std::size_t j = 0; j < toms.size(); ++j)
        byPitch.emplace_back(std::log2(std::max(20.0, tone[toms[j]].hz)), j);
    std::sort(byPitch.begin(), byPitch.end());
    const std::size_t n = byPitch.size();
    std::vector<double> sum(n + 1, 0.0), squares(n + 1, 0.0);
    for (std::size_t j = 0; j < n; ++j) {
        sum[j + 1] = sum[j] + byPitch[j].first;
        squares[j + 1] = squares[j] + byPitch[j].first * byPitch[j].first;
    }
    // The spread of sorted pitches [a, b).
    const auto cost = [&](std::size_t a, std::size_t b) {
        const double s = sum[b] - sum[a];
        return squares[b] - squares[a] - s * s / static_cast<double>(b - a);
    };
    const auto mean = [&](std::size_t a, std::size_t b) { return (sum[b] - sum[a]) / static_cast<double>(b - a); };
    const std::size_t minSize = std::max<std::size_t>(2, static_cast<std::size_t>(std::ceil(kMinTomGroupShare * static_cast<double>(n))));
    std::vector<std::size_t> starts{0}; // where each group starts in the sorted pitches
    for (std::size_t k = std::min<std::size_t>(3, n / minSize); k >= 2; --k) {
        // best[g][j]: the least spread of the first j pitches in g + 1 groups.
        const double inf = std::numeric_limits<double>::infinity();
        std::vector<std::vector<double>> best(k, std::vector<double>(n + 1, inf));
        std::vector<std::vector<std::size_t>> from(k, std::vector<std::size_t>(n + 1, 0));
        for (std::size_t j = minSize; j <= n; ++j)
            best[0][j] = cost(0, j);
        for (std::size_t g = 1; g < k; ++g)
            for (std::size_t j = (g + 1) * minSize; j <= n; ++j)
                for (std::size_t a = g * minSize; a + minSize <= j; ++a)
                    if (best[g - 1][a] < inf && best[g - 1][a] + cost(a, j) < best[g][j]) {
                        best[g][j] = best[g - 1][a] + cost(a, j);
                        from[g][j] = a;
                    }
        if (best[k - 1][n] == inf)
            continue;
        std::vector<std::size_t> split(k, 0);
        std::size_t end = n;
        for (std::size_t g = k; g-- > 1;) {
            split[g] = from[g][end];
            end = split[g];
        }
        bool apart = true;
        for (std::size_t g = 1; g < k; ++g) {
            const std::size_t groupEnd = g + 1 < k ? split[g + 1] : n;
            apart = apart && mean(split[g], groupEnd) - mean(split[g - 1], split[g]) >= kTomMergeOctaves;
        }
        if (apart) {
            starts = split;
            break;
        }
    }
    for (std::size_t g = 0; g < starts.size(); ++g) {
        const std::size_t groupEnd = g + 1 < starts.size() ? starts[g + 1] : n;
        for (std::size_t j = starts[g]; j < groupEnd; ++j)
            group[toms[byPitch[j].second]] = static_cast<int>(g);
    }
    return group;
}

} // namespace

DrumTranscription transcribeDrums(const float* left, const float* right, std::int64_t length, double rate,
                                  const DrumTranscriberParams& params)
{
    DrumTranscription out;
    int size = 256;
    while (size * 2 <= static_cast<int>(rate * 0.064))
        size *= 2; // 1024 at 44.1 / 48 kHz
    const int hop = size / 8;
    out.frameRate = rate / hop;
    // The rise between frames peaks when a hit is about 0.6 of the way into
    // the window (measured on SoundFont renders, whose hit times are exact).
    out.frameOffsetSeconds = 0.6 * size / rate;
    if (length < size)
        return out;

    const Spectrogram spec = bandSpectrogram(left, right, length, rate, size, hop);

    // The top band's power per millisecond (above 7 kHz, 24 dB/octave): fine
    // enough to tell a hat, which keeps sounding for tens of milliseconds,
    // from a kick's beater click, which is gone in a few.
    std::vector<float> topMs;
    {
        const int block = std::max(1, static_cast<int>(rate / 1000.0));
        Biquad a, b;
        a.setCoefficients(BiquadCoefficients::highPass(7000.0, 0.7071, rate));
        b.setCoefficients(BiquadCoefficients::highPass(7000.0, 0.7071, rate));
        topMs.reserve(static_cast<std::size_t>(length / block + 1));
        double sum = 0.0;
        int count = 0;
        for (std::int64_t i = 0; i < length; ++i) {
            const double y = b.process(a.process(0.5 * (left[i] + right[i])));
            sum += y * y;
            if (++count == block) {
                topMs.push_back(static_cast<float>(sum / block));
                sum = 0.0;
                count = 0;
            }
        }
    }
    // Persistence: the top band 20-30 ms after its peak near `seconds`, in dB
    // against the peak.
    const auto persistenceAt = [&](double seconds) {
        const auto n = static_cast<std::ptrdiff_t>(topMs.size());
        const auto at = static_cast<std::ptrdiff_t>(std::lround(seconds * 1000.0));
        std::ptrdiff_t peak = std::clamp<std::ptrdiff_t>(at - 10, 0, n - 1);
        for (std::ptrdiff_t i = std::max<std::ptrdiff_t>(0, at - 10); i < std::min(n, at + 15); ++i)
            if (topMs[static_cast<std::size_t>(i)] > topMs[static_cast<std::size_t>(peak)])
                peak = i;
        double later = 0.0;
        int count = 0;
        for (std::ptrdiff_t i = peak + 20; i < std::min(n, peak + 30); ++i, ++count)
            later += topMs[static_cast<std::size_t>(i)];
        if (count == 0)
            return -100.0;
        return 10.0 * std::log10((later / count + 1e-20) / (topMs[static_cast<std::size_t>(peak)] + 1e-20));
    };
    const int bands = spec.bands, frames = spec.frames;
    const std::vector<double>& hz = spec.centreHz;

    // Onsets: each band's rise from the previous frame.
    std::vector<float> flux(static_cast<std::size_t>(frames) * bands, 0.0f);
    for (int f = 1; f < frames; ++f)
        for (int b = 0; b < bands; ++b)
            flux[static_cast<std::size_t>(f) * bands + b] = std::max(0.0f, spec.at(f, b) - spec.at(f - 1, b));

    // The onset envelope weighs every band equally (by its mean rise), so
    // hats count as much as kicks.
    std::vector<double> bandNorm(static_cast<std::size_t>(bands), 1e-9);
    for (int f = 0; f < frames; ++f)
        for (int b = 0; b < bands; ++b)
            bandNorm[static_cast<std::size_t>(b)] += flux[static_cast<std::size_t>(f) * bands + b];
    double loudestBand = 0.0;
    for (double& n : bandNorm) {
        n /= frames;
        loudestBand = std::max(loudestBand, n);
    }
    // A band that barely moves must not turn its numerical jitter into
    // onsets by dividing it by almost nothing.
    for (double& n : bandNorm)
        n = std::max(n, 1e-3 * loudestBand + 1e-12);
    // Levels below this (80 dB under the song's loudest band) count as silence.
    float loudestLevel = 0.0f;
    for (float v : spec.value)
        loudestLevel = std::max(loudestLevel, v);
    const double levelFloor = 0.01 * loudestLevel + 1e-12;
    out.onsetEnvelope.assign(static_cast<std::size_t>(frames), 0.0f);
    for (int f = 0; f < frames; ++f) {
        double sum = 0.0;
        for (int b = 0; b < bands; ++b)
            sum += flux[static_cast<std::size_t>(f) * bands + b] / bandNorm[static_cast<std::size_t>(b)];
        out.onsetEnvelope[static_cast<std::size_t>(f)] = static_cast<float>(sum / bands);
    }

    // Events: peaks of the envelope above its local median.
    const std::vector<float>& env = out.onsetEnvelope;
    const int minGap = std::max(1, static_cast<int>(std::lround(0.045 * out.frameRate)));
    const int medianRadius = static_cast<int>(std::lround(0.5 * out.frameRate));
    std::vector<int> events;
    {
        std::vector<double> peakValues;
        std::vector<int> peaks;
        for (int f = 1; f + 1 < frames; ++f) {
            if (env[static_cast<std::size_t>(f)] <= 0.0f)
                continue;
            bool isMax = true;
            for (int g = std::max(0, f - minGap); g <= std::min(frames - 1, f + minGap) && isMax; ++g)
                isMax = env[static_cast<std::size_t>(g)] < env[static_cast<std::size_t>(f)]
                        || (env[static_cast<std::size_t>(g)] == env[static_cast<std::size_t>(f)] && g >= f);
            if (isMax) {
                peaks.push_back(f);
                peakValues.push_back(env[static_cast<std::size_t>(f)]);
            }
        }
        const double loud = percentile(peakValues, 0.95);
        std::vector<double> window;
        for (int f : peaks) {
            window.clear();
            for (int g = std::max(0, f - medianRadius); g <= std::min(frames - 1, f + medianRadius); g += 2)
                window.push_back(env[static_cast<std::size_t>(g)]);
            const double local = percentile(window, 0.5);
            if (env[static_cast<std::size_t>(f)] <= 1.5 * local + 0.05 * loud / std::max(0.1f, params.sensitivity))
                continue;
            // A hit raises its bands well above where they were; a held
            // tone's frame-to-frame wobble does not.
            const int before = std::max(0, f - 3);
            const int after = std::min(frames - 1, f + 3);
            double relative = 0.0;
            for (int b = 0; b < bands; ++b) {
                const double rise = std::max(0.0, static_cast<double>(spec.at(after, b)) - spec.at(before, b));
                relative += std::min(1.0, rise / (spec.at(before, b) + levelFloor));
            }
            if (relative / bands >= kMinRelativeRise)
                events.push_back(f);
        }
    }

    // Each event's rise per band, over the frames around its peak, less the
    // band's typical rise (half-wave rectified noise rises a little in
    // every band, which would make small events look flat).
    std::vector<Vec> rises;
    rises.reserve(events.size());
    constexpr int kBefore = 2, kAfter = 3;
    for (int f : events) {
        Vec e(static_cast<std::size_t>(bands), 0.0);
        for (int g = std::max(1, f - kBefore); g <= std::min(frames - 1, f + kAfter); ++g)
            for (int b = 0; b < bands; ++b)
                e[static_cast<std::size_t>(b)] += flux[static_cast<std::size_t>(g) * bands + b];
        for (int b = 0; b < bands; ++b)
            e[static_cast<std::size_t>(b)] = std::max(0.0, e[static_cast<std::size_t>(b)] - (kBefore + kAfter + 1) * bandNorm[static_cast<std::size_t>(b)]);
        rises.push_back(std::move(e));
    }

    // High-band power over time, for each hit's ring (how long it keeps
    // sounding): closed hats stop within tens of milliseconds, open hats and
    // cymbals keep ringing.
    std::vector<float> high(static_cast<std::size_t>(frames), 0.0f);
    for (int f = 0; f < frames; ++f)
        for (int b = 0; b < bands; ++b)
            if (hz[static_cast<std::size_t>(b)] >= kRingFromHz) {
                const float v = spec.at(f, b) * spec.at(f, b);
                high[static_cast<std::size_t>(f)] += v * v; // power
            }
    const auto ringOf = [&](int f, double fromS, double toS) {
        int top = f;
        for (int g = f; g < std::min(frames, f + static_cast<int>(0.03 * out.frameRate) + 1); ++g)
            if (high[static_cast<std::size_t>(g)] > high[static_cast<std::size_t>(top)])
                top = g;
        float m = high[static_cast<std::size_t>(top)];
        for (int g = top + static_cast<int>(fromS * out.frameRate); g <= top + static_cast<int>(toS * out.frameRate) && g < frames; ++g)
            m = std::min(m, high[static_cast<std::size_t>(g)]);
        return 10.0 * std::log10((m + 1e-12) / (high[static_cast<std::size_t>(top)] + 1e-12));
    };
    // The top band's jump at the event: a hat played while a crash rings
    // also seems to ring, but it barely raises the level.
    const auto jumpOf = [&](int f) {
        int top = f;
        for (int g = f; g < std::min(frames, f + static_cast<int>(0.03 * out.frameRate) + 1); ++g)
            if (high[static_cast<std::size_t>(g)] > high[static_cast<std::size_t>(top)])
                top = g;
        const int before = std::max(0, f - static_cast<int>(0.02 * out.frameRate));
        return 10.0 * std::log10((high[static_cast<std::size_t>(top)] + 1e-12) / (high[static_cast<std::size_t>(before)] + 1e-12));
    };
    // Crash candidates keep ringing; the clearest ones (a crash after
    // silence or hats, not re-struck while it still rings) also jump.
    std::vector<double> persistence(events.size());
    for (std::size_t i = 0; i < events.size(); ++i)
        persistence[i] = persistenceAt(events[i] / out.frameRate + out.frameOffsetSeconds);
    std::vector<double> ringSoon(events.size()), ringLater(events.size());
    std::vector<bool> ringsOn(events.size()), crashLike(events.size());
    for (std::size_t i = 0; i < events.size(); ++i) {
        ringSoon[i] = ringOf(events[i], 0.04, 0.10);
        ringLater[i] = ringOf(events[i], 0.10, 0.40);
        ringsOn[i] = ringSoon[i] > kCrashSoonDb && ringLater[i] > kCrashLaterDb;
        crashLike[i] = ringsOn[i] && jumpOf(events[i]) >= kCrashJumpDb;
    }

    // Templates from the song's own hits. Hat first: the top-heavy events
    // that stop quickly (in most grooves some hats play alone). Kick: the
    // shape the low-heavy events share once their hat is taken out (a groove
    // with hats on the eighths puts a hat on every kick, which would
    // otherwise become part of the kick). Snare: what the mid-heavy events
    // keep once kick and hat are taken out. Cymbal: what the long-ringing
    // events keep once their kick is taken out.
    std::array<Vec, kInstruments> w;
    std::array<bool, kInstruments> present{};
    std::vector<double> keys(rises.size());
    for (std::size_t i = 0; i < rises.size(); ++i)
        keys[i] = ringSoon[i] < kOpenRingDb ? regionSum(rises[i], hz, 6000.0, 1e9) / std::max(1e-12, regionSum(rises[i], hz, 0.0, 1e9)) : 0.0;
    w[kHat] = sharedShape(topEvents(rises, keys, 0.3), hz, 6000.0, 1e9, 0.5);
    const double hatTop = std::max(1e-12, regionSum(w[kHat], hz, kHatMeasureHz, 1e9));
    const auto subtract = [&](Vec v, const Vec& shape, double amount) {
        for (int b = 0; b < bands; ++b)
            v[static_cast<std::size_t>(b)] = std::max(0.0, v[static_cast<std::size_t>(b)] - amount * shape[static_cast<std::size_t>(b)]);
        return v;
    };
    std::vector<Vec> withoutHat(rises.size());
    for (std::size_t i = 0; i < rises.size(); ++i) {
        withoutHat[i] = subtract(rises[i], w[kHat], regionSum(rises[i], hz, kHatMeasureHz, 1e9) / hatTop);
        keys[i] = regionSum(rises[i], hz, 0.0, kKickTopHz);
    }
    w[kKick] = sharedShape(topEvents(withoutHat, keys, 0.4), hz, 0.0, kKickTopHz);
    const double kickLow = std::max(1e-12, regionSum(w[kKick], hz, 0.0, kKickTopHz));
    std::vector<Vec> residual(rises.size());
    for (std::size_t i = 0; i < rises.size(); ++i)
        residual[i] = subtract(rises[i], w[kKick], regionSum(rises[i], hz, 0.0, kKickTopHz) / kickLow);
    std::vector<Vec> snareCandidates(rises.size());
    for (std::size_t i = 0; i < rises.size(); ++i) {
        snareCandidates[i] = subtract(residual[i], w[kHat], regionSum(residual[i], hz, kHatMeasureHz, 1e9) / hatTop);
        keys[i] = ringLater[i] < kLongRingDb ? regionSum(snareCandidates[i], hz, 150.0, 1000.0) : 0.0;
    }
    w[kSnare] = sharedShape(topEvents(snareCandidates, keys, 0.3), hz, 150.0, 5000.0);
    int ringing = 0;
    for (std::size_t i = 0; i < rises.size(); ++i) {
        keys[i] = crashLike[i] ? regionSum(residual[i], hz, 1000.0, 1e9) : 0.0;
        ringing += keys[i] > 0.0 ? 1 : 0;
    }
    w[kCymbal] = sharedShape(topEvents(residual, keys, 1.0), hz, 1000.0, 1e9, 0.5);
    for (int c = 0; c < kInstruments; ++c) {
        double sum = 0.0;
        for (double x : w[static_cast<std::size_t>(c)])
            sum += x;
        present[static_cast<std::size_t>(c)] = sum > 0.0;
    }
    present[kCymbal] = present[kCymbal] && ringing >= kMinCymbalEvents;

    // Each event's amount of each instrument: non-negative least squares
    // over six frequency regions, all instruments at once, so a crash's mids
    // are not taken for a snare nor a hat's mids for one. Regions are
    // weighed by their typical rise so the quiet top counts like the low end.
    constexpr std::array<double, kRegions + 1> edges{0.0, 120.0, 300.0, 1000.0, 3000.0, 7000.0, 1e9};
    const auto toRegions = [&](const Vec& v) {
        std::array<double, kRegions> r{};
        for (int k = 0; k < kRegions; ++k)
            r[static_cast<std::size_t>(k)] = regionSum(v, hz, edges[static_cast<std::size_t>(k)], edges[static_cast<std::size_t>(k) + 1]);
        return r;
    };
    std::array<double, kRegions> weight{};
    for (const Vec& e : rises) {
        const auto r = toRegions(e);
        for (int k = 0; k < kRegions; ++k)
            weight[static_cast<std::size_t>(k)] += r[static_cast<std::size_t>(k)];
    }
    for (double& x : weight)
        x = x > 0.0 ? static_cast<double>(rises.size()) / x : 0.0;
    std::array<std::array<double, kRegions>, kInstruments> templ{};
    for (int c = 0; c < kInstruments; ++c) {
        templ[static_cast<std::size_t>(c)] = toRegions(w[static_cast<std::size_t>(c)]);
        for (int k = 0; k < kRegions; ++k)
            templ[static_cast<std::size_t>(c)][static_cast<std::size_t>(k)] *= weight[static_cast<std::size_t>(k)];
    }
    std::vector<std::array<double, kInstruments>> amounts(rises.size());
    for (std::size_t i = 0; i < rises.size(); ++i) {
        auto r = toRegions(rises[i]);
        for (int k = 0; k < kRegions; ++k)
            r[static_cast<std::size_t>(k)] *= weight[static_cast<std::size_t>(k)];
        // Only an event that keeps ringing can hold a cymbal; elsewhere the
        // cymbal's broad shape would take a hat's share.
        auto here = present;
        here[kCymbal] = here[kCymbal] && ringsOn[i];
        amounts[i] = nonNegativeFit(r, templ, here);
    }


    // The loud level of each instrument: its 95th percentile among the
    // events that hold it (for a cymbal, among the clear crashes).
    std::array<double, kInstruments> loud{};
    for (int c = 0; c < kInstruments; ++c) {
        std::vector<double> values;
        for (std::size_t i = 0; i < amounts.size(); ++i)
            if (amounts[i][static_cast<std::size_t>(c)] > 0.0 && (c != kCymbal || crashLike[i]))
                values.push_back(amounts[i][static_cast<std::size_t>(c)]);
        loud[static_cast<std::size_t>(c)] = percentile(values, 0.95);
    }
    constexpr std::array<double, kInstruments> kFloor{0.3, 0.4, 0.2, 0.3}; // of the instrument's loud hits
    const auto floorOf = [&](int c) {
        return kFloor[static_cast<std::size_t>(c)] * loud[static_cast<std::size_t>(c)] / std::max(0.1f, params.sensitivity);
    };

    // Toms: a tuned drum whose tone keeps ringing long after the song's kicks
    // have stopped, without a snare's wires. The fit above takes them for
    // kicks or snares; finer spectra at each hit tell them apart.
    const std::vector<int> tomGroup = findToms(left, right, length, rate, events, out.frameRate, out.frameOffsetSeconds,
                                               amounts, loud, floorOf(kKick), floorOf(kSnare));
    int tomGroups = 0;
    for (int g : tomGroup)
        tomGroups = std::max(tomGroups, g + 1);
    std::vector<double> tomValues;
    for (std::size_t i = 0; i < events.size(); ++i)
        if (tomGroup[i] >= 0)
            tomValues.push_back(amounts[i][kKick] + amounts[i][kSnare]);
    const double tomLoud = percentile(tomValues, 0.95);

    for (int c = 0; c < kInstruments; ++c) {
        if (!present[static_cast<std::size_t>(c)])
            continue;
        const double floor = floorOf(c);
        for (std::size_t i = 0; i < events.size(); ++i) {
            const double amount = amounts[i][static_cast<std::size_t>(c)];
            if (amount < floor || amount <= 0.0)
                continue;
            // A cymbal keeps ringing: only ringing events count as cymbal hits.
            if (c == kCymbal && !ringsOn[i])
                continue;
            // A tom's low end and body are the tom, not a kick or a snare.
            if (tomGroup[i] >= 0 && (c == kKick || c == kSnare))
                continue;
            DrumHit hit;
            hit.seconds = events[i] / out.frameRate + out.frameOffsetSeconds;
            hit.strength = static_cast<float>(amount / std::max(loud[static_cast<std::size_t>(c)], 1e-12));
            switch (c) {
            case kKick:
                hit.kind = DrumKind::Kick;
                break;
            case kSnare:
                hit.kind = DrumKind::Snare;
                break;
            case kHat:
                hit.kind = ringSoon[i] < kOpenRingDb ? DrumKind::ClosedHat : DrumKind::OpenHat;
                break;
            default:
                hit.kind = DrumKind::Crash;
                break;
            }
            out.hits.push_back(hit);
        }
    }
    // Tom groups from low to high: one is a low tom, two a floor and a high
    // tom, three all of them.
    static constexpr DrumKind kOne[] = {DrumKind::LowTom};
    static constexpr DrumKind kTwo[] = {DrumKind::FloorTom, DrumKind::HighTom};
    static constexpr DrumKind kThree[] = {DrumKind::FloorTom, DrumKind::LowTom, DrumKind::HighTom};
    const DrumKind* kinds = tomGroups == 1 ? kOne : tomGroups == 2 ? kTwo : kThree;
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (tomGroup[i] < 0)
            continue;
        DrumHit hit;
        hit.seconds = events[i] / out.frameRate + out.frameOffsetSeconds;
        hit.kind = kinds[tomGroup[i]];
        hit.strength = static_cast<float>((amounts[i][kKick] + amounts[i][kSnare]) / std::max(tomLoud, 1e-12));
        out.hits.push_back(hit);
    }
    std::sort(out.hits.begin(), out.hits.end(), [](const DrumHit& a, const DrumHit& b) { return a.seconds < b.seconds; });
    return out;
}

} // namespace daw
