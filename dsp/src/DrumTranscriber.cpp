// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include <daw/dsp/DrumTranscriber.h>

#include <daw/dsp/Biquad.h>
#include <daw/dsp/Fft.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
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


    constexpr std::array<double, kInstruments> kFloor{0.3, 0.4, 0.2, 0.3}; // of the instrument's loud hits
    for (int c = 0; c < kInstruments; ++c) {
        if (!present[static_cast<std::size_t>(c)])
            continue;
        // A cymbal keeps ringing: only ringing events count as cymbal hits,
        // and the loud level is taken among the clear crashes.
        const auto eligible = [&](std::size_t i) { return c != kCymbal || ringsOn[i]; };
        std::vector<double> values;
        for (std::size_t i = 0; i < amounts.size(); ++i)
            if (amounts[i][static_cast<std::size_t>(c)] > 0.0 && (c != kCymbal || crashLike[i]))
                values.push_back(amounts[i][static_cast<std::size_t>(c)]);
        const double loud = percentile(values, 0.95);
        const double floor = kFloor[static_cast<std::size_t>(c)] * loud / std::max(0.1f, params.sensitivity);
        for (std::size_t i = 0; i < events.size(); ++i) {
            const double amount = amounts[i][static_cast<std::size_t>(c)];
            if (amount < floor || amount <= 0.0)
                continue;
            if (!eligible(i))
                continue;
            DrumHit hit;
            hit.seconds = events[i] / out.frameRate + out.frameOffsetSeconds;
            hit.strength = static_cast<float>(amount / std::max(loud, 1e-12));
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
    std::sort(out.hits.begin(), out.hits.end(), [](const DrumHit& a, const DrumHit& b) { return a.seconds < b.seconds; });
    return out;
}

} // namespace daw
