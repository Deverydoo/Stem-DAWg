// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors

#include "SyntheticDrums.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace daw::test {

namespace {

class Noise {
public:
    explicit Noise(std::uint32_t seed) : state_(seed) {}
    float next()
    {
        state_ = state_ * 1664525u + 1013904223u;
        return static_cast<float>(static_cast<double>(state_ >> 8) / 8388608.0 - 1.0);
    }

private:
    std::uint32_t state_;
};

void addKick(std::vector<float>& out, std::int64_t at, double rate, float gain, Noise& noise)
{
    double phase = 0.0;
    const auto length = static_cast<std::int64_t>(0.5 * rate);
    for (std::int64_t i = 0; i < length && at + i < static_cast<std::int64_t>(out.size()); ++i) {
        const double t = i / rate;
        phase += 2.0 * std::numbers::pi * (50.0 + 100.0 * std::exp(-t / 0.015)) / rate;
        double v = 0.9 * std::sin(phase) * std::exp(-t / 0.12);
        if (t < 0.002)
            v += 0.3 * noise.next();
        out[static_cast<std::size_t>(at + i)] += static_cast<float>(gain * v);
    }
}

void addSnare(std::vector<float>& out, std::int64_t at, double rate, float gain, Noise& noise)
{
    const auto length = static_cast<std::int64_t>(0.4 * rate);
    float low = 0.0f, previous = 0.0f;
    for (std::int64_t i = 0; i < length && at + i < static_cast<std::int64_t>(out.size()); ++i) {
        const double t = i / rate;
        // Noise high-passed (first difference) then gently low-passed: 1-8 kHz.
        const float n = noise.next();
        const float high = n - previous;
        previous = n;
        low += 0.5f * (high - low);
        const double v = 0.5 * low * std::exp(-t / 0.09) + 0.4 * std::sin(2.0 * std::numbers::pi * 190.0 * t) * std::exp(-t / 0.05);
        out[static_cast<std::size_t>(at + i)] += static_cast<float>(gain * v);
    }
}

void addHat(std::vector<float>& out, std::int64_t at, std::int64_t until, double rate, float gain, double decay, double brightness,
            Noise& noise)
{
    const std::int64_t length = std::min<std::int64_t>(until - at, static_cast<std::int64_t>(8.0 * decay * rate));
    float previous = 0.0f, previous2 = 0.0f;
    for (std::int64_t i = 0; i < length && at + i < static_cast<std::int64_t>(out.size()); ++i) {
        const double t = i / rate;
        // Second difference: rises 12 dB per octave, so the energy sits high.
        const float n = noise.next();
        const float d2 = n - 2.0f * previous + previous2;
        previous2 = previous;
        previous = n;
        const double v = (brightness * d2 + (1.0 - brightness) * n) * std::exp(-t / decay);
        out[static_cast<std::size_t>(at + i)] += static_cast<float>(gain * v);
    }
}

} // namespace

DrumGroove makeDrumGroove(int bars, double startBpm, double endBpm, double rate, double startSeconds)
{
    DrumGroove g;
    // Beat times with a linearly moving tempo.
    const int totalBeats = bars * 4;
    double t = startSeconds;
    for (int b = 0; b <= totalBeats; ++b) {
        g.beats.push_back(t);
        const double bpm = startBpm + (endBpm - startBpm) * b / std::max(1, totalBeats);
        t += 60.0 / bpm;
    }
    const double end = g.beats.back() + 2.0;
    g.beats.pop_back();
    for (int bar = 0; bar < bars; ++bar)
        g.downbeats.push_back(g.beats[static_cast<std::size_t>(bar) * 4]);

    // The pattern, at eighth-note positions between beats.
    const auto at = [&](int beat, double fraction) {
        const double b0 = g.beats[static_cast<std::size_t>(beat)];
        const double b1 = beat + 1 < totalBeats ? g.beats[static_cast<std::size_t>(beat) + 1] : b0 + (b0 - g.beats[static_cast<std::size_t>(beat) - 1]);
        return b0 + fraction * (b1 - b0);
    };
    for (int bar = 0; bar < bars; ++bar) {
        const int b = bar * 4;
        if (bar % 8 == 0)
            g.hits.push_back({at(b, 0.0), kTestCrash});
        g.hits.push_back({at(b, 0.0), kTestKick});
        g.hits.push_back({at(b + 2, 0.0), kTestKick});
        g.hits.push_back({at(b + 2, 0.5), kTestKick});
        g.hits.push_back({at(b + 1, 0.0), kTestSnare});
        g.hits.push_back({at(b + 3, 0.0), kTestSnare});
        for (int eighth = 0; eighth < 8; ++eighth)
            g.hits.push_back({at(b + eighth / 2, (eighth % 2) * 0.5), eighth == 7 ? kTestOpenHat : kTestClosedHat});
    }
    std::stable_sort(g.hits.begin(), g.hits.end(), [](const TestHit& a, const TestHit& b) { return a.seconds < b.seconds; });

    std::vector<float> mono(static_cast<std::size_t>(end * rate), 0.0f);
    Noise noise(7);
    for (std::size_t i = 0; i < g.hits.size(); ++i) {
        const TestHit& h = g.hits[i];
        const auto start = static_cast<std::int64_t>(std::llround(h.seconds * rate));
        // A little variation in level, as a drummer would play.
        const float gain = 0.85f + 0.15f * static_cast<float>((i * 7919) % 11) / 10.0f;
        switch (h.code) {
        case kTestKick:
            addKick(mono, start, rate, gain, noise);
            break;
        case kTestSnare:
            addSnare(mono, start, rate, gain, noise);
            break;
        case kTestClosedHat:
            addHat(mono, start, start + static_cast<std::int64_t>(rate), rate, 0.25f * gain, 0.02, 0.8, noise);
            break;
        case kTestOpenHat: {
            // Rings until the next hat chokes it.
            std::int64_t until = static_cast<std::int64_t>(mono.size());
            for (std::size_t j = i + 1; j < g.hits.size(); ++j)
                if (g.hits[j].code == kTestClosedHat || g.hits[j].code == kTestOpenHat) {
                    until = static_cast<std::int64_t>(std::llround(g.hits[j].seconds * rate));
                    break;
                }
            addHat(mono, start, until, rate, 0.25f * gain, 0.25, 0.8, noise);
            break;
        }
        default:
            addHat(mono, start, static_cast<std::int64_t>(mono.size()), rate, 0.3f * gain, 1.2, 0.4, noise);
            break;
        }
    }
    g.audio = AudioAsset::create(2, static_cast<std::int64_t>(mono.size()), rate);
    for (int c = 0; c < 2; ++c)
        std::copy(mono.begin(), mono.end(), g.audio->channels[static_cast<std::size_t>(c)].begin());
    return g;
}

} // namespace daw::test
